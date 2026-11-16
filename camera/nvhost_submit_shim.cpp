/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdlib.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>

/* readlink sin arrastrar <unistd.h>: unistd.h activa las dos declaraciones
 * __overloadable de ioctl() de bionic (unsigned / unsigned long), que chocan
 * con nuestra definicion de ioctl. */
extern "C" ssize_t readlink(const char* __path, char* __buf, size_t __bufsize);
extern "C" int close(int __fd);
/* pread/open para la copia previa de argumentos (ver traceNvmapCmd).  Declarados a mano
 * por el mismo motivo que readlink/close: no queremos arrastrar <unistd.h>. */
extern "C" ssize_t pread(int __fd, void* __buf, size_t __count, off_t __offset);
extern "C" int open(const char* __path, int __flags, ...);
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <android/log.h>

/*
 * ABI shim for the stock Tegra124 camera blobs (camera.vendor.tegra.so,
 * libnvmm_camera_v3.so, libnvrm...): they issue the host1x channel SUBMIT
 * ioctl built against the stock 3.10 layout of struct nvhost_submit_args,
 * which was 120 bytes long:
 *
 *     6 x u32 counters, u32 syncpt_incrs (32 bit pointer), u32 fence (out),
 *     u64 cmdbuf_exts, u32 flags, u32 reserved, u64 pad[2],
 *     7 x u64 (cmdbufs, relocs, reloc_shifts, waitchks, waitbases,
 *              class_ids, fences)                                 -> 120 bytes
 *
 * Our kernel carries the upstream-ish commit ea5c4541fe8
 * ("video: tegra: host: Fix submit ioctl signature"), which reordered the
 * same fields into a 128 byte struct (u32 flags moved before u32 fence,
 * syncpt_incrs became u64, pad[3]) -> _IOWR('H', 26, 128).
 *
 * bus_client.c dispatches on the *whole* ioctl number (switch (cmd)), so the
 * blobs' 120 byte variant falls through to "default: -ENOTTY". Measured on
 * the tablet: 1189 ENOTTY in eight seconds, every single submit.
 *
 * The blobs then retry with NVHOST32_IOCTL_CHANNEL_SUBMIT, whose struct has
 * no 'flags' member at all, so the kernel silently drops
 * NVHOST_SUBMIT_FLAG_SYNC_FENCE_FD. nvhost_ioctl_channel_submit() therefore
 * never calls nvhost_sync_create_fence_fd() and stores a raw syncpoint
 * number - or plain 0 when more than one syncpoint is used - in 'fence'
 * instead of a sync fence fd. The HAL wraps that value in android::Fence:
 * every capture request blocked for its full 2000 ms timeout (measured:
 * "process_capture_request -> 0 in 2005 ms", ~0.5 fps preview, preview
 * starting dark because 3A only iterated twice per second), and its
 * destructor closed fd 0, which tripped fdsan ("attempted to close file
 * descriptor 0, expected to be unowned") in the camera provider.
 *
 * Translating the legacy request to the current one keeps 'flags' alive, so
 * the kernel hands back a real sync fence fd, exactly like on the Android 10
 * tree whose kernel still has the 120 byte layout.
 *
 * Only that single command number is rewritten; every other ioctl is passed
 * through untouched. This library is the first entry of the LD_PRELOAD list
 * of android.hardware.camera.provider@2.4-service (see
 * patches/hardware/interfaces/0005-...-preload-mocha-abi-shims.patch), which
 * puts it in the linker's shared group, the only scope
 * camera.vendor.tegra.so / libnvos.so look into.
 */

namespace {

constexpr unsigned kNvhostMagic = 'H';
constexpr unsigned kSubmitCmd = 26;
constexpr unsigned kLegacySize = 120;

struct SubmitLegacy {
    uint32_t submit_version;
    uint32_t num_syncpt_incrs;
    uint32_t num_cmdbufs;
    uint32_t num_relocs;
    uint32_t num_waitchks;
    uint32_t timeout;
    uint32_t syncpt_incrs;   /* 32 bit pointer to struct nvhost_syncpt_incr[] */
    uint32_t fence;          /* out */
    uint64_t cmdbuf_exts;
    uint32_t flags;
    uint32_t reserved;
    uint64_t pad[2];
    uint64_t cmdbufs;
    uint64_t relocs;
    uint64_t reloc_shifts;
    uint64_t waitchks;
    uint64_t waitbases;
    uint64_t class_ids;
    uint64_t fences;
};

struct SubmitCurrent {
    uint32_t submit_version;
    uint32_t num_syncpt_incrs;
    uint32_t num_cmdbufs;
    uint32_t num_relocs;
    uint32_t num_waitchks;
    uint32_t timeout;
    uint32_t flags;
    uint32_t fence;          /* out */
    uint64_t syncpt_incrs;
    uint64_t cmdbuf_exts;
    uint64_t pad[3];
    uint64_t cmdbufs;
    uint64_t relocs;
    uint64_t reloc_shifts;
    uint64_t waitchks;
    uint64_t waitbases;
    uint64_t class_ids;
    uint64_t fences;
};

static_assert(sizeof(SubmitLegacy) == kLegacySize,
              "the stock 3.10 submit ABI must stay 120 bytes");
static_assert(sizeof(SubmitCurrent) == 128,
              "our kernel's submit ABI is 128 bytes (NVHOST_IOCTL_CHANNEL_MAX_ARG_SIZE)");

/* asm-generic/ioctl.h number layout, spelled out so that this file needs no
 * libc prototype at all (bionic declares ioctl() twice with __overloadable,
 * which would conflict with our definition). */
constexpr unsigned kIocDirRW = 3;   /* read | write */
constexpr unsigned kIocDirShift = 30;
constexpr unsigned kIocSizeShift = 16;
constexpr unsigned kIocTypeShift = 8;

/* include/linux/nvhost_ioctl.h: '#define NVHOST_SUBMIT_FLAG_SYNC_FENCE_FD 0',
 * y bus_client.c hace 'if (args->flags & BIT(NVHOST_SUBMIT_FLAG_SYNC_FENCE_FD))'. */
constexpr unsigned kFlagSyncFenceFd = 0;

constexpr unsigned legacyRequest(unsigned size) {
    return (kIocDirRW << kIocDirShift) | (size << kIocSizeShift) |
           (kNvhostMagic << kIocTypeShift) | kSubmitCmd;
}

/* _IOWR('H', 26, struct nvhost_submit_args) as our kernel computes it. */
constexpr unsigned kCurrentRequest = legacyRequest(sizeof(SubmitCurrent));
/* What strace measured coming out of libnvrm: 0xc078481a. */
constexpr unsigned kLegacyRequest = legacyRequest(kLegacySize);

static_assert(kLegacyRequest == 0xc078481au,
              "the blobs ask for _IOC(RW,'H',26,120) = 0xc078481a");
static_assert(kCurrentRequest == 0xc080481au,
              "our kernel only knows _IOC(RW,'H',26,128) = 0xc080481a");

typedef int (*IoctlFn)(int, unsigned long, ...);

struct SyncptIncr {   /* struct nvhost_syncpt_incr in the kernel */
    uint32_t syncpt_id;
    uint32_t incrs;
};

/* ---- trazado del lado de ESPERA de syncpoint -------------------------------
 * Medido con el kernel #19 (handoff, 14 s): 2.500 fences del submit entregados,
 * 2.489 senalizados por el kernel (media 32 ms, max 95 ms) y el flood 'Waiting on
 * the syncpt semaphore failed' se queda en 111.656 lineas - 113.276 con el kernel
 * #16 y 101.561 con 'close'.  El flood NO depende de que el fence del submit se
 * entregue, se cierre o se ceda.  Lo que falla es otro camino: las esperas de
 * syncpoint, que hasta ahora pasaban por aqui sin decodificar.
 *
 * NVHOST_IOCTL_MAGIC 'H' reutiliza numeros entre el nodo de control y los canales,
 * asi que el numero solo se interpreta sabiendo QUE nodo es el fd:
 *   /dev/nvhost-ctrl : 1 READ, 3 WAIT, 6 WAITEX, 8 READ_MAX, 9 WAITMEX,
 *                      10 FENCE_CREATE (32 bit), 11 FENCE_CREATE (64 bit)
 *   canales          : 2 GET_SYNCPOINTS, 3 GET_WAITBASES, 16 GET_SYNCPOINT,
 *                      17 GET_WAITBASE, 15/26 SUBMIT
 * Solo decodificamos cuando el tamano codificado en el ioctl cubre la estructura
 * nuestra: si el blob pasa una version mas antigua, no leemos fuera de su buffer.
 */
struct CtrlRead    { uint32_t id, value; };
struct CtrlWait    { uint32_t id, thresh; int32_t timeout; };
struct CtrlWaitex  { uint32_t id, thresh; int32_t timeout; uint32_t value; };
struct CtrlWaitmex { uint32_t id, thresh; int32_t timeout; uint32_t value;
                     uint32_t tv_sec, tv_nsec, reserved_1, reserved_2; };
struct CtrlFence32 { uint32_t num_pts, pad; uint64_t pts, name; int32_t fence_fd; };
struct SyncptInfo  { uint32_t id, thresh; };
struct ChanParam   { uint32_t param, value; };

constexpr int kWaitSlots = 40;
struct WaitStat {
    int nr;
    uint32_t key;        /* syncpt (esperas) o param (asignaciones de canal) */
    uint32_t lo, hi;     /* umbral pedido, o valor leido en READ/READ_MAX */
    uint32_t n, fails;
    uint32_t fences;     /* fences creados sobre este syncpt */
    int32_t lastErr;
    int fd;
};
WaitStat gWaitStats[kWaitSlots];
int gWaitStatCount = 0;
unsigned gWaitTotal = 0;
unsigned gWaitLogged = 0;
unsigned gFenceCreateLogged = 0;

constexpr uint32_t kNoKey = 0xffffffffu;

IoctlFn gRealIoctl = nullptr;
unsigned gTranslations = 0;
int gFirstFence = -1;

/*
 * El blob llama SIEMPRE con flags=0x0 medido (1163 submits de una sesion, todos
 * flags=0x0): su ABI de 2015 no tiene forma de pedir un fence, y en el kernel de
 * 2015 el ioctl lo creaba de fabrica.  Nuestro kernel (3.10.108 reconstruido) hace
 *
 *   if (flags & BIT(NVHOST_SUBMIT_FLAG_SYNC_FENCE_FD))  -> fence = fd de sync real
 *   else if (num_syncpt_incrs == 1)                     -> fence = Umbral del syncpoint
 *   else                                                -> fence = 0
 *
 * y el blob mete ese entero en android::Fence como si fuera un fd.  Eso explica
 * todo lo medido: Fence(fd 1002), Fence(fd 0), el poll sobre un pipe (fd 1) que
 * tragaba 2000 ms por peticion, el 'fdsan: attempted to close file descriptor 0'
 * y las 25127 lineas de NvOsDebugPrintf 'Waiting on the syncpt semaphore failed
 * 0x2' (ENOENT: ese "fence" no es un objeto de sync) en 10 s.
 *
 * Por eso el shim ENCIENDE el flag.  Se puede apagar con MOCHA_CAMERA_SYNCFD=0 en
 * el entorno del provider para comparar.
 */
/*
 * Politicas posibles para el fd de sync fence que devuelve el kernel, seleccionables
 * sin recompilar (el provider lee el entorno y, si no esta, /data/local/tmp/mocha-syncfd-mode):
 *
 *   handoff (defecto)  entregar al blob el fd real que da el kernel, y cerrarlo nosotros
 *                     solo cuando el kernel ya lo senaliz6 (hilo recolector, max 200 ms).
 *                     Es lo que espera el pipeline de 2015: el kernel senaliza ese fence en
 *                     0..205 ms (sondeo del 2026-11-01 22:09) y ademas no se fuga el fd.
 *   close            cerrar el fd justo despues del submit y entregar 0 al blob.  Se
 *                     conserva como control: con fd 0 tambien salen frames, pero el blob
 *                     no recibe la senal y inunda el log de esperas fallidas.
 *   keep              entregar el fd real y no cerrarlo (fuga de fds: solo para medir).
 *   reap              entregar el fd real y cerrarlo cuando el kernel lo senala, desde un
 *                     hilo propio con poll().  Es la politica que queremos si el pipeline de
 *                     nvmm_camera usa ese fd para senalar sus semaforos de frame.
 *   0 / noflag        no encender NVHOST_SUBMIT_FLAG_SYNC_FENCE_FD (vuelve al comportamiento
 *                     de umbral-entero que causaba los 2000 ms de 'Fence::wait').
 *
 * Medido 2026-11-01 14:10 con la politica 'close': 5.348 submits, 126.500 fences cerrados, 0
 * 'Fence timed out' y 701 constructCaptureResult, pero ~455.000 esperas de semaforo fallidas
 * ('Waiting on the syncpt semaphore failed 0x2' vive en libnvmm_camera/libnvmm_camera_v3, que
 * importa NvOsSemaphoreWaitTimeout y tambien NvRmFenceSignalSemaphore).  Si el blob registra el
 * fence del submit para senalar sus semaforos, entregarle 0 (un fd invalido para registrar)
 * explicaria que nunca reciba la senal y inunde el log.  'reap' mantiene el fd valido sin fugarse.
 */
constexpr int kMaxOpenFences = 200;
/* 2026-11-01 22:09, medido con el sondeo pasivo: el kernel senaliza el fence del submit
 * de camara a los 0..205 ms (media 77 ms).  Con 200 ms el fd del blob sigue siendo valido
 * mientras el blob lo necesita y encima lo entregamos ya senalizado.  El valor anterior
 * (1500 ms) hacia aun peor el 'reap': llegabamos tarde a cerrar. */
constexpr int kFenceMaxAgeMs = 200;

/* struct pollfd y poll() declarados a mano: <poll.h> no esta incluido a proposito (ver el
 * comentario del readlink).  El layout es el del kernel/bionic. */
struct pollfd {
    int fd;
    short events;
    short revents;
};
extern "C" int poll(struct pollfd* __array, unsigned long __nfds, int __timeout);

struct OpenFence {
    int fd;
    unsigned long long born_ms;
};

OpenFence gOpenFences[kMaxOpenFences];
int gOpenFenceCount = 0;
pthread_mutex_t gOpenLock = PTHREAD_MUTEX_INITIALIZER;
unsigned gReaperSignaled = 0;
unsigned gReaperAged = 0;
bool gReaperStarted = false;

static unsigned long long nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
}

