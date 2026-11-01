/* Copyright (C) 2012 The CyanogenMod Project
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "Camera3Wrapper"
#include "Camera3Wrapper.h"
#include "CameraModuleApi.h"
#include "LegacyMetadata.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <errno.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <poll.h>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include <cutils/properties.h>
#include <sys/mman.h>
#include <hardware/camera3.h>
#include <hardware/gralloc.h>
#include <log/log.h>

namespace {
using android::CameraMetadata;

/*
 * camera.vendor.tegra.so is a HAL3.0 implementation: its camera3_capture_result_t
 * has exactly four members (frame_number, result, num_output_buffers,
 * output_buffers).  The Android 12 header adds input_buffer and partial_result
 * after them.  Copying the whole Oreo struct out of the vendor object reads past
 * the end of the 16-byte stock object: measured 2026-10-29 the wrapper handed the
 * provider garbage partial_result=2969190424 and an input_buffer whose
 * Camera3Stream* was 0, and CameraDeviceSession::constructCaptureResult() then
 * died reading stream->mId at offset 0x44 (SIGSEGV, SEGV_MAPERR, fault addr 0x44).
 * Only this prefix may ever be read from the stock callback.
 */
struct StockResult {
    uint32_t frame;
    const camera_metadata_t* metadata;
    uint32_t count;
    const camera3_stream_buffer_t* buffers;
};
static_assert(sizeof(StockResult) == 16, "mocha uses the 32-bit HAL3.0 result ABI");
static_assert(offsetof(camera3_capture_result_t, input_buffer) == sizeof(StockResult),
              "unexpected result ABI");
static_assert(offsetof(StockResult, frame) ==
                      offsetof(camera3_capture_result_t, frame_number),
              "stock frame_number offset changed");
static_assert(offsetof(StockResult, metadata) ==
                      offsetof(camera3_capture_result_t, result),
              "stock result offset changed");
static_assert(offsetof(StockResult, count) ==
                      offsetof(camera3_capture_result_t, num_output_buffers),
              "stock buffer count offset changed");
static_assert(offsetof(StockResult, buffers) ==
                      offsetof(camera3_capture_result_t, output_buffers),
              "stock buffer array offset changed");

class Camera;
struct Callbacks {
    camera3_callback_ops_t ops;
    Camera* owner;
};

/*
 * Medido 2026-10-31 22:28 (logcat /tmp/desp_222844.log; provider pid 2008, arranque
 * limpio, pantalla desbloqueada, camara trasera):
 *   E/NvCameraHal3: processCaptureRequest: Request 0: Buffer 0: Fence timed out after 2000 ms
 *   E/NvCameraHal3: processCaptureRequest: Request 1: Buffer 0: Fence timed out after 2000 ms
 *   134521 lineas de 'Waiting on the syncpt semaphore failed 0x2' en 10 s (pid 2008)
 *   0 submits legacy traducidos por nvhost_submit_shim durante ese intervalo
 * camera.vendor.tegra.so construye un android::Fence con el acquire_fence del buffer y
 * llama a su wait(2000): el _ZN7android5Fence4waitEj de la ABI antigua resuelve a
 * nuestro shim (poll(POLLIN) sobre el fd).  El fence que entrega el framework NUNCA se
 * senala por este camino, asi que cada peticion se traga los 2000 ms completos - es el
 * 'process_capture_request 0 -> 0 in 2002 ms' que ya se habia medido - y el HAL nunca
 * llega a submitar nada a nvhost, mientras su monitor de syncpoints se pasa ~10 s
 * esperando cuadros que no llegan (esa es la inundacion).
 *
 * persist.mocha.camera.acq:
 *   1 (por defecto) - esperamos el fence nosotros, con un techo corto, y entregamos -1
 *                     al HAL de 2015: su wait sale inmediato (fd invalido).
 *   0               - comportamiento anterior: duplicar el fd y dejar que espere el HAL.
 */
static const int kAcqWaitMs = 250;

static int acqPolicy() {
    static int cached = -1;
    if (cached < 0) {
        char prop[92] = {0};
        property_get("persist.mocha.camera.acq", prop, "1");
        cached = atoi(prop);
    }
    return cached;
}

/* Un sync_file senala POLLIN; un fd que no se puede esperar (EINVAL/EBADF - el gralloc
 * legacy de mocha no produce sync_file) se trata como "ya senalado": no tiene sentido
 * bloquear 2 s sobre algo que no va a cambiar nunca. */
static bool waitFenceShort(int fd, int ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        int r = ::poll(&pfd, 1, ms);
        if (r > 0) return (pfd.revents & (POLLIN | POLLERR | POLLHUP)) != 0;
        if (r == 0) return false;
        if (errno == EINTR) continue;
        return true;
    }
}

/* Describe el native_handle_t que recibe el HAL stock de 2015: sus fds (con el tipo
 * real, mirando /proc/self/fd) y sus enteros.
 *
 * Medido 2026-10-31 23:50 (traza_adquirir.sh): el blob llama a
 *   android::Fence::wait(2000 ms)
 * sobre el **fd 1**, que en el provider es un pipe de log ('pipe:[27741]'), y el shim
 * lo confirma:
 *   I/MochaCameraShim: Fence::wait(2000 ms): fd 1 NO es un fence del kernel (pipe:[27741])
 *   I/MochaCameraShim: Fence::wait(2000 ms): fd 1 (pipe:[27741]) NO senalizado -> -ETIMEDOUT
 *   E/NvCameraHal3: processCaptureRequest: Request 0: Buffer 0: Fence timed out after 2000 ms
 * El adapter pasa acquire_fence = -1 (y el shim de Fence devuelve 0 inmediato para fd
 * negativo), asi que ese 1 no sale del acquire_fence: sale de otra parte del buffer -
 * muy probablemente un entero del handle legacy que el blob interpreta como fd de
 * fence.  Esta funcion sirve para verlo. */
