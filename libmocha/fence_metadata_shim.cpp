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

/*
 * Compatibility shims for the NVIDIA Tegra camera vendor blob
 * (camera.vendor.tegra.so, built against the Android 4.x camera APIs).
 *
 * The blob references a handful of C++ symbols that no longer exist in
 * Android 12 libraries.  Each stub below reproduces the old ABI exactly:
 * ARM C++ member functions pass "this" in r0 and, for the 8-byte
 * sp<> return of createEventQueue(), an sret pointer in r0 with "this"
 * moved to r1 (see the matching signatures in sensor.c).
 */

#define LOG_TAG "MochaCameraShim"

#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include <camera/CameraMetadata.h>

/*
 * fdIsSyncFence - ¿es un fence de verdad?
 *
 * Medido 2026-10-31 23:12 (probe_nvhost.sh, sesion limpia de 15 s con la camara trasera):
 *   el provider tiene 170-186 fds abiertos, de ellos 126-138 anon_inode:dmabuf,
 *   6 canales VI (/dev/nvhost-ctrl-vi, /dev/nvhost-vi), 2 ISP, 2 nodos de sensor
 *   (/dev/imx179, /dev/focuser) ... y CERO fds de tipo sync_file.
 * Es decir: el blob de 2015 construye android::Fence(fd) con un fd que NO es un
 * fence del kernel, y luego espera 2000 ms.  poll() sobre un dmabuf (o un ashmem, o
 * un fd anonimo sin poll) no devuelve nunca POLLIN y tampoco devuelve error: se
 * agota el timeout entero.  Eso es exactamente lo que se ve en logcat:
 *   E/NvCameraHal3: processCaptureRequest: Request 0: Buffer 0: Fence timed out after 2000 ms
 *   E/NvCameraHal3: processCaptureRequest: Request 1: Buffer 0: Fence timed out after 2000 ms
 * y el consiguiente 'process_capture_request ... in 2002 ms'.
 *
 * En este kernel (3.10 con drivers/staging/android/sync.c) un fence si que es un
 * anon_inode: los nombres posibles son sync_file (kernels nuevos) o
 * stagingandroid / nvhost_sync (3.10).  Cualquier otra cosa no es un fence: se trata
 * como 'ya senalizado' y no se espera nada.
 */
static int fdIsSyncFence(int fd, char *name, size_t nameSize) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = ::readlink(path, name, nameSize - 1);
    if (n <= 0) {
        if (nameSize > 0) name[0] = '\0';
        return -1; /* no se puede saber: no bloquear */
    }
    name[n] = '\0';
    if (strstr(name, "sync_file") || strstr(name, "stagingandroid") ||
        strstr(name, "nvhost_sync"))
        return 1;
    return 0;
}

/*
 * Que libreria y que offset llamo a esta funcion.
 *
 * El blob de 2015 enlaza android::Fence como simbolo undefined, o sea que su
 * constructor pasa por aqui: registrando la direccion de retorno sabemos QUIEN
 * construye un Fence con un fd que no es un fence (medido: fd 1, el pipe de
 * stdout del provider, 2026-11-01 10:37).  Sin eso no hay forma de saber si el
 * fd sale de un handle de gralloc, de un ioctl de nvhost o de una cola interna
 * de NVIDIA.
 */
static void describeCaller(void *ret, char *out, size_t outSize) {
    unsigned long addr = (unsigned long)ret;
    out[0] = '\0';
    FILE *f = ::fopen("/proc/self/maps", "r");
    if (!f)
        return;
    char line[512];
    while (::fgets(line, sizeof(line), f)) {
        unsigned long lo = 0, hi = 0;
        if (::sscanf(line, "%lx-%lx", &lo, &hi) != 2)
            continue;
        if (addr < lo || addr >= hi)
            continue;
        const char *name = line;
        size_t i = 0;
        while (i < sizeof(line) && line[i]) {
            if (line[i] == '/' || (line[i] == '-' && i + 1 < sizeof(line) && line[i + 1] == '>')) {
                name = line + i;
                break;
            }
            i++;
        }
        size_t nameLen = ::strcspn(name, "\n");
        ::snprintf(out, outSize, "%.*s+0x%lx", (int)nameLen, name, addr - lo);
        break;
    }
    ::fclose(f);
}

/* Traza limitada: el HAL abre muchos fences por segundo. */
static void fenceTrace(const char *fmt, ...) {
    static unsigned budget = 40;
    static unsigned dropped = 0;
    if (budget == 0) {
        dropped++;
        if ((dropped % 500u) == 0u)
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                "traza de fences: %u lineas mas omitidas", dropped);
        return;
    }
    budget--;
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, ap);
    va_end(ap);
}

/*
 * android::Fence::wait(unsigned int timeOutMs)
 *
 * The old (pre-Android 5) Fence API took an unsigned millisecond timeout
 * where 0 meant "wait forever".  Android 12 libui only exports
 * Fence::wait(int); the old mangled symbol is gone.  A Fence object is
 * just an int fd member (mFenceFd is the first field, the class has no
 * virtual methods), so polling that fd for POLLIN is exactly what the
 * legacy implementation did on top of a sync_file fd.
 */