static void fenceReapAdd(int fd)
{
    pthread_mutex_lock(&gOpenLock);
    if (gOpenFenceCount >= kMaxOpenFences) {
        /* el mas viejo primero: si el pipeline va mas rapido que el reaper, no debemos
         * acumular descriptores */
        close(gOpenFences[0].fd);
        gReaperAged++;
        memmove(&gOpenFences[0], &gOpenFences[1],
                (size_t)(gOpenFenceCount - 1) * sizeof(OpenFence));
        gOpenFenceCount--;
    }
    gOpenFences[gOpenFenceCount].fd = fd;
    gOpenFences[gOpenFenceCount].born_ms = nowMs();
    gOpenFenceCount++;
    pthread_mutex_unlock(&gOpenLock);
}

static void* fenceReaperThread(void*)
{
    struct pollfd pf[kMaxOpenFences];
    int idx[kMaxOpenFences];

    for (;;) {
        int n = 0;
        unsigned long long now;
        int i, k;

        pthread_mutex_lock(&gOpenLock);
        now = nowMs();
        for (i = 0; i < gOpenFenceCount; i++) {
            if (now - gOpenFences[i].born_ms > (unsigned long long)kFenceMaxAgeMs) {
                close(gOpenFences[i].fd);
                gReaperAged++;
                gOpenFences[i] = gOpenFences[--gOpenFenceCount];
                i--;
                continue;
            }
            pf[n].fd = gOpenFences[i].fd;
            /* BUG ENCONTRADO 2026-11-01 21:43: con events = 0 no se pide NINGUN evento, o sea
             * que poll() nunca devuelve revents y el recolector jamas veia un fence senalizado
             * ('reap: 0 fences senalados').  Hay que pedir POLLIN (0x0001). */
            pf[n].events = 0x0001;      /* POLLIN */
            pf[n].revents = 0;
            idx[n] = i;
            n++;
        }
        pthread_mutex_unlock(&gOpenLock);

        if (n == 0) {
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, nullptr);
            continue;
        }

        poll(pf, (unsigned long)n, 200);

        pthread_mutex_lock(&gOpenLock);
        for (k = 0; k < n; k++) {
            int pos;
            if (pf[k].revents == 0)
                continue;
            pos = idx[k];
            if (pos >= gOpenFenceCount || gOpenFences[pos].fd != pf[k].fd)
                continue;   /* ya lo cerro otra ruta */
            close(gOpenFences[pos].fd);
            gReaperSignaled++;
            gOpenFences[pos] = gOpenFences[--gOpenFenceCount];
            if ((gReaperSignaled % 500u) == 0u)
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "reap: %u fences senalados y cerrados (%u por antiguedad, %u abiertos)",
                        gReaperSignaled, gReaperAged, gOpenFenceCount);
        }
        pthread_mutex_unlock(&gOpenLock);
    }
    return nullptr;
}

static void fenceReapStart()
{
    pthread_attr_t attr;
    pthread_t th;

    if (gReaperStarted)
        return;
    gReaperStarted = true;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, fenceReaperThread, nullptr) != 0)
        __android_log_print(ANDROID_LOG_WARN, "mocha-hal30",
                "reap: no se pudo crear el hilo recolector (errno %d)", errno);
    pthread_attr_destroy(&attr);
}

unsigned gExtraFlags = 0xffffffffu;
int gCloseFences = -1;          /* 1 cerrar, 0 entregar el fd real */
int gReapFences = 0;
unsigned gFencesClosed = 0;
unsigned gFencesKept = 0;
unsigned gFencesReaped = 0;

static const char* fenceModeName()
{
    static const char* mode = nullptr;
    if (mode == nullptr) {
        mode = getenv("MOCHA_CAMERA_SYNCFD");
        if (mode == nullptr || mode[0] == '\0') {
            FILE* f = fopen("/data/local/tmp/mocha-syncfd-mode", "r");
            static char buf[32];
            if (f != nullptr) {
                if (fgets(buf, sizeof(buf), f) != nullptr) {
                    size_t len = strlen(buf);
                    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                        buf[--len] = '\0';
                    mode = buf;
                }
                fclose(f);
            }
        }
        if (mode == nullptr || mode[0] == '\0')
            mode = "handoff";   /* antes 'close': el sondeo mostro que el blob si necesita el fd */
    }
    return mode;
}

/* ============================================================================
 * SONDEO PASIVO DE LOS FENCES DEL SUBMIT (es medida, no politica)
 *
 * Medido 2026-11-01 22:09 con este propio sondeo, y es el numero que resuelve la duda:
 *
 *     sondeo: 2400/1612 fences senalizados (media 77 ms, max 205 ms, 6 nunca senalizados)
 *
 * El kernel SI senaliza el sync fence del submit de camara, y tarda entre 0 y 205 ms
 * (media ~77 ms con ~200 submits/s de preview a 1920x1080).  O sea que el flood
 * 'Waiting on the syncpt semaphore failed' (101.561 esperas en los mismos 18 s) NO es
 * un syncpt que nunca llega: es que el blob no ve esa senal.  En modo 'close' le damos
 * fd 0, y en 'reap' el recolector puede cerrarlo antes de que venza su espera; en los
 * dos casos el blob espera una senal que no va a llegar y reintenta en bucle.
 *
 * Dum duplicamos el fd que devuelve el kernel y lo observamos desde un hilo propio
 * con poll(POLLIN).  Usamos un dup y no el fd del blob por dos motivos: no interferimos
 * con nada que haga el blob con ese descriptor, y el dup mantiene viva la estructura
 * interna del sync_file mientras nosotros la medimos.
 * Cada fence sondeado acaba clasificado como:
 *      senalizado en X ms   o   nunca senalizado dentro de kProbeMaxAgeMs
 * ============================================================================ */
extern "C" int dup(int __fd);

constexpr int kProbeSlots = 24;
constexpr int kProbeMaxAgeMs = 300;

struct ProbeSlot {
    int dupfd;
    unsigned long long born_ms;
    unsigned long long signed_ms;   /* 0 = todavia no se senalizo */
};

ProbeSlot gProbe[kProbeSlots];
int gProbeCount = 0;
bool gProbeStarted = false;
pthread_mutex_t gProbeLock = PTHREAD_MUTEX_INITIALIZER;
unsigned gProbeSeen = 0, gProbeSigned = 0, gProbeLost = 0, gProbeBad = 0;
unsigned long long gProbeLatSum = 0, gProbeLatMax = 0;

static void fenceProbeAdd(int fd)
{
    int d = dup(fd);
    if (d < 0) {
        gProbeBad++;
        return;
    }
    pthread_mutex_lock(&gProbeLock);
    if (gProbeCount >= kProbeSlots) {
        /* hueco lleno: clasificamos y soltamos el mas viejo */
        ProbeSlot* old = &gProbe[0];
        if (old->signed_ms == 0)
            gProbeLost++;
        close(old->dupfd);
        memmove(&gProbe[0], &gProbe[1],
                (size_t)(gProbeCount - 1) * sizeof(ProbeSlot));
        gProbeCount--;
    }
    gProbe[gProbeCount].dupfd = d;
    gProbe[gProbeCount].born_ms = nowMs();
    gProbe[gProbeCount].signed_ms = 0;
    gProbeCount++;
    gProbeSeen++;
    pthread_mutex_unlock(&gProbeLock);
}