/*
 * Inspeccion del contenido del buffer que devuelve el blob (2026-11-01 11:19).
 * Con los fences ya reales el adapter entrega ~90 peticiones en 10 s, cada una con
 * su buffer, pero la captura de pantalla sigue sin mostrar una imagen reconocible.
 * Para separar "aqui no llegan fotogramas" de "el preview esta apuntando a una
 * habitacion oscura", mapeamos el dmabuf del buffer devuelto y sacamos estadisticas
 * de contenido.  Un encuadre fijo deberia dar muestras casi constantes entre frames
 * consecutivos; un buffer que nadie rellena se ve siempre igual (blanco, negro o
 * basura), y un buffer reutilizado con datos reales cambia de firma frame a frame.
 */
static bool traceEnabled();
static void trace(const char* fmt, ...);

unsigned gPeeks = 0;
unsigned char gLastSig[16];

static void peekBuffer(const native_handle_t* h, uint32_t frame) {
    if (h == nullptr || h->numFds <= 0) {
        trace("peek %u: handle sin fds", frame);
        return;
    }
    int fd = h->data[0];
    for (int i = 0; i < h->numFds; ++i) {
        char path[64], target[96];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", h->data[i]);
        ssize_t r = ::readlink(path, target, sizeof(target) - 1);
        if (r > 0) {
            target[r] = '\0';
            if (strstr(target, "dmabuf")) {
                fd = h->data[i];
                break;
            }
        }
    }
    size_t size = 0;
    for (int i = 0; i < h->numInts; ++i) {
        int v = h->data[h->numFds + i];
        if (v >= 0x10000 && v <= 0x8000000 && (v & 0xFFF) == 0) {
            size = (size_t)v;
            break;
        }
    }
    if (size == 0)
        size = 2u * 1024u * 1024u;
    void* map = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        trace("peek %u: fd=%d tam=%zu mmap FALLO (%s)", frame, fd, size, strerror(errno));
        return;
    }
    const unsigned char* base = (const unsigned char*)map;
    unsigned char sig[16];
    unsigned lo = 255, hi = 0, cambios = 0, muestras = 0;
    unsigned long suma = 0;
    unsigned prev = base[0];
    for (size_t i = 0; i < size; i += 4096) {
        unsigned v = base[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        suma += v;
        muestras++;
        if (v != prev) {
            cambios++;
            prev = v;
        }
    }
    for (int k = 0; k < 16; k++)
        sig[k] = base[(size_t)k * (size / 16u) + 16u];
    bool igual = gPeeks > 0 && memcmp(sig, gLastSig, sizeof(sig)) == 0;
    memcpy(gLastSig, sig, sizeof(sig));
    gPeeks++;
    trace("peek %u: fd=%d tam=%zu min=%u max=%u media=%.1f cambios=%u/%u firma=%02x%02x%02x%02x %s",
          frame, fd, size, lo, hi, muestras ? (double)suma / muestras : 0.0,
          cambios, muestras, sig[0], sig[1], sig[2], sig[3],
          igual ? "IGUAL que el anterior" : "distinto al anterior");
    /* Volcar el frame entero para poder mirarlo fuera: /data/local/tmp/mocha-frame[N].bin
     * debe existir y ser escribible (lo crea medir7.sh con adb root). */
    if (gPeeks <= 2u) {
        char ruta[64];
        snprintf(ruta, sizeof(ruta), "/data/local/tmp/mocha-frame%u.bin", gPeeks);
        int out = open(ruta, O_WRONLY | O_TRUNC);
        if (out >= 0) {
            ssize_t w = 0;
            while ((size_t)w < size) {
                ssize_t n = write(out, base + w, size - (size_t)w);
                if (n <= 0) break;
                w += n;
            }
            close(out);
            trace("peek %u: volcado %zd bytes en %s", frame, w, ruta);
        } else {
            trace("peek %u: no se pudo abrir %s (%s)", frame, ruta, strerror(errno));
        }
    }
    munmap(map, size);
}

static void describeHandle(const native_handle_t* h, char* out, size_t outSize) {
    if (!h) {
        snprintf(out, outSize, "handle=NULL");
        return;
    }
    int n = snprintf(out, outSize, "numFds=%d numInts=%d fds=[", h->numFds, h->numInts);
    if (n < 0) { out[0] = '\0'; return; }
    size_t used = (size_t)n;
    for (int i = 0; i < h->numFds && used + 1 < outSize; ++i) {
        int fd = h->data[i];
        char target[96];
        target[0] = '\0';
        if (fd >= 0) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
            ssize_t r = ::readlink(path, target, sizeof(target) - 1);
            if (r > 0) target[r] = '\0';
        }
        n = snprintf(out + used, outSize - used, "%s%d(%s)", i ? "," : "", fd,
                     target[0] ? target : "?");
        if (n < 0) break;
        used += (size_t)n;
    }
    if (used + 1 < outSize) used += (size_t)snprintf(out + used, outSize - used, "] ints=[");
    for (int i = 0; i < h->numInts && used + 1 < outSize; ++i) {
        n = snprintf(out + used, outSize - used, "%s%d", i ? "," : "", h->data[h->numFds + i]);
        if (n < 0) break;
        used += (size_t)n;
    }
    if (used + 1 < outSize) snprintf(out + used, outSize - used, "]");
}

/*
 * HAL3.0 requires the buffers of a stream to be registered before they may be
 * used; NVIDIA keeps the registration for the lifetime of the stream and never
 * looks at the handle passed inside camera3_stream_buffer_t.  Android 12 stopped
 * calling register_stream_buffers() for HAL3.2 devices, so the stock device used
 * to run with an empty registration set: no VI/ISP output was ever produced and
 * the sensor pipeline stalled (measured: 33369 x 'Waiting on the syncpt semaphore
 * failed 0x2' and zero output buffers).  This adapter registers every buffer
 * itself and pins it in the legacy gralloc, so the pointer it hands to the stock
 * HAL stays valid for as long as the stock device holds it.
 */
struct BufferReference {
    const gralloc_module_t* gralloc;
    buffer_handle_t handle;
    bool registered;
    BufferReference(const gralloc_module_t* module, buffer_handle_t buffer, bool pinned)
        : gralloc(module), handle(buffer), registered(pinned) {}
    ~BufferReference() {
        if (registered && gralloc) gralloc->unregisterBuffer(gralloc, handle);
    }
};

/* logd prunes any process that floods the primary ring, and the 2015 NVIDIA blob
 * floods it with syncpoint errors while a capture is running: that is exactly the
 * window in which this adapter's own messages are needed.  /dev/kmsg is a separate
 * ring, so a copy of every decision the adapter takes survives the flood.
 * Enabled with "setprop debug.camera.trace 1", off by default. */
