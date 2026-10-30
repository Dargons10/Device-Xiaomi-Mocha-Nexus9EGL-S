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
    if (traceFd < 0) traceFd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (traceFd < 0) return;
    char line[400];
    int length = snprintf(line, sizeof(line) - 2, "mocha-hal30 ");
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
        }
        camera3_capture_request_t legacyRequest = *request;
        legacyRequest.output_buffers = frame->vendorBuffers.data();
        legacyRequest.settings = request->settings ? frame->settings.getAndLock() : nullptr;
        traceRequest(request->frame_number, request, frame->settings);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            frames.emplace(request->frame_number, frame);
        }
        int result = vendor->ops->process_capture_request(vendor, &legacyRequest);
        if (legacyRequest.settings) frame->settings.unlock(legacyRequest.settings);
        if (result) trace("process_capture_request %u -> %d", request->frame_number, result);
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
                for (unsigned i = 0; i < stock->count; ++i)
                    if (stock->buffers[i].release_fence >= 0) ::close(stock->buffers[i].release_fence);
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
                    if (returned.release_fence >= 0) ::close(returned.release_fence);
                    fatal = true;
                    drained.notify_all();
                    continue;
                }
                auto converted = frame.clientBuffers[n];
                converted.status = returned.status;
                converted.acquire_fence = -1;
                converted.release_fence = returned.release_fence;
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