static void* fenceProbeThread(void*)
{
    struct pollfd pf[kProbeSlots];
    int idx[kProbeSlots];

    for (;;) {
        int n = 0, i, k;
        unsigned long long now;

        pthread_mutex_lock(&gProbeLock);
        now = nowMs();
        for (i = 0; i < gProbeCount; i++) {
            if (now - gProbe[i].born_ms > (unsigned long long)kProbeMaxAgeMs) {
                if (gProbe[i].signed_ms == 0)
                    gProbeLost++;
                close(gProbe[i].dupfd);
                gProbe[i] = gProbe[--gProbeCount];
                i--;
                continue;
            }
            pf[n].fd = gProbe[i].dupfd;
            pf[n].events = 0x0001;      /* POLLIN */
            pf[n].revents = 0;
            idx[n] = i;
            n++;
        }
        pthread_mutex_unlock(&gProbeLock);

        if (n == 0) {
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, nullptr);
            continue;
        }

        poll(pf, (unsigned long)n, 50);

        pthread_mutex_lock(&gProbeLock);
        for (k = 0; k < n; k++) {
            int pos;
            unsigned long long t;
            if ((pf[k].revents & 0x0001) == 0)
                continue;
            pos = idx[k];
            if (pos >= gProbeCount || gProbe[pos].dupfd != pf[k].fd)
                continue;               /* otra pasada ya lo clasifico */
            if (gProbe[pos].signed_ms != 0)
                continue;
            t = nowMs() - gProbe[pos].born_ms;
            gProbe[pos].signed_ms = t;
            gProbeSigned++;
            gProbeLatSum += t;
            if (t > gProbeLatMax)
                gProbeLatMax = t;
            if ((gProbeSigned % 200u) == 0u)
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "sondeo: %u/%u fences senalizados (media %llu ms, max %llu ms, "
                        "%u nunca senalizados, %u dups fallidos)",
                        gProbeSigned, gProbeSeen,
                        gProbeLatSum / (unsigned long long)gProbeSigned,
                        gProbeLatMax, gProbeLost, gProbeBad);
        }
        pthread_mutex_unlock(&gProbeLock);
    }
    return nullptr;
}

static void fenceProbeStart()
{
    pthread_attr_t attr;
    pthread_t th;

    if (gProbeStarted)
        return;
    gProbeStarted = true;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, fenceProbeThread, nullptr) != 0)
        __android_log_print(ANDROID_LOG_WARN, "mocha-hal30",
                "sondeo: no se pudo crear el hilo recolector (errno %d)", errno);
    pthread_attr_destroy(&attr);
}

static unsigned extraFlags() {
    if (gExtraFlags == 0xffffffffu) {
        const char* mode = fenceModeName();
        if (mode[0] == 'h') {                      /* handoff (defecto) */
            gCloseFences = 0;
            gReapFences = 1;          /* el blob se queda el fd; lo cerramos ya senalizado */
            fenceReapStart();
        } else if (mode[0] == 'k') {               /* keep: entregar y no cerrar (mide fugas) */
            gCloseFences = 0;
            gReapFences = 0;
        } else if (mode[0] == 'r') {                      /* reap */
            gCloseFences = 0;
            gReapFences = 1;
            fenceReapStart();
        } else if (mode[0] == '0' || mode[0] == 'n') {    /* no forzar el flag */
            gCloseFences = 1;
            gReapFences = 0;
        } else {                                           /* close (control ya medido) */
            gCloseFences = 1;
            gReapFences = 0;
        }
        gExtraFlags = (mode[0] == '0' || mode[0] == 'n') ? 0u : (1u << kFlagSyncFenceFd);
        fenceProbeStart();      /* el sondeo funciona en todos los modos */
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "submit: flags=0x%x; politica de fences=%s (env MOCHA_CAMERA_SYNCFD o "
                "/data/local/tmp/mocha-syncfd-mode)", gExtraFlags, mode);
    }
    return gExtraFlags;
}

/*
 * Traza de TODOS los ioctl sobre nodos nvhost, medida 2026-11-01 10:52: el shim
 * no vio NI UN submit (submit#=0) en una sesion de camara completa, mientras el
 * kernel inundaba el log con 'Waiting on the syncpt semaphore failed' y el
 * preview salia negro.  Hay dos explicaciones posibles y esta traza las separa:
 *   a) el blob nunca llega a SUBMIT (el pipeline VI no arranca) -> los waits fallan
 *      porque nadie incrementa los syncpt,
 *   b) el blob llama a ioctl() por una ruta que no pasa por este simbolo
 *      (syscall(__NR_ioctl) dentro de libnvos.so), y aqui no vemos nada aunque
 *      los submits si ocurran.
 * Los dos casos se distinguen mirando QUE ioctls llegan y con que resultado.
 */
constexpr int kCacheSize = 256;
int gFdKind[kCacheSize];   /* 0 desconocido, 1 nvhost, 2 otro */
char gFdName[kCacheSize][24];
unsigned gIoctlLogged = 0;
unsigned gIoctlOther = 0;
unsigned gIoctlFailed = 0;

/* Histograma de ioctls nvhost por numero de peticion.  Con un solo contador global no
 * distinguimos QUE espera el blob: el flood 'Waiting on the syncpt semaphore failed' ocurre
 * mientras este shim ve 309 ioctls fallidas de 200.001, o sea que las esperas que fallan no
 * son ioctls.  El histograma muestra que cmds emite cada nodo y con que exito. */
struct IoctlStat {
    unsigned request;
    unsigned count;
    unsigned fails;
    int fd;
};
constexpr int kIoctlStatSlots = 48;
IoctlStat gIoctlStats[kIoctlStatSlots];
int gIoctlStatCount = 0;

}  // namespace

static int fdKind(int fd) {
    if (fd < 3 || fd >= kCacheSize)
        return 0;
    if (gFdKind[fd] != 0)
        return gFdKind[fd];
    char path[64], name[128];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, name, sizeof(name) - 1);
    if (n <= 0) {
        gFdKind[fd] = 2;
        return 2;
    }
    name[n] = '\0';
    if (strstr(name, "nvhost")) {
        gFdKind[fd] = 1;
    } else if (strstr(name, "nvmap")) {
        /* /dev/nvmap: el codificador JPEG del blob llama a NVMAP_IOC_WRITE ahi, y es
         * justo donde sale 'NVMAP_IOC_WRITE failed: Invalid argument'. */
        gFdKind[fd] = 3;
    } else {
        gFdKind[fd] = 2;
    }
    snprintf(gFdName[fd], sizeof(gFdName[fd]), "%s", name);
    if ((gFdKind[fd] == 1 || gFdKind[fd] == 3) && gIoctlLogged < 40u)
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                            "fd %d es %s -> se trazan sus ioctl", fd, name);
    return gFdKind[fd];
}

/* ---- semaforos internos del blob --------------------------------------------
 * El texto 'Waiting on the syncpt semaphore failed 0x2' lo imprime el blob, y el
 * semaforo en cuestion es un NvOsSemaphore: libnvmm_camera_v3.so importa
 *   NvOsSemaphoreCreate / Wait / WaitTimeout / Signal / Destroy
 *   NvRmFenceSignalSemaphore        <- 'senala este semaforo cuando senale este fence'
 * y los resuelve por PLT, o sea que podemos verlos sin tocar el blob.
 *
 * Medido con el volcado de syncpoints: las esperas de KERNEL (CTRL_SYNCPT_WAITEX,
 * nr=6) funcionan todas - 1.933 esperas sobre los syncpt 6, 19, 21, 22, 23, 24, 25,
 * 30 y 31 con 0 fallos y umbrales que avanzan (24: 15..870, 30: 14..2043) - mientras
 * GET_WAITBASE (nr=17, param 0) falla 24/24 con EINVAL sobre /dev/camera.pcl porque
 * bus_client.c hace '!pdata->waitbases[0]' para el nodo de camera.
 * Este trazado responde la pregunta que falta: cuantos semaforos espera el blob, con
 * que plazo, y si alguien los senala alguna vez.
 */
static void* semSym(const char* name) {
    /* RTLD_NEXT primero; si libnvos se cargo antes que nosotros, buscarla por su
     * handle: dlsym(handle) devuelve la definicion de ESE modulo, no la nuestra
     * (con RTLD_DEFAULT caeriamos en nuestra propia funcion y hariamos recursion). */
    void* s = dlsym(RTLD_NEXT, name);
    if (s != nullptr)
        return s;
    void* h = dlopen("libnvos.so", RTLD_NOLOAD | RTLD_NOW);
    if (h != nullptr)
        s = dlsym(h, name);
    return s;
}

/* Medido: interponer estas funciones Cambia el timing del pipeline - en una sesion con
 * la interposicion activada el flood desaparecio (0 lineas) pero tambien desaparecieron
 * los frames (constructCaptureResult=0, process_capture_request=0) y un unico semafaro
 * (handle 0xafa384a0) giro 44 millones de veces con esperas 'sin plazo' que volvan
 * inmediatamente con exito (0) y solo 7 seales en toda la sesion.
 * O sea: sirve para DIAGNOSTICAR pero no debe estar activo en el build normal.
 * Se encade con -DMOCHA_SEM_TRACE (ver Android.mk).
 */
#ifdef MOCHA_SEM_TRACE
typedef void* NvSemaphore;
typedef int (*SemCreateFn)(NvSemaphore*);
typedef int (*SemWaitFn)(NvSemaphore);
typedef int (*SemWaitTimeoutFn)(NvSemaphore, unsigned);
typedef void (*SemSignalFn)(NvSemaphore);
typedef void (*SemDestroyFn)(NvSemaphore);

SemCreateFn gSemCreate = nullptr;
SemWaitFn gSemWait = nullptr;
SemWaitTimeoutFn gSemWaitTimeout = nullptr;
SemSignalFn gSemSignal = nullptr;
SemDestroyFn gSemDestroy = nullptr;

unsigned gSemWaits = 0;          /* Wait + WaitTimeout */
unsigned gSemWaitFails = 0;
unsigned gSemSignals = 0;
unsigned gSemCreated = 0;
unsigned gSemLogged = 0;
unsigned gSemTimeoutBuckets[6];  /* 0, 1..999, 1000..1999, 2000..4999, 5000..1e6, >1e6 */
unsigned gSemMinUs = 0xffffffffu;
unsigned gSemMaxUs = 0;

static void semBucket(unsigned us) {
    if (us == 0)
        gSemTimeoutBuckets[0]++;
    else if (us < 1000)
        gSemTimeoutBuckets[1]++;
    else if (us < 2000)
        gSemTimeoutBuckets[2]++;
    else if (us < 5000)
        gSemTimeoutBuckets[3]++;
    else if (us <= 1000000)
        gSemTimeoutBuckets[4]++;
    else
        gSemTimeoutBuckets[5]++;
    if (us < gSemMinUs)
        gSemMinUs = us;
    if (us > gSemMaxUs)
        gSemMaxUs = us;
}