static int traceFd = -1;

static bool traceEnabled() {
    static int cached = -1;
    if (cached < 0) cached = property_get_bool("debug.camera.trace", 0) ? 1 : 0;
    return cached > 0;
}

static void trace(const char* fmt, ...) {
    if (!traceEnabled()) return;
    if (traceFd < 0) {
        /* The camera provider is not root, so it normally cannot write /dev/kmsg.
         * Fall back to a pre-created world writable file (created by adb root). */
        traceFd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (traceFd < 0)
            traceFd = open("/data/local/tmp/mocha-hal30.log",
                           O_WRONLY | O_APPEND | O_CLOEXEC);
    }
    if (traceFd < 0) return;
    char line[400];
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    int length = snprintf(line, sizeof(line) - 2, "mocha-hal30 %8lld.%03lld ", ms / 1000, ms % 1000);
    if (length < 0) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + length, sizeof(line) - (size_t)length - 2, fmt, args);
    va_end(args);
    length = (int)strlen(line);
    line[length++] = '\n';
    ssize_t written = write(traceFd, line, (size_t)length);
    if (written < 0) ALOGE("Cannot write the camera trace to /dev/kmsg: %s", strerror(errno));
}

struct Stream {
    camera3_stream_t legacy = {};
    camera3_stream_t* client = nullptr;
    std::map<buffer_handle_t, std::unique_ptr<BufferReference>> buffers;
};

static bool isProcessedFormat(uint32_t format) {
    return format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED ||
           format == HAL_PIXEL_FORMAT_YCbCr_420_888 ||
           format == HAL_PIXEL_FORMAT_RGBA_8888 ||
           format == HAL_PIXEL_FORMAT_YV12;
}

static std::set<std::pair<uint32_t, uint32_t>> processedGeometry(
        const std::map<camera3_stream_t*, std::unique_ptr<Stream>>& streams) {
    std::set<std::pair<uint32_t, uint32_t>> geometry;
    for (const auto& item : streams) {
        const camera3_stream_t* stream = item.second->client;
        if (!stream || !isProcessedFormat(stream->format)) continue;
        // The encoder input stream comes and goes around a recording without
        // changing the sensor mode the stock HAL selected.  Reopening the stock
        // device while the recorder still holds camera buffers wedges the new
        // session, so encoder streams are not part of the geometry.
        if (stream->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) continue;
        geometry.emplace(stream->width, stream->height);
    }
    return geometry;
}

/* Tags the 2015 blob puts in its templates whose A12 definition has a different
 * type or no longer exists; camera_metadata_update() rejects them. */
static void eraseLegacyTags(CameraMetadata* metadata) {
    static const uint32_t kLegacyTags[] = {
        1048578, 589834, 589835, 1638400, 1638401, 1638402,
        1703936, 1769472, 1769473, 1835013,
    };
    for (uint32_t tag : kLegacyTags) {
        if (metadata->exists(tag)) metadata->erase(tag);
    }
}

struct Frame {
    CameraMetadata settings;
    CameraMetadata partials;
    std::vector<camera3_stream_buffer_t> clientBuffers;
    std::vector<camera3_stream_buffer_t> vendorBuffers;
    std::vector<bool> returned;
    size_t remaining = 0;
    bool metadataDone = false;
    bool shutterDone = false;
};

class Camera {
public:
    camera3_device_t device = {};
    camera3_device_t* vendor = nullptr;
    const camera_metadata_t* characteristics = nullptr;
    const gralloc_module_t* gralloc = nullptr;
    std::string deviceName;

    Camera() {
        callbacks.ops.process_capture_result = resultCallback;
        callbacks.ops.notify = notifyCallback;
        callbacks.owner = this;
        device.priv = this;
    }

    int initialize(const camera3_callback_ops_t* client) {
        if (!client || !client->process_capture_result || !client->notify) return -EINVAL;
        std::lock_guard<std::mutex> requestLock(requestMutex);
        if (clientCallbacks) return -ENOSYS;
        clientCallbacks = client;
        int result = vendor->ops->initialize(vendor, &callbacks.ops);
        if (result) clientCallbacks = nullptr;
        return result;
    }

