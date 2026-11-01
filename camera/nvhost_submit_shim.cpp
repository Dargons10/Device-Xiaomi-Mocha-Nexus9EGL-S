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
    gFdKind[fd] = strstr(name, "nvhost") ? 1 : 2;
    snprintf(gFdName[fd], sizeof(gFdName[fd]), "%s", name);
    if (gFdKind[fd] == 1 && gIoctlLogged < 40u)
        __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                            "fd %d es %s -> se trazan sus ioctl", fd, name);
    return gFdKind[fd];
}

/* bionic declares "int ioctl(int, unsigned, ...)" (bits/ioctl.h); on a 32 bit
 * build int/unsigned/unsigned long are all 32 bit registers, so the calling
 * convention of the blobs is unaffected. */
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
            fenceProbeAdd((int)translated.fence);     /* copia nuestra para medir el fence */
            fenceProbeAdd((int)translated.fence);     /* copia nuestra para medir */
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

    int ret = gRealIoctl(fd, request, argp);
    bool esNvhost = (fdKind(fd) == 1) || (((request >> kIocTypeShift) & 0xffu) == kNvhostMagic);
    if (!esNvhost)
        return ret;

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