static void dumpSemaphores(const char* why) {
    __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
            "semaforos NvOs (%s): %u esperas (%u fallidas), %u seiales, %u creados; "
            "us min=%u max=%u",
            why, gSemWaits, gSemWaitFails, gSemSignals, gSemCreated,
            gSemMinUs == 0xffffffffu ? 0u : gSemMinUs, gSemMaxUs);
    __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
            "   plazos pedidos: 0us=%u  1..999us=%u  1..2ms=%u  2..5ms=%u  5..1000ms=%u  >1000ms=%u",
            gSemTimeoutBuckets[0], gSemTimeoutBuckets[1], gSemTimeoutBuckets[2],
            gSemTimeoutBuckets[3], gSemTimeoutBuckets[4], gSemTimeoutBuckets[5]);
}

extern "C" int NvOsSemaphoreWaitTimeout(NvSemaphore h, unsigned us) {
    if (gSemWaitTimeout == nullptr)
        gSemWaitTimeout = (SemWaitTimeoutFn) semSym("NvOsSemaphoreWaitTimeout");
    if (gSemWaitTimeout == nullptr)
        return -1;
    gSemWaits++;
    semBucket(us);
    int r = gSemWaitTimeout(h, us);
    if (r != 0)
        gSemWaitFails++;
    if (gSemLogged < 30u) {
        gSemLogged++;
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "semaforo espera#%u h=%p plazo=%u us -> r=%d%s",
                gSemLogged, h, us, r, r == 0 ? " (exito)" : " (FALLA)");
    } else if ((gSemWaits % 1000u) == 1u) {
        dumpSemaphores("parcial");
    }
    return r;
}

extern "C" int NvOsSemaphoreWait(NvSemaphore h) {
    if (gSemWait == nullptr)
        gSemWait = (SemWaitFn) semSym("NvOsSemaphoreWait");
    if (gSemWait == nullptr)
        return -1;
    gSemWaits++;
    semBucket(0xffffffffu);              /* espera sin plazo: cap 'sin limite' */
    int r = gSemWait(h);
    if (r != 0)
        gSemWaitFails++;
    if (gSemLogged < 30u) {
        gSemLogged++;
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "semaforo espera#%u h=%p sin plazo -> r=%d%s",
                gSemLogged, h, r, r == 0 ? " (exito)" : " (FALLA)");
    }
    return r;
}

extern "C" void NvOsSemaphoreSignal(NvSemaphore h) {
    if (gSemSignal == nullptr)
        gSemSignal = (SemSignalFn) semSym("NvOsSemaphoreSignal");
    if (gSemSignal == nullptr)
        return;
    gSemSignals++;
    gSemSignal(h);
}

extern "C" int NvOsSemaphoreCreate(NvSemaphore* ph) {
    if (gSemCreate == nullptr)
        gSemCreate = (SemCreateFn) semSym("NvOsSemaphoreCreate");
    if (gSemCreate == nullptr)
        return -1;
    gSemCreated++;
    return gSemCreate(ph);
}

extern "C" void NvOsSemaphoreDestroy(NvSemaphore h) {
    if (gSemDestroy == nullptr)
        gSemDestroy = (SemDestroyFn) semSym("NvOsSemaphoreDestroy");
    if (gSemDestroy == nullptr)
        return;
    gSemDestroy(h);
}
#endif  /* MOCHA_SEM_TRACE */

static bool isCtrlNode(int fd) {
    if (fd < 3 || fd >= kCacheSize)
        return false;
    return strcmp(gFdName[fd], "/dev/nvhost-ctrl") == 0;
}

static WaitStat* waitStat(int nr, uint32_t key, int fd) {
    int i;
    for (i = 0; i < gWaitStatCount; i++)
        if (gWaitStats[i].nr == nr && gWaitStats[i].key == key)
            return &gWaitStats[i];
    if (gWaitStatCount >= kWaitSlots)
        return nullptr;
    WaitStat* w = &gWaitStats[gWaitStatCount++];
    w->nr = nr;
    w->key = key;
    w->lo = 0xffffffffu;
    w->hi = 0;
    w->n = 0;
    w->fails = 0;
    w->fences = 0;
    w->lastErr = 0;
    w->fd = fd;
    return w;
}

static void waitRange(WaitStat* w, uint32_t v) {
    if (w == nullptr)
        return;
    if (v < w->lo)
        w->lo = v;
    if (v > w->hi)
        w->hi = v;
    w->n++;
}

static void dumpWaitStats(const char* why) {
    int i;
    __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
            "esperas de syncpoint (%s), %u entradas, %u ioctl de syncpoint vistos:",
            why, (unsigned) gWaitStatCount, gWaitTotal);
    for (i = 0; i < gWaitStatCount; i++) {
        const WaitStat* w = &gWaitStats[i];
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "   nr=%-2u key=%-10u n=%-7u fallas=%-6u rango=%u..%u fences=%u err=%d fd=%d%s",
                (unsigned) w->nr, w->key, w->n, w->fails,
                w->lo == 0xffffffffu ? 0u : w->lo, w->hi, w->fences, w->lastErr, w->fd,
                (w->fd >= 3 && w->fd < kCacheSize) ? gFdName[w->fd] : "");
    }
}

/* Decodifica las llamadas de syncpoint.  'argp' es memoria del propio proceso
 * (el kernel acaba de tocarla), y solo se sigue cuando el ioctl tuvo exito y el
 * numero de elementos esta acotado: asi no leemos fuera del buffer del blob. */
static void traceSyncptCmd(int fd, unsigned request, void* argp, int ret) {
    unsigned nr = request & 0xffu;
    unsigned size = (request >> kIocSizeShift) & 0x3fffu;
    bool ctrl = isCtrlNode(fd);
    int err = ret == 0 ? 0 : errno;

    if (ctrl) {
        if (nr == 1 || nr == 8) {            /* SYNCPT_READ / SYNCPT_READ_MAX */
            if (size < sizeof(CtrlRead))
                return;
            CtrlRead* a = (CtrlRead*) argp;
            WaitStat* w = waitStat((int) nr, a->id, fd);
            waitRange(w, ret == 0 ? a->value : 0u);
            if (w != nullptr)
                w->lastErr = err;
            gWaitTotal++;
            if (gWaitLogged < 8u) {
                gWaitLogged++;
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "read#%u %s syncpt=%u valor=%u -> %s",
                        gWaitLogged, nr == 1 ? "CTRL_SYNCPT_READ" : "CTRL_SYNCPT_READ_MAX",
                        a->id, a->value, ret == 0 ? "ok" : "FALLO");
            }
            return;
        }
        if (nr == 3 || nr == 6 || nr == 9) {  /* WAIT / WAITEX / WAITMEX */
            uint32_t id, thresh;
            int32_t timeout;
            if (nr == 3 && size >= sizeof(CtrlWait)) {
                CtrlWait* a = (CtrlWait*) argp;
                id = a->id; thresh = a->thresh; timeout = a->timeout;
            } else if (nr == 6 && size >= sizeof(CtrlWaitex)) {
                CtrlWaitex* a = (CtrlWaitex*) argp;
                id = a->id; thresh = a->thresh; timeout = a->timeout;
            } else if (nr == 9 && size >= sizeof(CtrlWaitmex)) {
                CtrlWaitmex* a = (CtrlWaitmex*) argp;
                id = a->id; thresh = a->thresh; timeout = a->timeout;
            } else {
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "espera nr=%u tam=%u: estructura mas pequena que la nuestra "
                        "(%u/%u/%u), no se decodifica", nr, size,
                        (unsigned) sizeof(CtrlWait), (unsigned) sizeof(CtrlWaitex),
                        (unsigned) sizeof(CtrlWaitmex));
                return;
            }
            WaitStat* w = waitStat((int) nr, id, fd);
            if (w != nullptr) {
                w->n++;
                if (ret != 0)
                    w->fails++;
                w->lastErr = err;
                if (thresh < w->lo)
                    w->lo = thresh;
                if (thresh > w->hi)
                    w->hi = thresh;
            }
            gWaitTotal++;
            if (gWaitLogged < 40u) {
                gWaitLogged++;
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "espera#%u nr=%u syncpt=%u umbral=%u timeout=%d ms -> %s (%s)",
                        gWaitLogged, nr, id, thresh, timeout,
                        ret == 0 ? "ok" : "FALLO", ret == 0 ? "" : strerror(err));
            } else if ((gWaitTotal % 2000u) == 1u) {
                dumpWaitStats("parcial");
            }
            return;
        }
        if (nr == 10 || nr == 11) {          /* CTRL_SYNC_FENCE_CREATE */
            if (nr != 10 || size < sizeof(CtrlFence32))
                return;                      /* la variante de 64 no se ha medido */
            CtrlFence32* a = (CtrlFence32*) argp;
            uint32_t npts = a->num_pts;
            char pts[96];
            int len = 0;
            unsigned i;

            pts[0] = '\0';
            if (ret == 0 && a->pts != 0 && npts <= 8u) {
                const SyncptInfo* info = (const SyncptInfo*) (uintptr_t) a->pts;
                for (i = 0; i < npts; i++) {
                    int room = (int) sizeof(pts) - len - 1;
                    int w2;
                    if (room < 16)
                        break;
                    w2 = snprintf(pts + len, (size_t) room + 1, " %u<=%u",
                            info[i].id, info[i].thresh);
                    if (w2 < 0)
                        break;
                    len += (w2 < room) ? w2 : room;
                    if (w2 >= room)
                        break;
                    WaitStat* w = waitStat(10, info[i].id, fd);
                    if (w != nullptr)
                        w->fences++;
                }
            } else if (npts > 8u) {
                snprintf(pts, sizeof(pts), " ?%u pts", npts);
            }
            WaitStat* w = waitStat(10, kNoKey, fd);
            if (w != nullptr) {
                w->n++;
                if (ret != 0)
                    w->fails++;
                w->lastErr = err;
            }
            gWaitTotal++;
            if (gFenceCreateLogged < 24u) {
                gFenceCreateLogged++;
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "ctrl_fence_create#%u num_pts=%u [%s] -> %s fd=%d",
                        gFenceCreateLogged, npts, pts,
                        ret == 0 ? "ok" : "FALLO", ret == 0 ? a->fence_fd : -err);
            }
            return;
        }
        return;
    }

    /* canal: GET_SYNCPOINTS(2) / GET_WAITBASES(3) cuentan; GET_SYNCPOINT(16) y
     * GET_WAITBASE(17) devuelven el syncpt o el waitbase que el blob va a usar
     * para sus esperas, que es lo que queremos comparar con los submits. */
    if (nr == 2 || nr == 3 || nr == 16 || nr == 17) {
        if (size < sizeof(ChanParam)) {
            if (nr == 2 || nr == 3)
                return;                      /* _IOR(struct nvhost_get_param_args) */
            __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                    "canal nr=%u tam=%u < %u: ABI antigua, no se decodifica",
                    nr, size, (unsigned) sizeof(ChanParam));
            return;
        }
        ChanParam* a = (ChanParam*) argp;
        WaitStat* w = waitStat((int) nr, a->param, fd);
        if (w != nullptr) {
            w->n++;
            if (ret != 0)
                w->fails++;
            w->lastErr = err;
            if (ret == 0) {
                if (a->value < w->lo)
                    w->lo = a->value;
                if (a->value > w->hi)
                    w->hi = a->value;
            }
        }
        if (gWaitLogged < 24u && (nr == 16 || nr == 17)) {
            gWaitLogged++;
            __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                    "canal %s fd=%d(%s) param=%u -> valor=%u %s (%s)",
                    nr == 16 ? "GET_SYNCPOINT" : "GET_WAITBASE", fd,
                    (fd >= 3 && fd < kCacheSize) ? gFdName[fd] : "?",
                    a->param, a->value, ret == 0 ? "ok" : "FALLO",
                    ret == 0 ? "" : strerror(err));
        }
        return;
    }
}