    int configure(camera3_stream_configuration_t* config) {
        if (!config || !config->num_streams || !config->streams ||
                config->operation_mode != CAMERA3_STREAM_CONFIGURATION_NORMAL_MODE) return -EINVAL;
        std::lock_guard<std::mutex> requestLock(requestMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (fatal || closing || !clientCallbacks) return -ENODEV;
            // The HIDL session holds its inflight lock during configure_streams:
            // never wait for a callback under that call, the framework drains first.
            if (!frames.empty()) return -EBUSY;
        }
        std::map<camera3_stream_t*, std::unique_ptr<Stream>> next;
        std::vector<camera3_stream_t*> vendorStreams;
        unsigned processed = 0, jpeg = 0;
        for (unsigned i = 0; i < config->num_streams; ++i) {
            camera3_stream_t* client = config->streams[i];
            if (!client || client->stream_type != CAMERA3_STREAM_OUTPUT ||
                    !client->width || !client->height || next.count(client)) return -EINVAL;
            if (client->format == HAL_PIXEL_FORMAT_BLOB) ++jpeg;
            else if (isProcessedFormat(client->format) ||
                     (client->format == HAL_PIXEL_FORMAT_YV12 && streams.count(client))) ++processed;
            else return -EINVAL;
            if (processed > 3 || jpeg > 1) return -EINVAL;
            std::unique_ptr<Stream> stream(new (std::nothrow) Stream);
            if (!stream) return -ENOMEM;
            stream->client = client;
            stream->legacy = *client;
            // priv is a framework cookie at HAL3.2 offset 24.  The stock HAL does
            // not know that member and reads other fields of its own stream
            // structure there, which is how a Camera3Stream* pointer turned into
            // an unresolvable stream format in earlier revisions of this wrapper.
            stream->legacy.priv = nullptr;
            vendorStreams.push_back(&stream->legacy);
            next.emplace(client, std::move(stream));
        }
        // The stock HAL selects the sensor mode from the processed output sizes
        // and cannot change it while the sensor streams: the IMX179/OV5693 VI
        // pipeline then stalls on syncpoints and returns no buffer.  Closing the
        // stock device powers the sensor down so configure_streams can bring it
        // up again in the required mode.
        bool geometryChanged = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            geometryChanged = !streams.empty() &&
                    processedGeometry(streams) != processedGeometry(next);
        }
        if (geometryChanged) {
            ALOGI("Processed stream geometry changed; restarting stock camera device");
            int result = reopenVendor();
            if (result) {
                ALOGE("Cannot restart stock camera device: %d", result);
                for (auto& item : next) failedStreams.push_back(std::move(item.second));
                std::lock_guard<std::mutex> stateLock(stateMutex);
                fatal = true;
                return result;
            }
        }
        for (unsigned i = 0; i < config->num_streams; ++i)
            trace("client stream %u: %ux%u format 0x%x usage 0x%x rotation %d",
                  i, config->streams[i]->width, config->streams[i]->height,
                  config->streams[i]->format, config->streams[i]->usage,
                  config->streams[i]->rotation);
        camera3_stream_configuration_t legacyConfig = {};
        legacyConfig.num_streams = static_cast<uint32_t>(vendorStreams.size());
        legacyConfig.streams = vendorStreams.data();
        int result = vendor->ops->configure_streams(vendor, &legacyConfig);
        if (result) {
            ALOGE("Stock configure_streams failed: %d", result);
            trace("configure_streams -> %d (%u streams, %u processed, %u blob)",
                  result, legacyConfig.num_streams, processed, jpeg);
            for (auto& item : next) failedStreams.push_back(std::move(item.second));
            std::lock_guard<std::mutex> stateLock(stateMutex);
            fatal = true;
            return result;
        }
        for (auto& item : next) {
            Stream& stream = *item.second;
            ALOGI("Stream %ux%u format 0x%x -> 0x%x, usage 0x%x, max buffers %u",
                  stream.client->width, stream.client->height, stream.client->format,
                  stream.legacy.format, stream.legacy.usage, stream.legacy.max_buffers);
            trace("stock stream %ux%u format 0x%x -> 0x%x usage 0x%x max %u",
                  stream.client->width, stream.client->height, stream.client->format,
                  stream.legacy.format, stream.legacy.usage, stream.legacy.max_buffers);
            // The stock HAL resolves IMPLEMENTATION_DEFINED (0x22) to a concrete
            // fourcc (measured: YV12 0x32315659).  HIDL reports it as overrideFormat;
            // the mapper cannot allocate the unresolved 0x22 format.
            stream.client->format = stream.legacy.format;
            stream.client->usage = stream.legacy.usage;
            stream.client->max_buffers = stream.legacy.max_buffers;
            stream.client->priv = nullptr;
        }
        // configure_streams unlinked the previous streams; only now release the
        // pinned handles and stream objects the stock HAL may still point to.
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            streams.swap(next);
        }
        lastSettings.clear();
        return 0;
    }

    const camera_metadata_t* defaultRequest(int type) {
        if (type < CAMERA3_TEMPLATE_PREVIEW || type >= CAMERA3_TEMPLATE_COUNT) return nullptr;
        std::lock_guard<std::mutex> requestLock(requestMutex);
        if (!vendor) return nullptr;
        CameraMetadata& cached = templates[type];
        if (cached.isEmpty()) {
            const camera_metadata_t* stock = vendor->ops->construct_default_request_settings(vendor, type);
            if (!stock) return nullptr;
            camera_metadata_t* copy = clone_camera_metadata(stock);
            if (!copy) return nullptr;
            cached.acquire(copy);
            eraseLegacyTags(&cached);
            if (!mocha::normalizeRequest(&cached, characteristics))
                ALOGE("Default request template %d normalization incomplete; using it as-is", type);
        }
        const camera_metadata_t* result = cached.getAndLock();
        cached.unlock(result);
        return result;
    }

    /* Only the first requests are traced: after that the picture is stable and
     * the trace would only compete with the blob for the kernel ring. */
    void traceRequest(uint32_t number, const camera3_capture_request_t* request,
                      const CameraMetadata& settings) {
        if (!traceEnabled() || ++tracedRequests > 8) return;
        auto byte = [&settings](uint32_t tag) -> int {
            camera_metadata_ro_entry_t entry = settings.find(tag);
            return entry.count ? entry.data.u8[0] : -1;
        };
        camera_metadata_ro_entry_t fps = settings.find(ANDROID_CONTROL_AE_TARGET_FPS_RANGE);
        trace("request %u outputs=%u settings=%zu mode=%d scene=%d ae=%d af=%d lock=%d "
              "vstab=%d flash=%d fps=%dx%d",
              number, request->num_output_buffers, settings.entryCount(),
              byte(ANDROID_CONTROL_MODE), byte(ANDROID_CONTROL_SCENE_MODE),
              byte(ANDROID_CONTROL_AE_MODE), byte(ANDROID_CONTROL_AF_MODE),
              byte(ANDROID_CONTROL_AE_LOCK), byte(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE),
              byte(ANDROID_CONTROL_AE_STATE),
              fps.count ? (int)fps.data.i32[0] : -1, fps.count ? (int)fps.data.i32[1] : -1);
        for (unsigned i = 0; i < request->num_output_buffers; ++i) {
            const camera3_stream_t* stream = request->output_buffers[i].stream;
            trace("  output %u: %ux%u format 0x%x usage 0x%x type %d", i,
                  stream->width, stream->height, stream->format, stream->usage,
                  stream->stream_type);
        }
    }

    int capture(camera3_capture_request_t* request) {
        if (!request || request->input_buffer || !request->num_output_buffers ||
                !request->output_buffers) return -EINVAL;
        std::lock_guard<std::mutex> requestLock(requestMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (fatal || closing || !clientCallbacks) return -ENODEV;
            if (frames.count(request->frame_number)) return -EINVAL;
        }
        bool newBuffer = false;
        std::vector<Stream*> requestedStreams;
        for (unsigned i = 0; i < request->num_output_buffers; ++i) {
            const auto& buffer = request->output_buffers[i];
            auto found = streams.find(buffer.stream);
            if (found == streams.end() || !buffer.buffer || !*buffer.buffer ||
                    std::find(requestedStreams.begin(), requestedStreams.end(), found->second.get()) !=
                        requestedStreams.end()) return -EINVAL;
            requestedStreams.push_back(found->second.get());
            if (!found->second->buffers.count(*buffer.buffer)) newBuffer = true;
        }
        if (newBuffer) {
            // Re-registering a stream replaces its whole buffer set, which is
            // only safe once every earlier result and buffer was delivered.
            int result = drain();
            if (result) return result;
            for (unsigned i = 0; i < request->num_output_buffers; ++i) {
                Stream& stream = *requestedStreams[i];
                buffer_handle_t handle = *request->output_buffers[i].buffer;
                if (stream.buffers.count(handle)) continue;
                bool pinned = false;
                if (gralloc) {
                    // gralloc.tegra.so is the module the stock HAL itself uses for
                    // these handles; when the module chosen by hw_get_module() is not
                    // the one that created them, registration only fails and the stock
                    // HAL still gets its own import to work with.
                    pinned = gralloc->registerBuffer(gralloc, handle) == 0;
                    if (!pinned) ALOGE("legacy gralloc cannot register buffer %p for %ux%u", handle,
                                        stream.legacy.width, stream.legacy.height);
                }
                std::unique_ptr<BufferReference> ref(new (std::nothrow) BufferReference(gralloc, handle, pinned));
                if (!ref) return -ENOMEM;
                stream.buffers.emplace(handle, std::move(ref));
                std::vector<buffer_handle_t*> handles;
                for (auto& item : stream.buffers) handles.push_back(&item.second->handle);
                camera3_stream_buffer_set_t set = {&stream.legacy,
                    static_cast<uint32_t>(handles.size()), handles.data()};
                result = vendor->ops->register_stream_buffers(vendor, &set);
                if (result) {
                    ALOGE("Stock register_stream_buffers failed: %d", result);
                    std::lock_guard<std::mutex> stateLock(stateMutex);
                    fatal = true;
                    return result;
                }
                ALOGI("Registered %zu buffers for stream %ux%u format 0x%x",
                      handles.size(), stream.legacy.width, stream.legacy.height, stream.legacy.format);
                trace("registered %zu buffers for %ux%u format 0x%x (pinned=%d)",
                      handles.size(), stream.legacy.width, stream.legacy.height,
                      stream.legacy.format, pinned ? 1 : 0);
            }
        }
        std::shared_ptr<Frame> frame = std::make_shared<Frame>();
        if (request->settings) {
            frame->settings = request->settings;
            if (frame->settings.isEmpty()) return -EINVAL;
            eraseLegacyTags(&frame->settings);
            if (!mocha::normalizeRequest(&frame->settings, characteristics))
                ALOGE("Request %u normalization incomplete; sending it as-is", request->frame_number);
            // HAL3.0 identifies trigger events by these deprecated ids; HAL3.2
            // clients send START/CANCEL without maintaining the old id fields.
            auto af = frame->settings.find(ANDROID_CONTROL_AF_TRIGGER);
            if (af.count && af.data.u8[0] != ANDROID_CONTROL_AF_TRIGGER_IDLE)
                afTriggerId = afTriggerId == INT32_MAX ? 1 : afTriggerId + 1;
            auto ae = frame->settings.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER);
            if (ae.count && ae.data.u8[0] == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START)
                aeTriggerId = aeTriggerId == INT32_MAX ? 1 : aeTriggerId + 1;
            if (frame->settings.update(ANDROID_CONTROL_AF_TRIGGER_ID, &afTriggerId, 1) ||
                    frame->settings.update(ANDROID_CONTROL_AE_PRECAPTURE_ID, &aeTriggerId, 1)) return -ENOMEM;
            lastSettings = frame->settings;
        } else if (lastSettings.isEmpty()) return -EINVAL;
        frame->remaining = request->num_output_buffers;
        frame->clientBuffers.assign(request->output_buffers,
                                    request->output_buffers + request->num_output_buffers);
        frame->vendorBuffers = frame->clientBuffers;
        frame->returned.resize(request->num_output_buffers, false);
        for (unsigned i = 0; i < request->num_output_buffers; ++i) {
            Stream& stream = *requestedStreams[i];
            frame->vendorBuffers[i].stream = &stream.legacy;
            frame->vendorBuffers[i].buffer = &stream.buffers.at(*request->output_buffers[i].buffer)->handle;
            /* The HIDL device@3.2 implementation fills the fence members of the
             * camera3_stream_buffer_t it hands us.  The 2015 NVIDIA HAL builds an
             * android::Fence from acquire_fence and its destructor closes that fd,
             * so a leftover 0 closes stdin and aborts the provider (fdsan), and a
             * wait on it always consumes the full timeout.  Never let a stdio fd
             * reach the legacy device. */
            camera3_stream_buffer_t& vendor = frame->vendorBuffers[i];
            int acquire = vendor.acquire_fence;
            static unsigned long long acqTotal = 0, acqUnsignaled = 0, acqBogus = 0;
            bool verbose = request->frame_number < 8u;
            if (acquire >= 0 && acquire <= STDERR_FILENO) {
                acqTotal++;
                acqBogus++;
                if (verbose)
                    ALOGI("buffer %u: acquire_fence %d es un fd de stdio, se descarta "
                          "(acumulado total=%llu bogus=%llu)", i, acquire, acqTotal,
                          acqBogus);
                vendor.acquire_fence = -1;
            } else if (acquire >= 0) {
                acqTotal++;
                bool signaled = waitFenceShort(acquire, kAcqWaitMs);
                if (!signaled) acqUnsignaled++;
                if (verbose)
                    ALOGI("buffer %u: acquire_fence=%d signaled=%d policy=%d "
                          "(acumulado total=%llu sin_senalar=%llu bogus=%llu)",
                          i, acquire, signaled ? 1 : 0, acqPolicy(), acqTotal,
                          acqUnsignaled, acqBogus);
                /* El framework nos cede la propiedad del fd: hay que cerrarlo, si no
                 * se filtra uno por buffer y por peticion. */
                int copy = acqPolicy() == 0 ? fcntl(acquire, F_DUPFD_CLOEXEC, 3) : -1;
                /* ::close: dentro de Camera hay un metodo close(), hay que pedir el de libc */
                ::close(acquire);
                vendor.acquire_fence = copy;
            }
            vendor.release_fence = -1;
            /* Que ve exactamente el blob de 2015 por cada buffer de la peticion. */
            if (verbose) {
                char desc[384];
                /* HAL3.0: el blob hace *(*pBuffer->buffer) para quedarse con el handle.
                 * En HAL3.2+ (lo que nos da el framework) ese mismo campo apunta a un
                 * indice de buffer, no a un handle: por eso nosotros ponemos &ref.handle. */
                const buffer_handle_t paraElBlob = vendor.buffer ? *vendor.buffer : nullptr;
                describeHandle(paraElBlob, desc, sizeof(desc));
                ALOGI("buffer %u: acquire=%d release=%d status=%d indice_cliente=%d handle {%s}",
                      i, vendor.acquire_fence, vendor.release_fence, vendor.status,
                      request->output_buffers[i].buffer
                          ? (int)*request->output_buffers[i].buffer : -1,
                      desc);
            }
            trace("buffer %u: stream=%p acquire=%d release=%d status=%d",
                  i, (void*)vendor.stream, vendor.acquire_fence, vendor.release_fence,
                  vendor.status);
        }
        camera3_capture_request_t legacyRequest = *request;
        legacyRequest.output_buffers = frame->vendorBuffers.data();
        legacyRequest.settings = request->settings ? frame->settings.getAndLock() : nullptr;
        traceRequest(request->frame_number, request, frame->settings);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            frames.emplace(request->frame_number, frame);
        }
        auto t0 = std::chrono::steady_clock::now();
        int result = vendor->ops->process_capture_request(vendor, &legacyRequest);
        long long spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
        if (legacyRequest.settings) frame->settings.unlock(legacyRequest.settings);
        trace("process_capture_request %u -> %d in %lld ms", request->frame_number, result, spent);
        if (result) {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            frames.erase(request->frame_number);
            drained.notify_all();
        }
        return result;
    }

    int flush() {
        std::lock_guard<std::mutex> requestLock(requestMutex);
        // HAL3.0 has no flush entry.  Requests are serialized here, so draining
        // delivers every accepted buffer and packet without calling a reserved
        // ops slot of the stock device.
        return drain();
    }

    int close() {
        std::lock_guard<std::mutex> requestLock(requestMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            closing = true;
        }
        /* mocha: antes de cerrar el dispositivo stock hay que dejar que termine
         * lo que tenga en vuelo.  Al cambiar de camara (frontal/trasera) o de
         * modo, CameraService cierra esta dispositivo y el close() stock se
         * ejecutaba con peticiones sin completar: el HAL de 2015 seguia
         * escribiendo en buffers ya liberados y el provider petaba.
         * drain() espera hasta 3 s (los requests del HAL stock tardan ~2 s). */
        int drained = drain();
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            trace("adapter: drain previa al cierre de camera %s -> %d (inflight=%zu)",
                  deviceName.c_str(), drained, frames.size());
        }
        int result = vendor ? vendor->common.close(&vendor->common) : 0;
        trace("adapter for camera %s closed (%d)", deviceName.c_str(), result);
        vendor = nullptr;
        frames.clear();
        streams.clear();
        failedStreams.clear();
        return result;
    }

    void dump(int fd) {
        // dump() may race configure/close and must not wait for a stuck stock HAL.
        std::unique_lock<std::mutex> requestLock(requestMutex, std::try_to_lock);
        if (!requestLock.owns_lock()) {
            dprintf(fd, "Mocha HAL3.0 adapter: request busy; stock dump skipped\n");
            return;
        }
        {
            std::unique_lock<std::mutex> stateLock(stateMutex, std::try_to_lock);
            if (!stateLock.owns_lock()) {
                dprintf(fd, "Mocha HAL3.0 adapter: state busy; stock dump skipped\n");
                return;
            }
            dprintf(fd, "Mocha HAL3.0 adapter: inflight=%zu streams=%zu fatal=%d closing=%d\n",
                    frames.size(), streams.size(), fatal, closing);
        }
        if (vendor && vendor->ops->dump) vendor->ops->dump(vendor, fd);
    }