/*
 * android::Fence::Fence(int fenceFd)
 *
 * Android 12 si exporta este constructor, pero el blob lo resolvia contra otra
 * firma; mientras nosotros lo implementamos podemos mirar el fd ANTES de que el
 * HAL lo use.  Dos cosas importan aqui:
 *   1) saber quien lo construye (describeCaller) porque fd 1 no puede salir de
 *      ningun sitio legitimo, y
 *   2) no dejar que un fd de stdio entre en un Fence: el destructor propio
 *      (_ZN7android5FenceD1Ev) cierra el fd, y cerrar el 1 del provider cierra
 *      su pipe de log; a partir de ahi cualquier open() puede quedarse con el 1
 *      y el HAL acabara usando stdout como si fuera un buffer o un fence.
 */
extern "C" void _ZN7android5FenceC1Ei(int *self, int fd) {
    char name[128], caller[220];
    int isFence = fdIsSyncFence(fd, name, sizeof(name));
    describeCaller(__builtin_return_address(0), caller, sizeof(caller));
    if (fd >= 0 && fd <= STDERR_FILENO)
        fenceTrace("Fence(fd %d): ES UN FD DE STDIO (%s), se sustituye por -1. Llamador: %s",
                   fd, isFence == 0 ? name : "sin readlink", caller[0] ? caller : "?");
    else if (isFence != 1)
        fenceTrace("Fence(fd %d) NO es un fence del kernel (%s). Llamador: %s",
                   fd, isFence == 0 ? name : "readlink fallo", caller[0] ? caller : "?");
    else
        fenceTrace("Fence(fd %d) es un fence real (%s). Llamador: %s",
                   fd, name, caller[0] ? caller : "?");
    *self = (fd >= 0 && fd <= STDERR_FILENO) ? -1 : fd;
}

extern "C" int _ZN7android5Fence4waitEj(int *self, unsigned int timeOutMs) {
    int fd = *self;
    if (fd < 0)
        return 0; /* invalid fence: treat as already signaled */

    /* Diagnostic 2026-10-31: NO cambiamos el comportamiento.  Solo miramos que tipo de
     * fd es, para saber si el blob de 2015 esta esperando un fence real del kernel o
     * otra cosa (un dmabuf, un pipe, un fd ya cerrado).  Medido con probe_nvhost.sh:
     * el provider de camara tiene 170-186 fds abiertos durante una sesion (126-138
     * anon_inode:dmabuf, 5-6 canales VI, 2 ISP, 2 nodos de sensor) y CERO fds de tipo
     * sync_file, mientras logcat dice 'Request 0: Buffer 0: Fence timed out after
     * 2000 ms'.  Si aqui sale 'NO es un fence', el wait de 2 s del HAL no pasa por
     * esta funcion y hay que buscarlo en el fence interno de NVIDIA (TVMR). */
    char name[128];
    int isFence = fdIsSyncFence(fd, name, sizeof(name));
    if (isFence != 1) {
        fenceTrace("Fence::wait(%u ms): fd %d NO es un fence del kernel (%s); se espera de todos modos",
                   timeOutMs, fd, isFence == 0 ? name : "readlink fallo");
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int timeout = (timeOutMs == 0u) ? -1 : (int)timeOutMs;
    int ret;
    do {
        ret = ::poll(&pfd, 1, timeout);
    } while (ret < 0 && errno == EINTR);

    if (ret == 0) {
        fenceTrace("Fence::wait(%u ms): fd %d (%s) NO senalizado -> -ETIMEDOUT",
                   timeOutMs, fd, name[0] ? name : "?");
        return -ETIMEDOUT;
    }
    if (ret < 0) {
        fenceTrace("Fence::wait(%u ms): fd %d (%s) poll fallo %s -> -%d",
                   timeOutMs, fd, name[0] ? name : "?", strerror(errno), errno);
        return -errno;
    }
    fenceTrace("Fence::wait(%u ms): fd %d (%s) senalizado", timeOutMs, fd, name);
    return 0;
}

/*
 * android::Fence::~Fence()
 *
 * Android 12 declares ~Fence() = default in the header, so the dtor is
 * inlined at every call site and libui exports no FenceD1Ev symbol.  The
 * Fence ctor (from libui, which the blob links against) takes ownership
 * of the fd, so the legacy dtor must close it.
 */
extern "C" void _ZN7android5FenceD1Ev(int *self) {
    int fd = *self;
    /* Nunca cerrar stdio: el destructor de Fence es el unico sitio donde el blob
     * de 2015 cierra el fd, y si el fd resulto ser 1 (el pipe de log del
     * provider) cerrar aqui deja al proceso sin stdout y con la puerta abierta a
     * que el proximo open() reutilice ese numero. */
    if (fd > STDERR_FILENO)
        ::close(fd);
    *self = -1;
}

/*
 * const camera_metadata_t* android::CameraMetadata::getAndLock()
 * void android::CameraMetadata::unlock(const camera_metadata_t*)
 *
 * The old API exposed a non-const getAndLock()/unlock() pair.  Android 12
 * CameraMetadata has the very same locking semantics, just const-ified
 * (mLocked flag + mLock mutex inside the object).  The old non-const
 * mangled symbols therefore have no match in libcamera_client; delegate
 * to the const implementations to keep using the object's own lock.
 */
extern "C" const camera_metadata_t* _ZN7android14CameraMetadata10getAndLockEv(
        android::CameraMetadata *self) {
    return static_cast<const android::CameraMetadata *>(self)->getAndLock();
}

extern "C" void _ZN7android14CameraMetadata6unlockEPK15camera_metadata(
        android::CameraMetadata *self, const camera_metadata_t *metadata) {
    static_cast<const android::CameraMetadata *>(self)->unlock(metadata);
}