/* bionic declares "int ioctl(int, unsigned, ...)" (bits/ioctl.h); on a 32 bit
 * build int/unsigned/unsigned long are all 32 bit registers, so the calling
 * convention of the blobs is unaffected. */
/* ---- limitador del registro directo del blob ---------------------------------
 * El blob de 2015 saca su depurado por dos vias distintas:
 *   - stderr: el hook de CameraWrapper.cpp lo reenvia a logcat y ya lo limitaba
 *   - __android_log_print(...): directo a logcat, sin limitar
 *
 * Medido (2026-11-02, sesion de 16 s con la camara funcionando): 35.775 lineas, casi todas
 * con tag 'NvOsDebugPrintf' y de un unico hilo (tid 2393), todas con el mismo texto
 * 'Waiting on the syncpt semaphore failed 0x2'. El logcat muestra el texto igual que el
 * mensaje del blob, o sea que llega como un unico %s ya formateado: interponer
 * __android_log_print con esa firma es seguro. Los casos con varargs autenticos se reenvian
 * INTACTOS con __android_log_vprint usando el va_list original (nunca re-formateamos lo que
 * reenviamos: solo usamos una copia del va_list para poder comparar el texto).
 *
 * Por que merece la pena: en una medicion anterior logd llego al 70 % de CPU con 642.427
 * faltos menores e hizo pruning ('Skipping entries from slow reader'), perdiendose el mensaje
 * distintivo que explica el fallo del syncpt. Historicamente el flood era ~15.000 lineas/s
 * (842.513 lineas identicas en 55 s = 88 % del log) y costaba bateria real.
 *
 * Politica: de cada tag, la PRIMERA aparicion de cada texto distinto por segundo pasa; las
 * repeticiones se resumen al cambiar de segundo, con aviso bajo nuestra propia tag para no
 * romper la deduplicacion del tag original. La senal diagnostica se conserva.
 */
#define MOCHA_LOG_MAX_DISTINCT_PER_SEC 40

struct LogTagState {
    const char* tag;
    unsigned long second;
    unsigned distinct;
    unsigned repeats;
    char last[240];
};

static LogTagState gLogTags[8];
static pthread_mutex_t gLogLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned gLogIn = 0, gLogOut = 0, gLogSuppressed = 0;
static unsigned gLogDumped = 0;

typedef int (*LogPrintFn)(int, const char*, const char*, ...);
static LogPrintFn gRealLogPrint = nullptr;

static void logTagFlush(LogTagState& st) {
    if (st.repeats == 0)
        return;
    /* OJO: usamos __android_log_write, NO __android_log_print: esa funcion si la
     * interponemos, y llamar a la nuestra mientras sujetamos gLogLock seria un deadlock
     * (el mutex no es recursivo). __android_log_write no esta interpuesta. */
    char msg[300];
    snprintf(msg, sizeof msg, "[limite] tag %s: '%s' repetida %u veces mas en ese segundo",
            st.tag != nullptr ? st.tag : "(null)", st.last, st.repeats);
    __android_log_write(ANDROID_LOG_INFO, "mocha-hal30", msg);
    st.repeats = 0;
}

static LogTagState* logTagSlot(const char* tag) {
    for (int i = 0; i < 8; i++) {
        if (gLogTags[i].tag == nullptr) {
            gLogTags[i].tag = tag;
            return &gLogTags[i];
        }
        if (tag != nullptr && gLogTags[i].tag != nullptr && strcmp(gLogTags[i].tag, tag) == 0)
            return &gLogTags[i];
    }
    return nullptr;
}

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    if (prio > ANDROID_LOG_DEBUG) {
        /* INFO/WARN/ERROR no se tocan: aqui solo limitamos el depurado (VERBOSE y DEBUG),
         * que es lo que inunda. Asi nuestros propios diagnosticos no pasan por el filtro. */
        int r = __android_log_vprint(prio, tag, fmt, ap);
        va_end(ap);
        return r;
    }

    char txt[240];
    txt[0] = 0;
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(txt, sizeof txt, fmt, cp);
    va_end(cp);

    if (n < 0 || n >= (int) sizeof txt) {
        /* Formato largo o raro: no lo reproducimos, reenviamos la llamada tal cual. */
        int r = __android_log_vprint(prio, tag, fmt, ap);
        va_end(ap);
        return r;
    }

    int out = 0;
    unsigned long now = (unsigned long) time(nullptr);
    pthread_mutex_lock(&gLogLock);
    gLogIn++;
    LogTagState* st = logTagSlot(tag);
    if (st == nullptr) {
        pthread_mutex_unlock(&gLogLock);
        int r = __android_log_vprint(prio, tag, fmt, ap);
        va_end(ap);
        return r;
    }
    if (now != st->second) {
        logTagFlush(*st);
        st->second = now;
        st->distinct = 0;
        st->last[0] = 0;
    }
    if (strcmp(txt, st->last) == 0) {
        /* Mesma linea repetida: se descarta la llamada a logd (ahorro real de CPU). */
        st->repeats++;
        gLogSuppressed++;
    } else if (st->distinct < MOCHA_LOG_MAX_DISTINCT_PER_SEC) {
        snprintf(st->last, sizeof st->last, "%s", txt);
        st->distinct++;
        gLogOut++;
        out = __android_log_vprint(prio, tag, fmt, ap);
    } else {
        st->repeats++;
        gLogSuppressed++;
    }
    if ((gLogIn % 20000u) < 4u && gLogDumped < 8u) {
        gLogDumped++;
        char msg[160];
        snprintf(msg, sizeof msg, "limitador de log: %u llamadas, %u reenviadas, %u descartadas",
                gLogIn, gLogOut, gLogSuppressed);
        __android_log_write(ANDROID_LOG_INFO, "mocha-hal30", msg);
    }
    pthread_mutex_unlock(&gLogLock);
    va_end(ap);
    return out;
}

/* ---- interposicion del registro del blob, en su punto de origen ----------------
 * libnvmm_camera_v3.so y libnvmm_camera.so importan (los dejan sin definir) TANTO
 * NvOsDebugPrintf como NvOsFprintf, y el mismo texto 'Waiting on the syncpt semaphore
 * failed 0x%x' sale por dos vias distintas:
 *   - NvOsDebugPrintf -> logcat con tag 'NvOsDebugPrintf'   (eso si lo cortaba el
 *     limitador de __android_log_print: 40.000 llamadas, 39.864 descartadas)
 *   - NvOsFprintf -> stderr, que CameraWrapper.cpp reenvia con tag 'CameraWrapper'
 *     (esa via seguia inundando: medido 2.246 lineas/s en una sesion normal)
 * Por eso el limitador de log a secas no basta: hay que deduplicar en el origen.
 *
 * AQUI el ahorro es mayor: si la linea sobra, el blob no formatea, no escribe en stderr, no
 * pasa por la tuberia y no llama a logd. Las lineas que si pasan se reenvian con el fmt y
 * el va_list ORIGINALES mediante __android_log_vprint (asi no hay que reenviar varargs a
 * mano, que es la parte fragil: nunca consumimos el va_list original, solo una copia para
 * poder comparar el texto).
 */
typedef int (*NvOsVprintFn)(int, const char*, const char*, va_list);

typedef int (*NvOsDebugPrintFn)(const char*, ...);
typedef int (*NvOsFprintfFn)(void*, const char*, ...);
static NvOsDebugPrintFn gRealDebugPrintf = nullptr;
static NvOsFprintfFn gRealFprintf = nullptr;
static unsigned gNvosLogIn = 0, gNvosLogDrop = 0;

/* Unica familia de textos que filtramos: el flood conocido del HAL de 2015.
 * Todo lo demas se reenvia intacto a la funcion real (no nos metemos en salidas que
 * podrian ser datos y no depuracion). */
static const char* const kFloodPatterns[] = {
    "Waiting on the syncpt semaphore failed",
    "Failed to get FRD",
    "Work function returned",
};

static bool isFloodText(const char* txt) {
    for (size_t i = 0; i < sizeof kFloodPatterns / sizeof kFloodPatterns[0]; i++)
        if (strncmp(txt, kFloodPatterns[i], strlen(kFloodPatterns[i])) == 0)
            return true;
    return false;
}

static void* nvosSym(const char* name) {
    void* s = dlsym(RTLD_NEXT, name);
    if (s != nullptr)
        return s;
    void* h = dlopen("libnvos.so", RTLD_NOLOAD | RTLD_NOW);
    if (h != nullptr)
        s = dlsym(h, name);
    return s;
}

/* Misma politica que el limitador de log: primer texto distinto por tag y por segundo,
 * repeticiones resumadas. 'key' separa las vias de emision para no mezclar deduplicados. */
static bool logGate(const char* key, const char* txt) {
    unsigned long now = (unsigned long) time(nullptr);
    bool pass = false;
    pthread_mutex_lock(&gLogLock);
    LogTagState* st = logTagSlot(key);
    if (st == nullptr) {
        pthread_mutex_unlock(&gLogLock);
        return true;
    }
    if (now != st->second) {
        logTagFlush(*st);
        st->second = now;
        st->distinct = 0;
        st->last[0] = 0;
    }
    if (strcmp(txt, st->last) == 0) {
        st->repeats++;
        gLogSuppressed++;
    } else if (st->distinct < MOCHA_LOG_MAX_DISTINCT_PER_SEC) {
        snprintf(st->last, sizeof st->last, "%s", txt);
        st->distinct++;
        gLogOut++;
        pass = true;
    } else {
        st->repeats++;
        gLogSuppressed++;
    }
    if (((gLogIn + gNvosLogIn) % 20000u) < 4u && gLogDumped < 16u) {
        gLogDumped++;
        char msg[160];
        snprintf(msg, sizeof msg, "limitador de log: %u llamadas, %u reenviadas, %u descartadas",
                gLogIn, gLogOut, gLogSuppressed);
        __android_log_write(ANDROID_LOG_INFO, "mocha-hal30", msg);
    }
    pthread_mutex_unlock(&gLogLock);
    return pass;
}