private:
    Callbacks callbacks = {};
    const camera3_callback_ops_t* clientCallbacks = nullptr;
    std::mutex requestMutex, stateMutex, callbackMutex;
    std::condition_variable drained;
    std::map<camera3_stream_t*, std::unique_ptr<Stream>> streams;
    std::vector<std::unique_ptr<Stream>> failedStreams;
    std::map<uint32_t, std::shared_ptr<Frame>> frames;
    // HAL3 templates stay alive until this outer device closes, including
    // across internal restarts of the stock device.
    std::array<CameraMetadata, CAMERA3_TEMPLATE_COUNT> templates;
    CameraMetadata lastSettings;
    int32_t afTriggerId = 0, aeTriggerId = 0;
    int tracedRequests = 0;
    bool fatal = false;
    bool closing = false;

    int reopenVendor() {
        camera_module_t* stock = get_stock_camera_module();
        if (!stock || !stock->common.methods || !stock->common.methods->open) return -ENODEV;
        if (vendor) {
            // Igual que en close(): no cerrar el dispositivo stock con requests
            // en vuelo, o el HAL escribe en buffers que ya no nos pertenecen.
            int drained = drain();
            trace("reopenVendor: drain previa al cierre de %s -> %d", deviceName.c_str(),
                  drained);
            vendor->common.close(&vendor->common);
            vendor = nullptr;
        }
        hw_device_t* opened = nullptr;
        int result = stock->common.methods->open(&stock->common, deviceName.c_str(), &opened);
        if (result) return result;
        if (!opened) return -ENODEV;
        vendor = reinterpret_cast<camera3_device_t*>(opened);
        if (vendor->common.version != CAMERA_DEVICE_API_VERSION_3_0 || !vendor->ops ||
                !vendor->ops->initialize || !vendor->ops->configure_streams ||
                !vendor->ops->register_stream_buffers ||
                !vendor->ops->construct_default_request_settings ||
                !vendor->ops->process_capture_request) {
            vendor->common.close(&vendor->common);
            vendor = nullptr;
            return -ENODEV;
        }
        result = vendor->ops->initialize(vendor, &callbacks.ops);
        if (result) {
            vendor->common.close(&vendor->common);
            vendor = nullptr;
            return result;
        }
        // The owned template clones survive the restart: construct_default_request_settings
        // promises them until the outer device is destroyed.
        trace("stock device %s reopened", deviceName.c_str());
        return 0;
    }

    int drain() {
        std::unique_lock<std::mutex> lock(stateMutex);
        bool ready = drained.wait_for(lock, std::chrono::seconds(3),
                                      [this] { return frames.empty() || fatal; });
        if (!ready) {
            ALOGE("Timed out draining %zu stock capture requests", frames.size());
            trace("drain timeout: %zu requests never finished", frames.size());
            return -ETIMEDOUT;
        }
        return fatal ? -ENODEV : 0;
    }

    void retire(uint32_t number) {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto found = frames.find(number);
        if (found != frames.end() && !found->second->remaining &&
                found->second->metadataDone && found->second->shutterDone) {
            frames.erase(found);
            drained.notify_all();
        }
    }

    static void resultCallback(const camera3_callback_ops_t* ops, const camera3_capture_result_t* raw) {
        // Never dereference 'raw' as a HAL3.2 result: the stock object is 16 bytes.
        reinterpret_cast<const Callbacks*>(ops)->owner->result(reinterpret_cast<const StockResult*>(raw));
    }

    void result(const StockResult* stock) {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        CameraMetadata metadata;
        std::vector<camera3_stream_buffer_t> output;
        bool metadataError = false;
        bool hasMetadata = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            auto found = frames.find(stock->frame);
            if (found == frames.end()) {
                ALOGE("Unexpected stock result frame %u", stock->frame);
                /* The legacy device fills camera3_stream_buffer_t entries of the
                 * HAL3.0 shape, which has no fence members.  Reading them yields
                 * whatever follows the struct, and closing such a value destroys an
                 * unrelated descriptor (fdsan aborts the provider on fd 0). */
                trace("unexpected stock result %u: %zu buffers dropped, no descriptors closed",
                      stock->frame, stock->count);
                return;
            }
            Frame& frame = *found->second;
            if (stock->metadata && !frame.metadataDone) {
                camera_metadata_ro_entry_t marker = {};
                bool partial = !find_camera_metadata_ro_entry(stock->metadata,
                        ANDROID_QUIRKS_PARTIAL_RESULT, &marker) && marker.count == 1 &&
                        marker.data.u8[0] == ANDROID_QUIRKS_PARTIAL_RESULT_PARTIAL;
                if (partial) {
                    metadataError = !mocha::mergeMetadata(&frame.partials, stock->metadata);
                } else {
                    hasMetadata = mocha::mergeMetadata(&metadata, stock->metadata) &&
                                  mocha::finishResult(&metadata, frame.partials);
                    metadataError = !hasMetadata;
                    frame.metadataDone = true;
                }
                if (metadataError) frame.metadataDone = true;
            }
            for (unsigned i = 0; i < stock->count; ++i) {
                const camera3_stream_buffer_t& returned = stock->buffers[i];
                size_t n;
                for (n = 0; n < frame.vendorBuffers.size(); ++n) {
                    const auto& expected = frame.vendorBuffers[n];
                    if (returned.stream == expected.stream && returned.buffer &&
                            *returned.buffer == *expected.buffer && !frame.returned[n]) break;
                }
                if (n == frame.vendorBuffers.size()) {
                    ALOGE("Unexpected stock buffer for frame %u", stock->frame);
                /* Never close a descriptor the legacy device claims to own: it does
                 * not own one, the field is not part of its struct. */
                    fatal = true;
                    drained.notify_all();
                    continue;
                }
                auto converted = frame.clientBuffers[n];
                converted.status = returned.status;
                converted.acquire_fence = -1;
                /* The legacy device writes no release fence; hand the buffer back
                 * without one instead of forwarding the value read past its struct. */
                converted.release_fence = -1;
                if (traceEnabled() && (gPeeks < 4u || (gPeeks % 25u) == 0u))
                    peekBuffer(returned.buffer ? *returned.buffer : nullptr, stock->frame);
                output.push_back(converted);
                frame.returned[n] = true;
                --frame.remaining;
            }
        }
        trace("stock result %u: buffers=%u metadata=%d accepted=%d partial-merge=%d",
              stock->frame, stock->count, stock->metadata != nullptr ? 1 : 0,
              hasMetadata ? 1 : 0, metadataError ? 1 : 0);
        if (metadataError) {
            camera3_notify_msg_t error = {};
            error.type = CAMERA3_MSG_ERROR;
            error.message.error.frame_number = stock->frame;
            error.message.error.error_code = CAMERA3_MSG_ERROR_RESULT;
            clientCallbacks->notify(clientCallbacks, &error);
        }
        if (hasMetadata || !output.empty()) {
            camera3_capture_result_t converted = {};
            converted.frame_number = stock->frame;
            converted.result = hasMetadata ? metadata.getAndLock() : nullptr;
            converted.num_output_buffers = static_cast<uint32_t>(output.size());
            converted.output_buffers = output.empty() ? nullptr : output.data();
            // HAL3.2 only: the stock device has no input buffers because it has
            // no reprocessing, and its partial_result member does not exist.
            converted.input_buffer = nullptr;
            converted.partial_result = hasMetadata ? 1 : 0;
            clientCallbacks->process_capture_result(clientCallbacks, &converted);
            if (converted.result) metadata.unlock(converted.result);
        }
        // Do not unblock re-registration until the client consumed this callback,
        // including its buffer handles and metadata.
        retire(stock->frame);
    }

    static void notifyCallback(const camera3_callback_ops_t* ops, const camera3_notify_msg_t* message) {
        reinterpret_cast<const Callbacks*>(ops)->owner->notify(message);
    }

    void notify(const camera3_notify_msg_t* message) {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        camera3_notify_msg_t converted = *message;
        uint32_t number = 0;
        bool hasFrame = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (message->type == CAMERA3_MSG_SHUTTER) {
                number = message->message.shutter.frame_number;
                hasFrame = true;
                auto found = frames.find(number);
                if (found != frames.end()) found->second->shutterDone = true;
            } else if (message->type == CAMERA3_MSG_ERROR) {
                number = message->message.error.frame_number;
                hasFrame = true;
                int code = message->message.error.error_code;
                auto found = frames.find(number);
                if (found != frames.end()) {
                    if (code == CAMERA3_MSG_ERROR_REQUEST || code == CAMERA3_MSG_ERROR_RESULT)
                        found->second->metadataDone = true;
                    if (code == CAMERA3_MSG_ERROR_REQUEST) found->second->shutterDone = true;
                }
                converted.message.error.error_stream = nullptr;
                for (auto& item : streams) {
                    if (&item.second->legacy == message->message.error.error_stream)
                        converted.message.error.error_stream = item.second->client;
                }
                if (code == CAMERA3_MSG_ERROR_DEVICE) { fatal = true; drained.notify_all(); }
            }
        }
        clientCallbacks->notify(clientCallbacks, &converted);
        if (message->type == CAMERA3_MSG_ERROR)
            trace("notify error frame=%u code=%d stream=%p", number,
                  message->message.error.error_code, message->message.error.error_stream);
        if (hasFrame) retire(number);
    }
};