extern "C" int NvOsDebugPrintf(const char* fmt, ...) {
    if (fmt == nullptr)
        return 0;
    gNvosLogIn++;
    va_list ap;
    va_start(ap, fmt);
    char txt[240];
    txt[0] = 0;
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(txt, sizeof txt, fmt, cp);
    va_end(cp);
    if (n < 0) {
        __android_log_vprint(ANDROID_LOG_DEBUG, "NvOsDebugPrintf", fmt, ap);
        va_end(ap);
        return 0;
    }
    if (n >= (int) sizeof txt || !isFloodText(txt)) {
        /* No es el flood o no cabe entero: reenviamos intacto con fmt y va_list originales. */
        __android_log_vprint(ANDROID_LOG_DEBUG, "NvOsDebugPrintf", fmt, ap);
        va_end(ap);
        return 0;
    }
    if (logGate("nvos:dbg", txt))
        __android_log_vprint(ANDROID_LOG_DEBUG, "NvOsDebugPrintf", fmt, ap);
    else
        gNvosLogDrop++;
    va_end(ap);
    return 0;
}

extern "C" int NvOsFprintf(void* hFile, const char* fmt, ...) {
    if (fmt == nullptr)
        return 0;
    if (gRealFprintf == nullptr)
        gRealFprintf = (NvOsFprintfFn) nvosSym("NvOsFprintf");
    gNvosLogIn++;
    va_list ap;
    va_start(ap, fmt);
    char txt[240];
    txt[0] = 0;
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(txt, sizeof txt, fmt, cp);
    va_end(cp);
    if (gRealFprintf == nullptr || n < 0 || n >= (int) sizeof txt || !isFloodText(txt)) {
        /* Texto que no es el flood: no lo tocamos. Lo reenviamos a la funcion REAL con el
         * fmt y el va_list original, mediante un puntero a funcion real: no podemos pasar
         * un va_list a una funcion varargs, asi que reemitimos el texto ya resuelto (es lo
         * mismo para salida de texto) y, si la real no esta disponible, a logcat. */
        if (gRealFprintf != nullptr)
            gRealFprintf(hFile, "%s", txt);
        else
            __android_log_vprint(ANDROID_LOG_DEBUG, "NvOsFprintf", fmt, ap);
        va_end(ap);
        return 0;
    }
    if (logGate("nvos:printf", txt)) {
        /* Reemitimos el texto ya resuelto: una sola llamada real, sin reenviar varargs. */
        gRealFprintf(hFile, "%s", txt);
    } else {
        gNvosLogDrop++;
    }
    va_end(ap);
    return 0;
}

/* ---- trazador de ioctls nvmap (magic 'N'), observacion pura ----------------------
 * Por que hace falta aqui: la foto muere dentro del codificador JPEG del blob y lo unico
 * que el blob dice es, literalmente (camera.log 26784-26791, sesion 2026-11-15 20:35:26):
 *     Encoder_Open ++
 *     Encoder_Close ++
 *     NVMAP_IOC_WRITE failed: Invalid argument        (2 veces)
 *     NvCameraHal3JpegProcessor: doJpegEncoding -- (error 0x6)
 * 'Invalid argument' es -EINVAL.  En este kernel NVMAP_IOC_WRITE/READ van a
 * nvmap_ioctl_rw_handle() (drivers/video/tegra/nvmap/nvmap_ioctl.c:514) y solo pueden dar
 * -EINVAL en cuatro sitios:
 *   b) !op.addr || !op.count || !op.elem_size                                   (:545)
 *   c) unmarshal_user_handle(op.handle) == 0, o sea que el 'handle' que manda el blob
 *      NO es un fd de dmabuf.  En este arbol unmarshal_user_handle() (nvmap_ioctl.c:53)
 *      se limita a nvmap_get_id_from_dmabuf_fd(); el decodificador historico de handles
 *      de nvmap ya no existe                                            (:549)
 *   d) rw_handle() (nvmap_ioctl.c:1012): elem_size>h->size, h_offs>=h->size,
 *      elem_size>sys_stride, elem_size>h_stride,
 *      sys_stride>(h->size-h_offs)/count, h_stride>(h->size-h_offs)/count (:1024-1042)
 * Este trazador comprueba (b) y (c) desde fuera y saca a la luz la geometria que el blob
 * pidio en (d).  NO cambia nada: observa despues de que la llamada real haya terminado.
 * El tamano real de h->size lo imprime el propio kernel con un pr_warn acotado en esas
 * mismas clausulas: leerlo desde aqui exigiria lseek(SEEK_END) sobre el fd de dmabuf del
 * blob, y moverle la posicion a SU fd si cambiaria su comportamiento.
 */
namespace {

constexpr unsigned kNvmapMagic = 'N';

/* include/linux/nvmap.h:180 y :191.  En el provider de 32 bits 'unsigned long addr'
 * mide 4, asi que nvmap_rw_handle y nvmap_rw_handle_32 quedan identicos. */
struct NvmapRw {
    uint32_t addr, handle, offset, elem_size, hmem_stride, user_stride, count;
};
struct NvmapCache {
    uint32_t addr, handle, len;
    int32_t op;
};

struct NvmapNr { unsigned nr; const char* name; };
const NvmapNr kNvmapNrs[] = {
    {0, "CREATE"},       {2, "FROM_ID"},      {3, "ALLOC"},
    {4, "FREE"},         {5, "MMAP_32"},      {6, "WRITE_32"},
    {7, "READ_32"},      {8, "PARAM_32"},     {10, "PIN_MULT_32"},
    {11, "UNPIN_MULT_32"}, {12, "CACHE_32"},  {13, "GET_ID"},
    {14, "SHARE"},       {15, "GET_FD"},      {16, "FROM_FD"},
    {17, "CACHE_LIST"},  {18, "RESERVE"},     {100, "ALLOC_KIND"},
};
/* El resto de structs nvmap que el blob usa y que hay que saber lo que piden:
 * include/linux/nvmap.h:157 (create_handle), :161 (alloc_handle), :171
 * (alloc_kind_handle), :185 (map_caller_32). */
struct NvmapCreate { uint32_t pide; uint32_t devuelve; };   /* union{size|fd|id} + handle(out) */
struct NvmapAlloc { uint32_t handle, heap_mask, flags, align; };
struct NvmapAllocKind { uint32_t handle, heap_mask, flags, align; uint8_t kind, comp_tags; };
struct NvmapMap32 { uint32_t handle, offset, length, flags, addr; };

/* Copia de la memoria del PROPIO proceso via /proc/self/mem.  Por que no memcpy: si el
 * blob pasara un puntero no mapeado, el kernel solo habria devuelto -EFAULT, pero un
 * memcpy nuestro habria matado el provider con SIGSEGV.  pread sobre /proc/self/mem
 * devuelve error en lugar de matar nada. */
int gMemFd = -1;

/* El snapshot previo de argumentos es OPT-IN (2026-11-16, K-46).  Motivo: el shim va
 * LD_PRELOAD'ado en el camera provider (parche hardware/interfaces/0005), y abrir
 * /proc/self/mem + pread dentro del camino de los ioctls nvmap metio una llamada que puede
 * bloquear (access_remote_vm() coge mmap_sem del proceso) en los hilos rapidos del provider -
 * los mismos que ya mostraban 'FastThread: did not receive expected priority boost on time'.
 * El tablet llevo desde 14:14 sin dar ventanas estables de adb con ESTE shim en servicio.
 * Sin evidencia de que sea la causa (K-46 sigue abierto), pero el default tiene que ser el
 * comportamiento conocido-bueno: aqui = SIN snapshots.  Se enciende con
 *   /data/local/tmp/mocha-argsnap  contenido '1'   o   MOCHA_ARGSNAP=1.
 */
bool argSnapOn()
{
    static int estado = -1;
    if (estado < 0) {
        const char* env = getenv("MOCHA_ARGSNAP");
        if (env != nullptr && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y'))
            estado = 1;
        else {
            FILE* f = fopen("/data/local/tmp/mocha-argsnap", "r");
            char buf[8];
            estado = 0;
            if (f != nullptr) {
                if (fgets(buf, sizeof(buf), f) != nullptr && buf[0] == '1')
                    estado = 1;
                fclose(f);
            }
        }
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "snapshot de argumentos nvmap: %s (knob /data/local/tmp/mocha-argsnap o "
                "MOCHA_ARGSNAP=1)", estado == 1 ? "ACTIVO" : "apagado (comportamiento conocido-bueno)");
    }
    return estado == 1;
}

bool memTry(const void* addr, void* dst, size_t n) {
    if (addr == nullptr || n == 0 || n > 64)
        return false;
    if (gMemFd < 0) {
        gMemFd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
        if (gMemFd < 0)
            return false;
    }
    return pread(gMemFd, dst, n, (off_t)(uintptr_t)addr) == (ssize_t)n;
}

const char* heapName(uint32_t m) {
    if (m & (1u << 30)) return "IOVMM(sysmem)";
    if (m & (1u << 28)) return "CARVEOUT_VPR";
    if (m & (1u << 29)) return "CARVEOUT_IRAM";
    if (m & (1u << 27)) return "CARVEOUT_TSEC";
    if (m & 1u) return "CARVEOUT_GENERIC";
    return "?";
}

/* nvmap ioctls que llegan por un fd de /dev/nvmap pero no estan en kNvmapNrs: imprimir
 * los primeros distintos, que 'sin clasificar' no puede ser la respuesta. */
unsigned gNvmapUnk[8][2];
int gNvmapUnkN = 0;
unsigned gNvmapCreateLogged = 0, gNvmapAllocLogged = 0, gNvmapMapLogged = 0;

constexpr int kNvmapSlots = (int)(sizeof kNvmapNrs / sizeof kNvmapNrs[0]);
unsigned gNvmapCnt[kNvmapSlots];
unsigned gNvmapFail[kNvmapSlots];
unsigned gNvmapTotal = 0, gNvmapOther = 0;
unsigned gNvmapRwLogged = 0, gNvmapErrLogged = 0;

/* K-47: traductor de la peticion nvmap degenerada de CERO atomos.  Medido dos veces con firma
 * identica (2026-11-16 16:54 con pack al 79 % y 19:14 con pack al 33 %, kernel #22, shim trazador
 * md5 f2a6de5d):  handle=1163(anon_inode:dmabuf) off=0 elem=0 hstride=0 ustride=0 count=1 addr=0x0
 * -> nvmap_ioctl.c:545 devuelve -EINVAL -> 'doJpegEncoding -- (error 0x6)' -> 'threadLoop: jpeg
 * encoding fail' -> no hay JPEG.  O sea NO depende de la bateria ni del estado del pack.
 * Responder AQUI (en el shim) en vez de en el kernel es la misma accion con bastante menos riesgo:
 * la peticion no transfiere datos por construccion, se deshace empujando otro .so (segundos, sin
 * reflashear LNX y sin reiniciar: basta con que el provider vuelva a arrancar) y no puede estropear
 * un arranque.  Knob /data/local/tmp/mocha-nvmap-noop: '0' = pasar al kernel tal cual (comportamiento
 * de siempre); fichero ausente = no traducir.  ACTUALIZADO 2026-11-16 19:36, medido con el camino ya
 * reparado (K-51: permisos de /dev/tegra_avpchannel): el codificador abre ('Encoder_Open ++' y ahora
 * tambien su marca de salida '--'), 'nvmap WRITE_32: 272 llamadas, 0 fallas' y la peticion degenerada
 * DEJ6 DE APARECER -> era residuo del cierre fallido del codificador, no la causa.  Por eso el
 * traductor queda APAGADO por defecto y solo la cadena '1' del knob lo enciende: no queremos
 * enmascarar un defecto real si vuelve a aparecer. */
static int nvmapNoopOn(void)
{
    static int estado = -1;
    if (estado < 0) {
        FILE *f = fopen("/data/local/tmp/mocha-nvmap-noop", "r");
        char c = 0;
        if (f != nullptr) {
            if (fread(&c, 1, 1, f) != 1)
                c = '0';
            fclose(f);
            estado = (c == '1') ? 1 : 0;
        } else {
            const char *e = getenv("MOCHA_NVMAP_NOOP");
            estado = (e != nullptr && e[0] == '1') ? 1 : 0;
        }
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "traductor de WRITE/READ de 0 atomos: %s (knob /data/local/tmp/mocha-nvmap-noop: "
                "escribe 1 para activarlo)", estado ? "ACTIVO" : "apagado (por defecto)");
    }
    return estado;
}
unsigned gNvmapNoopLogged = 0;