Camera* camera(const camera3_device_t* device) { return device ? static_cast<Camera*>(device->priv) : nullptr; }
int initialize(const camera3_device_t* device, const camera3_callback_ops_t* ops) {
    return camera(device) ? camera(device)->initialize(ops) : -EINVAL;
}
int configure(const camera3_device_t* device, camera3_stream_configuration_t* streams) {
    return camera(device) ? camera(device)->configure(streams) : -EINVAL;
}
const camera_metadata_t* defaultRequest(const camera3_device_t* device, int type) {
    return camera(device) ? camera(device)->defaultRequest(type) : nullptr;
}
int capture(const camera3_device_t* device, camera3_capture_request_t* request) {
    return camera(device) ? camera(device)->capture(request) : -EINVAL;
}
int flush(const camera3_device_t* device) { return camera(device) ? camera(device)->flush() : -EINVAL; }
void dump(const camera3_device_t* device, int fd) { if (camera(device)) camera(device)->dump(fd); }
int closeDevice(hw_device_t* device) {
    if (!device) return -EINVAL;
    Camera* adapter = camera(reinterpret_cast<camera3_device_t*>(device));
    int result = adapter->close();
    delete adapter;
    return result;
}
camera3_device_ops_t operations = {
    .initialize = initialize,
    .configure_streams = configure,
    // HAL3.2 clients hand the buffers over inside each request; this adapter
    // registers them with the stock device itself (see BufferReference).
    .register_stream_buffers = nullptr,
    .construct_default_request_settings = defaultRequest,
    .process_capture_request = capture,
    .get_metadata_vendor_tag_ops = nullptr,
    .dump = dump,
    .flush = flush,
};
}  // namespace

int camera3_device_open(const hw_module_t* module, const char* name, hw_device_t** device) {
    if (!module || !name || !*name || !device) return -EINVAL;
    *device = nullptr;
    // Reject negative ids, trailing junk and the old off-by-one count boundary.
    for (const char* c = name; *c; ++c) if (*c < '0' || *c > '9') return -EINVAL;
    errno = 0;
    char* end = nullptr;
    long id = strtol(name, &end, 10);
    if (errno || *end || id < 0 || id >= 2) return -EINVAL;
    camera_info info = {};
    int result = get_mocha_camera_info(static_cast<int>(id), &info);
    if (result) return result;
    if (!info.static_camera_characteristics) {
        ALOGE("Camera %ld has no characteristics to normalize requests against", id);
        return -EINVAL;
    }
    camera_module_t* stock = get_stock_camera_module();
    if (!stock || id >= stock->get_number_of_cameras()) return -EINVAL;
    std::unique_ptr<Camera> adapter(new (std::nothrow) Camera);
    if (!adapter) return -ENOMEM;
    const hw_module_t* gralloc = nullptr;
    result = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &gralloc);
    if (result || !gralloc) {
        // Not fatal: the adapter can still register the handles with the stock HAL.
        ALOGE("Cannot load a legacy gralloc module (%d); buffers will not be pinned", result);
    } else {
        adapter->gralloc = reinterpret_cast<const gralloc_module_t*>(gralloc);
        if (!adapter->gralloc->registerBuffer || !adapter->gralloc->unregisterBuffer) {
            ALOGE("Legacy gralloc module has no register/unregister entry");
            adapter->gralloc = nullptr;
        }
    }
    adapter->deviceName = name;
    hw_device_t* opened = nullptr;
    result = stock->common.methods->open(&stock->common, name, &opened);
    if (result) {
        trace("stock open of camera %s -> %d", name, result);
        return result;
    }
    if (!opened) return -ENODEV;
    adapter->vendor = reinterpret_cast<camera3_device_t*>(opened);
    if (opened->version != CAMERA_DEVICE_API_VERSION_3_0 || !adapter->vendor->ops ||
            !adapter->vendor->ops->initialize || !adapter->vendor->ops->configure_streams ||
            !adapter->vendor->ops->register_stream_buffers ||
            !adapter->vendor->ops->construct_default_request_settings ||
            !adapter->vendor->ops->process_capture_request) {
        ALOGE("Stock camera %ld is not a complete HAL3.0 device (version 0x%x)", id, opened->version);
        trace("stock camera %ld has version 0x%x, not a HAL3.0 device", id, opened->version);
        opened->close(opened);
        return -ENODEV;
    }
    adapter->characteristics = info.static_camera_characteristics;
    adapter->device.common.tag = HARDWARE_DEVICE_TAG;
    adapter->device.common.version = CAMERA_DEVICE_API_VERSION_3_2;
    adapter->device.common.module = const_cast<hw_module_t*>(module);
    adapter->device.common.close = closeDevice;
    adapter->device.ops = &operations;
    trace("camera %ld opened through the HAL3.0 adapter (buffers pinned=%d)", id,
          adapter->gralloc ? 1 : 0);
    *device = &adapter.release()->device.common;
    ALOGI("Opened stock camera %ld as a HAL3.2 device through the HAL3.0 adapter", id);
    return 0;
}