int nvmapNrSlot(unsigned nr) {
    for (int i = 0; i < kNvmapSlots; i++)
        if (kNvmapNrs[i].nr == nr)
            return i;
    return -1;
}

/* Que hay detras del 'handle' que manda el blob (deberia ser un fd de dmabuf: eso
 * decide el caso (c)). */
bool fdNameOf(int fd, char* out, size_t n) {
    char path[48];
    if (fd < 0 || fd > 4096) { out[0] = 0; return false; }
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t r = readlink(path, out, n - 1);
    if (r <= 0) { out[0] = 0; return false; }
    out[r] = '\0';
    return true;
}

}  // namespace

/* Decodifica una llamada nvmap.  'pre' es la copia de los argumentos ANTES de la llamada
 * real (o nullptr si no se pudo copiar); 'argp' es la struct del blob DESPUES de la llamada.
 * Por que hace falta 'pre' (aprendido el 2026-11-16, camera.log 28621-28626):
 * nvmap_ioctl_rw_handle() termina con `__put_user(copied, &uarg->count)`
 * (drivers/video/tegra/nvmap/nvmap_ioctl.c:612), o sea que el kernel ESCRIBE el numero de
 * bytes copiados encima del `count` del blob.  Decodificar despues de un fallo muestra
 * count=0 y hace creer que el blob mando count=0: el trazador etiqueto
 * 'addr/count/elem_size en 0' una llamada que el kernel habia rechazado por
 * 'h_offs >= h->size' con count=1.  Los argumentos reales solo se ven antes de llamar.
 */
static void traceNvmapCmd(int fd, unsigned request, const void* pre, unsigned preSize,
                          void* argp, int ret) {
    unsigned nr = request & 0xffu;
    unsigned size = (request >> kIocSizeShift) & 0x3fffu;
    int slot = nvmapNrSlot(nr);
    gNvmapTotal++;
    if (slot >= 0) {
        gNvmapCnt[slot]++;
        if (ret != 0)
            gNvmapFail[slot]++;
    }

    /* Decodificacion POST-llamada de la struct del blob: es segura porque seguimos DENTRO del
     * ioctl(), o sea el marco de pila del llamador sigue vivo y su struct tambien.  Unica
     * salvedad, documentada: en un rw CON EXITO el kernel escribe 'copied' encima de 'count'
     * (nvmap_ioctl.c:602-612); en un rw FALLADO el kernel sale antes de ese __put_user, asi que
     * lo que se lee son exactamente los argumentos que mando el blob.  Por eso el snapshot
     * previo (argSnapOn) es un extra opcional, no un requisito: sin el, las trazas de
     * CREATE/ALLOC/MMAP y del WRITE fallido funcionan igual (aprendido 2026-11-16 16:52, cuando
     * atar todo al snapshot dej6 la medicion sin datos porque el provider no pudo leer
     * /data/local/tmp ni /proc/self/mem). */
    unsigned char local[40];
    const void* src = pre;
    unsigned srcSize = preSize;
    if (src == nullptr && argp != nullptr && size <= sizeof(local)) {
        memcpy(local, argp, size);
        src = local;
        srcSize = size;
    }

    if (slot < 0 && gNvmapUnkN < 8) {
        int i;
        for (i = 0; i < gNvmapUnkN; i++)
            if (gNvmapUnk[i][0] == nr && gNvmapUnk[i][1] == size)
                break;
        if (i == gNvmapUnkN) {
            gNvmapUnk[gNvmapUnkN][0] = nr;
            gNvmapUnk[gNvmapUnkN][1] = size;
            gNvmapUnkN++;
            __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                    "nvmap SIN CLASIFICAR nr=%u dir=%u tam=%u fd=%d -> %s",
                    nr, (request >> kIocDirShift) & 0x3u, size, fd,
                    ret == 0 ? "ok" : strerror(errno));
        }
    }

    /* CREATE / FROM_ID / GET_ID / SHARE / GET_FD / FROM_FD: el campo de entrada es
     * size (CREATE) o fd (FROM_FD) y el kernel devuelve el handle en el segundo campo. */
    if ((nr == 0 || nr == 2 || nr == 13 || nr == 14 || nr == 15 || nr == 16) &&
        src != nullptr && srcSize >= sizeof(NvmapCreate) && gNvmapCreateLogged < 16u) {
        const NvmapCreate* a = (const NvmapCreate*)src;
        const NvmapCreate* o = (const NvmapCreate*)src;
        gNvmapCreateLogged++;
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "nvmap %s fd=%d pide=%u(0x%x) handle=%u tam_hm=%u -> %s%s",
                kNvmapNrs[slot].name, fd, a->pide, a->pide, o->devuelve, size,
                ret == 0 ? "ok" : "FALLO", ret == 0 ? "" : strerror(errno));
        return;
    }

    /* ALLOC / ALLOC_KIND: size NO esta aqui (se dio en CREATE); lo que hay que leer es
     * desde que heap y con que alineamiento se reparte. */
    if ((nr == 3 || nr == 100) && src != nullptr && srcSize >= (nr == 3 ? sizeof(NvmapAlloc) : sizeof(NvmapAllocKind)) &&
        gNvmapAllocLogged < 16u) {
        const NvmapAlloc* a = (const NvmapAlloc*)src;
        unsigned kindb = 0;
        if (nr == 100)
            kindb = ((const NvmapAllocKind*)src)->kind;
        gNvmapAllocLogged++;
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "nvmap %s fd=%d handle=%u heap=%s(0x%x) flags=0x%x align=%u%s -> %s%s",
                kNvmapNrs[slot].name, fd, a->handle, heapName(a->heap_mask), a->heap_mask,
                a->flags, a->align, (nr == 100) ? (kindb ? " kind" : " kind0") : "",
                ret == 0 ? "ok" : "FALLO", ret == 0 ? "" : strerror(errno));
        return;
    }

    /* MMAP_32: que trozo del buffer se mapea (offset+length) -> dice donde cree el blob
     * que estan sus planos/dentro del handle. */
    if (nr == 5 && src != nullptr && srcSize >= sizeof(NvmapMap32) &&
        gNvmapMapLogged < 16u) {
        const NvmapMap32* a = (const NvmapMap32*)src;
        gNvmapMapLogged++;
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "nvmap MMAP_32 fd=%d handle=%u off=%u len=%u flags=0x%x addr=0x%x -> %s%s",
                fd, a->handle, a->offset, a->length, a->flags, a->addr,
                ret == 0 ? "ok" : "FALLO", ret == 0 ? "" : strerror(errno));
        return;
    }

    if (nr == 6 || nr == 7) {
        const NvmapRw* a = nullptr;
        const char* modo = "args post-llamada (en un rw FALLADO el kernel no llega a escribir count, asi que esto ES el argumento del blob; en uno exitoso count = bytes copiados)";
        if (src != nullptr && srcSize >= sizeof(NvmapRw)) {
            a = (const NvmapRw*)src;
            if (pre != nullptr)
                modo = "args previos a la llamada";
        }
        if (a == nullptr) {
            if (gNvmapErrLogged < 24u && ret != 0) {
                gNvmapErrLogged++;
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "nvmap %s FALLO (%s) sin copia de argumentos (tam=%u no cabe en 40): fd=%d",
                        nr == 6 ? "WRITE" : "READ", strerror(errno), size, fd);
            }
            return;
        }
        char hn[48];
        bool esfd = fdNameOf((int)a->handle, hn, sizeof hn);
        if (ret != 0) {
            if (gNvmapErrLogged < 24u) {
                gNvmapErrLogged++;
                const char* why;
                if (!a->addr || !a->count || !a->elem_size)
                    why = "addr/count/elem_size en 0 -> nvmap_ioctl.c:545";
                else if (!esfd)
                    why = "el handle no es un fd propio -> unmarshal_user_handle()=0 -> nvmap_ioctl.c:549";
                else
                    why = "geometria (una de las 6 clausulas de rw_handle; el kernel dice cual y el tamano real del handle con 'mocha rw EINVAL')";
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "nvmap %s FALLO (%s) por %s [%s] fd=%d handle=%u(%s) off=%u elem=%u "
                        "hstride=%u ustride=%u count=%u addr=0x%x",
                        nr == 6 ? "WRITE" : "READ", strerror(errno), why, modo, fd,
                        a->handle, esfd ? hn : "NO-ES-FD-DE-ESTE-PROCESO",
                        a->offset, a->elem_size, a->hmem_stride, a->user_stride,
                        a->count, a->addr);
            }
        } else if (gNvmapRwLogged < 6u) {
            gNvmapRwLogged++;
            __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                    "nvmap %s ok fd=%d handle=%u(%s) off=%u elem=%u hstride=%u ustride=%u "
                    "count=%u addr=0x%x",
                    nr == 6 ? "WRITE" : "READ", fd, a->handle,
                    esfd ? hn : "NO-ES-FD-DE-ESTE-PROCESO", a->offset, a->elem_size,
                    a->hmem_stride, a->user_stride, a->count, a->addr);
        }
        return;
    }

    if (nr == 12 && src != nullptr && srcSize >= sizeof(NvmapCache)) {
        const NvmapCache* a = (const NvmapCache*)src;
        if (ret != 0 && gNvmapErrLogged < 24u) {
            gNvmapErrLogged++;
            __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                    "nvmap CACHE_32 FALLO (%s) handle=%u addr=0x%x len=%u op=%d",
                    strerror(errno), a->handle, a->addr, a->len, a->op);
        }
        return;
    }

    if (ret != 0 && gNvmapErrLogged < 24u) {
        gNvmapErrLogged++;
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "nvmap nr=%u tam=%u fd=%d FALLO (%s)", nr, size, fd, strerror(errno));
        return;
    }

    gNvmapOther++;
    if ((gNvmapOther % 4000u) == 1u) {
        for (int i = 0; i < kNvmapSlots; i++)
            if (gNvmapCnt[i] != 0)
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "nvmap %s: %u llamadas, %u fallas", kNvmapNrs[i].name,
                        gNvmapCnt[i], gNvmapFail[i]);
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "nvmap: %u ioctls en total, %u sin clasificar", gNvmapTotal, gNvmapOther);
    }
}

extern "C" int ioctl(int fd, unsigned request, ...) {
    va_list args;
    va_start(args, request);
    void* argp = va_arg(args, void*);
    va_end(args);

    if (gRealIoctl == nullptr)
        gRealIoctl = (IoctlFn)dlsym(RTLD_NEXT, "ioctl");
    if (gRealIoctl == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (fd >= 3 && argp != nullptr && request == kLegacyRequest) {
        SubmitLegacy* legacy = (SubmitLegacy*)argp;
        SubmitCurrent translated;

        memset(&translated, 0, sizeof(translated));
        translated.submit_version = legacy->submit_version;
        translated.num_syncpt_incrs = legacy->num_syncpt_incrs;
        translated.num_cmdbufs = legacy->num_cmdbufs;
        translated.num_relocs = legacy->num_relocs;
        translated.num_waitchks = legacy->num_waitchks;
        translated.timeout = legacy->timeout;
        /* The whole point of this shim: the 32 bit fallback struct cannot
         * carry these flags and the kernel then drops the sync fence fd. */
        translated.flags = legacy->flags | extraFlags();
        translated.syncpt_incrs = legacy->syncpt_incrs;
        translated.cmdbuf_exts = legacy->cmdbuf_exts;
        translated.cmdbufs = legacy->cmdbufs;
        translated.relocs = legacy->relocs;
        translated.reloc_shifts = legacy->reloc_shifts;
        translated.waitchks = legacy->waitchks;
        translated.waitbases = legacy->waitbases;
        translated.class_ids = legacy->class_ids;
        translated.fences = legacy->fences;

        int ret = gRealIoctl(fd, kCurrentRequest, &translated);
        gTranslations++;

        /* El kernel nos devolvio un fd de sync fence.  El blob de 2015 trata ese
         * campo como un entero: lo mete en estructuras internas y NUNCA lo cierra
         * (medido: 1589 submits en 10 s terminaron con 1816 descriptores abiertos
         * en el provider, 1 por submit).  Tampoco lo usa para esperar: los unicos
         * Fence::wait que vemos son sobre fd 0 y fd -1.  Asi que lo cerramos aqui
         * y devolvemos 0 (= /dev/null, siempre senalado), que es lo que el blob ya
         * tolera sin bloquearse. */
        if (ret == 0 && translated.fence > 2u) {
            /* ANTES se anadia dos veces: consumia dos huecos del sondeo por fence y
             * inflaba los contadores ('5408 senalizados' sobre '4000 vistos'). */
            fenceProbeAdd((int)translated.fence);     /* copia nuestra para medir el fence */
            if (gReapFences) {
                fenceReapAdd((int)translated.fence);
                gFencesReaped++;
                if (gFencesReaped == 1u || (gFencesReaped % 500u) == 0u)
                    __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                            "submit: %u fences entregados al blob y en recoleccion "
                            "(%u ya senalados y cerrados, %u abiertos ahora)",
                            gFencesReaped, gReaperSignaled, gOpenFenceCount);
            } else if (gCloseFences) {
                close((int)translated.fence);
                translated.fence = 0;
                gFencesClosed++;
                if (gFencesClosed == 1u || (gFencesClosed % 500u) == 0u)
                    __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                            "submit: %u fences del kernel cerrados tras el submit", gFencesClosed);
            } else {
                gFencesKept++;
                if (gFencesKept == 1u || (gFencesKept % 500u) == 0u)
                    __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                            "submit: %u fences entregados sin cerrar (solo para medir)",
                            gFencesKept);
            }
        }

        /* The kernel wrote the fence into our struct; the caller expects it
         * at the very same offset in both layouts, but copy it explicitly. */
        legacy->fence = translated.fence;

        if (gTranslations <= 12u || (gTranslations % 500u) == 0u) {
            const SyncptIncr* incrs =
                    (const SyncptIncr*)(uintptr_t)legacy->syncpt_incrs;
            char sp[128];
            unsigned n = legacy->num_syncpt_incrs;
            unsigned i;
            int len = 0;

            sp[0] = '\0';
            if (incrs != nullptr && n <= 8u) {
                for (i = 0; i < n; i++) {
                    int room = (int)sizeof(sp) - len - 1;
                    int w;
                    if (room < 12)
                        break;
                    w = snprintf(sp + len, (size_t)room + 1, " %u:+%u",
                            incrs[i].syncpt_id, incrs[i].incrs);
                    if (w < 0)
                        break;
                    len += (w < room) ? w : room;
                    if (w >= room)
                        break;
                }
            } else {
                snprintf(sp, sizeof(sp), " ?");
            }

            __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                    "submit#%u ver=%u cmdbufs=%u syncpts=[%s] relocs=%u "
                    "waitchks=%u waitbases?=%s timeout=%u flags=0x%x -> %s "
                    "fence=%d (0x%x->0x%x)",
                    gTranslations, legacy->submit_version, legacy->num_cmdbufs,
                    sp, legacy->num_relocs, legacy->num_waitchks,
                    legacy->class_ids ? "yes" : "no", legacy->timeout,
                    legacy->flags, ret == 0 ? "ok" : "FAILED",
                    (int)translated.fence, kLegacyRequest, kCurrentRequest);
        }
        gFirstFence = (int)translated.fence;
        return ret;
    }

    int kind = fdKind(fd);
    unsigned magic = (request >> kIocTypeShift) & 0xffu;
    bool esNvhost = (kind == 1) || (magic == kNvhostMagic);
    bool esNvmap = (kind == 3) || (magic == kNvmapMagic);
    if (!esNvhost && !esNvmap)
        return gRealIoctl(fd, request, argp);

    /* Copia de los argumentos nvmap ANTES de que el kernel los toque (ver traceNvmapCmd).  Solo
     * si el knob esta activo: por defecto no se toca /proc/self/mem (ver argSnapOn). */
    unsigned char snap[40];
    const void* pre = nullptr;
    unsigned preSize = 0;
    if (esNvmap && argp != nullptr && argSnapOn()) {
        unsigned nr = request & 0xffu;
        unsigned size = (request >> kIocSizeShift) & 0x3fffu;
        if ((nr == 6 || nr == 7 || nr == 5 || nr == 12 || nr == 3 || nr == 100 ||
             nr == 0 || nr == 2 || nr == 13 || nr == 14 || nr == 15 || nr == 16) &&
            size <= sizeof(snap) && memTry(argp, snap, size)) {
            pre = snap;
            preSize = size;
        }
    }

    /* K-47: ver nvmapNoopOn() mas arriba.  Se hace ANTES de llamar al kernel: si la peticion es
     * de cero atomos, el shim responde exito y escribe count=0 en la struct del blob, que es
     * exactamente lo que el blob lee despues de un WRITE/READ con exito (nvmap_ioctl.c:602-612). */
    if (esNvmap && argp != nullptr) {
        unsigned nr0 = request & 0xffu;
        unsigned sz0 = (request >> kIocSizeShift) & 0x3fffu;
        if ((nr0 == 6 || nr0 == 7) && sz0 >= sizeof(NvmapRw)) {
            NvmapRw op;
            memcpy(&op, argp, sizeof(op));
            if (op.addr == 0 && op.elem_size == 0 && op.hmem_stride == 0 &&
                op.user_stride == 0 && op.count == 1 && nvmapNoopOn()) {
                if (gNvmapNoopLogged < 8u) {
                    gNvmapNoopLogged++;
                    __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                            "nvmap %s de 0 atomos (fd=%d handle=%u offs=%u) -> el shim responde "
                            "exito sin copiar; antes esto era -EINVAL y mataba el JPEG (K-47)",
                            nr0 == 6 ? "WRITE" : "READ", fd, op.handle, op.offset);
                }
                op.count = 0;
                memcpy(argp, &op, sizeof(op));
                return 0;
            }
        }
    }

    int ret = gRealIoctl(fd, request, argp);

    if (esNvmap)
        traceNvmapCmd(fd, request, pre, preSize, argp, ret);

    if (ret != 0)
        gIoctlFailed++;

    {
        int slot = -1, i;
        for (i = 0; i < gIoctlStatCount; i++) {
            if (gIoctlStats[i].request == request) { slot = i; break; }
        }
        if (slot < 0 && gIoctlStatCount < kIoctlStatSlots) {
            slot = gIoctlStatCount++;
            gIoctlStats[slot].request = request;
            gIoctlStats[slot].count = 0;
            gIoctlStats[slot].fails = 0;
            gIoctlStats[slot].fd = fd;
        }
        if (slot >= 0) {
            gIoctlStats[slot].count++;
            if (ret != 0)
                gIoctlStats[slot].fails++;
        }
    }

    if (esNvhost)
        traceSyncptCmd(fd, request, argp, ret);

    if (gIoctlLogged < 60u) {
        gIoctlLogged++;
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                "ioctl fd=%d cmd=0x%08x dir=%u tam=%u nr=%u arg=%p -> %s%s",
                fd, request,
                (request >> kIocDirShift) & 0x3u,
                (request >> kIocSizeShift) & 0x3fffu,
                request & 0xffu,
                argp, ret == 0 ? "ok" : "FALLO", ret == 0 ? "" : strerror(errno));
    } else {
        gIoctlOther++;
        if ((gIoctlOther % 5000u) == 1u) {
            int top[kIoctlStatSlots];
            int n = gIoctlStatCount, i, j, k;

            __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                    "ioctl nvhost: %u llamadas mas (%u fallidas); cmds mas usados:",
                    gIoctlOther, gIoctlFailed);
            if (n > 4)
                n = 4;
            for (i = 0; i < n; i++)
                top[i] = i;
            for (i = 1; i < n; i++) {                 /* insercion, n <= 4 */
                int v = top[i];
                j = i - 1;
                while (j >= 0 && gIoctlStats[top[j]].count < gIoctlStats[v].count) {
                    top[j + 1] = top[j];
                    j--;
                }
                top[j + 1] = v;
            }
            /* una linea por cmd: concatenar en un buffer fijo hacia que snprintf devolviera
             * la longitud deseada, len se pasara del tamano y el fortify abortaba el provider */
            for (k = 0; k < n; k++) {
                const IoctlStat* st = &gIoctlStats[top[k]];
                __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                        "   fd=%d%s cmd=0x%08x nr=%u dir=%u tam=%u x=%u fallas=%u",
                        st->fd,
                        (st->fd >= 3 && st->fd < kCacheSize) ? gFdName[st->fd] : "",
                        st->request, st->request & 0xffu,
                        (st->request >> kIocDirShift) & 0x3u,
                        (st->request >> kIocSizeShift) & 0x3fffu,
                        st->count, st->fails);
            }
        }
    }
    return ret;
}
