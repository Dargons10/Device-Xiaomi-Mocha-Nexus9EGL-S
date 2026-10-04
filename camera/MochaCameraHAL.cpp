/*
 * Mocha Camera Device - HAL3 implementation
 * Camera3 device stub for Xiaomi Mi Pad
 * For LineageOS 15.1 (Android 8.1)
 */

#define LOG_TAG "MochaCameraHAL"
#define LOG_NDEBUG 0

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/native_handle.h>
#include <hardware/camera_common.h>
#include <hardware/camera3.h>
#include <hardware/gralloc.h>
#include <utils/threads.h>
#include <utils/Vector.h>
#include <system/graphics.h>
#include <system/camera_metadata.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <cstdio>
#include <dirent.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <unistd.h>
#include <sync/sync.h>

#include "MochaCameraHAL.h"
#include "CameraPipeline.h"
#include "InFlightTracker.h"
#include "JpegEncoder.h"

using namespace android;

// Forward declaration of HAL module info (defined at end of file)
extern camera_module_t HMI;

namespace mocha {

// Camera configurations
const MochaCameraInfo MochaCameraHAL::kCameras[] = {
    { 0, "IMX179", CAMERA_FACING_BACK, 90, 3264, false },
    { 1, "OV5693", CAMERA_FACING_FRONT, 270, 2592, false },
};

const int MochaCameraHAL::kNumCameras = sizeof(MochaCameraHAL::kCameras) / sizeof(MochaCameraHAL::kCameras[0]);

// Static camera characteristics cache
static camera_metadata_t* gCameraCharacteristics[2] = { nullptr, nullptr };

// --- Dynamic camera presence detection -----------------------------------
// The HAL table above is static, but a unit may be built with a sensor that
// is not populated (e.g. OV5693 on some Mi Pad 1 boards answers I2C with
// -ETIMEDOUT). Advertising an absent sensor produces a phantom camera that
// crashes on open. We probe each V4L2 capture node once and only expose the
// sensors that are actually present, remapping logical->physical camera ids.
#define MOCHA_MAX_CAMERAS 8
static bool gPresentPhys[MOCHA_MAX_CAMERAS] = { false };
static int  gPhysIdForLogical[MOCHA_MAX_CAMERAS] = { 0 };
static int  gNumPresent = -1;

static bool probeCameraPresent(int physId) {
    // Keep this path mapping in sync with CameraPipeline::open().
    const char* devPath = (physId == 0) ? "/dev/video0" : "/dev/video1";
    int fd = ::open(devPath, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        ALOGW("probe: cannot open %s (%s) -> sensor %d absent",
              devPath, strerror(errno), physId);
        return false;
    }
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    bool ok = false;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        ok = (cap.capabilities &
              (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) != 0;
    }
    ::close(fd);
    if (!ok)
        ALOGW("probe: %s exists but reports no capture capability -> absent", devPath);
    return ok;
}

static void ensureCamerasProbed() {
    if (gNumPresent >= 0) return;
    int n = 0;
    for (int i = 0; i < MochaCameraHAL::kNumCameras && n < MOCHA_MAX_CAMERAS; i++) {
        bool p = probeCameraPresent(i);
        gPresentPhys[i] = p;
        if (p) gPhysIdForLogical[n++] = i;
    }
    gNumPresent = n;
    ALOGI("camera probe: %d/%d sensors present", n, MochaCameraHAL::kNumCameras);
}

// Module callbacks
static camera_module_callbacks_t gModuleCallbacks;

// --- SIGSEGV/SIGBUS guard for gralloc lock calls ---------------------------
// The framework can free a gralloc buffer while the HAL still holds a handle
// to it (use-after-free). NvGrLock then dereferences the freed buffer object
// (e.g. locking its internal mutex) and raises SIGSEGV, killing the whole
// camera provider. We install a per-thread guard: if a SIGSEGV/SIGBUS happens
// inside a guarded gralloc lock, we siglongjmp back to the guard and skip the
// frame instead of crashing. jmp_buf + armed flag are thread-local because the
// HwBinder pool processes capture requests on multiple threads concurrently.
namespace segv_guard {

__thread jmp_buf jmpbuf;
__thread volatile sig_atomic_t armed = 0;
static volatile sig_atomic_t caught_total = 0;
static volatile sig_atomic_t installed = 0;

static void handler(int sig, siginfo_t* info, void* ucontext) {
    (void)info;
    (void)ucontext;
    if (armed) {
        caught_total++;
        siglongjmp(jmpbuf, 1);
    }
    // Not inside a guarded section: restore default disposition and re-raise so
    // genuine bugs still crash and write a tombstone.
    signal(sig, SIG_DFL);
    raise(sig);
}

static void ensure_installed() {
    if (installed) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    installed = 1;
    ALOGI("SEGFAULT guard installed for gralloc locks");
}

static uint32_t caught_count() { return (uint32_t)caught_total; }

}  // namespace segv_guard

// --- sync_fence fd leak cleanup -------------------------------------------
// The Tegra VIC kernel driver creates sync_fence fds for each captured frame
// and passes them to the camera provider process. These fences are never
// closed by the HAL, causing a massive fd leak. When the fd limit (32768)
// is hit, HandleImporter fails and the app shows a fatal error.
// This thread periodically scans /proc/self/fd and closes leaked fences.
namespace fence_cleanup {

static std::atomic<bool> gStarted{false};
static std::thread gThread;

static void loop() {
    while (true) {
        usleep(15000000); // 15 seconds

        DIR* dir = opendir("/proc/self/fd");
        if (!dir) continue;

        // Pass 1: count sync_fence fds
        int fenceCount = 0;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            char path[256];
            snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
            char target[256];
            ssize_t len = readlink(path, target, sizeof(target) - 1);
            if (len <= 0) continue;
            target[len] = '\0';
            if (strncmp(target, "anon_inode:sync_fence", 21) == 0)
                fenceCount++;
        }

        // Only clean up when count exceeds threshold (active fences at 30fps
        // are ~30-60; 200 is well above that, so we never touch live fences)
        const int threshold = 200;
        if (fenceCount <= threshold) {
            closedir(dir);
            continue;
        }

        // Pass 2: close only the excess (fenceCount - threshold)
        rewinddir(dir);
        int toClose = fenceCount - threshold;
        int closed = 0;
        while ((entry = readdir(dir)) != nullptr && closed < toClose) {
            if (entry->d_name[0] == '.') continue;
            char path[256];
            snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
            char target[256];
            ssize_t len = readlink(path, target, sizeof(target) - 1);
            if (len <= 0) continue;
            target[len] = '\0';
            if (strncmp(target, "anon_inode:sync_fence", 21) == 0) {
                int fd = atoi(entry->d_name);
                close(fd);
                closed++;
            }
        }
        closedir(dir);

        if (closed > 0) {
            ALOGI("Fence cleanup: closed %d leaked sync_fence fds (total was %d, threshold %d)",
                  closed, fenceCount, threshold);
        }
    }
}

static void start() {
    if (gStarted.exchange(true)) return;
    gThread = std::thread(loop);
    gThread.detach();
    ALOGI("Fence cleanup thread started");
}

}  // namespace fence_cleanup

// Runs `fn` under the SIGSEGV/SIGBUS guard. Returns 0 if fn ran to completion,
// or -EFAULT if a fault was caught (fn did not complete; the buffer was invalid).
template <typename F>
static int run_guarded(F&& fn) {
    segv_guard::ensure_installed();
    if (sigsetjmp(segv_guard::jmpbuf, 1) == 0) {
        segv_guard::armed = 1;
        fn();
        segv_guard::armed = 0;
        return 0;
    }
    // Reached via siglongjmp after a caught fault.
    segv_guard::armed = 0;
    ALOGE("gralloc lock fault caught (SIGSEGV/SIGBUS) - invalid/freed buffer, skipping frame (total caught=%u)",
          segv_guard::caught_count());
    return -EFAULT;
}

// Forward declarations
static int camera_device_init(const hw_module_t *module, hw_device_t **device);
static int camera_device_close(hw_device_t *device);
static int camera_device_initialize(const camera3_device_t *device, const camera3_callback_ops_t *ops);
static int camera_device_configure_streams(const camera3_device_t *device, camera3_stream_configuration_t *config);
static const camera_metadata_t* camera_device_construct_default_request_settings(const camera3_device_t *device, int type);
static int camera_device_process_capture_request(const camera3_device_t *device, camera3_capture_request_t *request);
static void camera_device_dump(const camera3_device_t *device, int fd);
static int camera_device_flush(const camera3_device_t *device);

static camera3_device_ops_t camera_device_ops = {
    .initialize = camera_device_initialize,
    .configure_streams = camera_device_configure_streams,
    .register_stream_buffers = nullptr,
    .construct_default_request_settings = camera_device_construct_default_request_settings,
    .process_capture_request = camera_device_process_capture_request,
    .get_metadata_vendor_tag_ops = nullptr,
    .dump = camera_device_dump,
    .flush = camera_device_flush,
    .reserved = { 0 },
};

struct mocha_camera_device_t {
    hw_device_t common;
    camera3_device_ops_t *ops;
    int camera_id;
    const camera3_callback_ops_t *callback_ops;
    bool is_initialized;
    bool streams_configured;

    void* pipeline;
    InFlightTracker* inflight_tracker;
    camera3_stream_t* output_stream;
    uint32_t pipeline_width;

    uint32_t pipeline_height;
    uint32_t last_config_width;
    uint32_t last_config_height;

    mocha::JpegEncoder* jpeg_encoder;
    uint8_t* temp_rgba;
    uint32_t temp_rgba_size;

    uint8_t af_mode;
    uint8_t af_trigger;
    bool af_trigger_handled;
    bool af_auto_scanned;

    const camera3_stream_t* blob_stream;
    volatile bool closing;

    /* Serializes access to dev->pipeline across the HwBinder worker threads.
       The framework can have a process_capture_request running on one binder
       thread while close()/flush()/configure_streams() run on another. Without
       this, close() deletes the pipeline out from under an in-flight capture
       (use-after-free) which crashes the provider and wedges the caller's
       closeCamera() binder transaction (the front<->back switch hang). */
    std::mutex* pipelineLock;
};

// Initialize static camera characteristics
static camera_metadata_t* init_static_characteristics(int cameraId) {
    if (cameraId < 0 || cameraId >= 2) return nullptr;
    if (gCameraCharacteristics[cameraId] != nullptr) return gCameraCharacteristics[cameraId];

    const MochaCameraInfo& cam = MochaCameraHAL::kCameras[cameraId];
    
    size_t entry_capacity = 100;
    size_t data_capacity = 8192;
    camera_metadata_t* metadata = allocate_camera_metadata(entry_capacity, data_capacity);
    if (!metadata) {
        ALOGE("Failed to allocate camera metadata");
        return nullptr;
    }

    // Camera facing (map CAMERA_FACING_* to ANDROID_LENS_FACING_*)
    uint8_t facing = (cam.facing == CAMERA_FACING_BACK)
        ? ANDROID_LENS_FACING_BACK : ANDROID_LENS_FACING_FRONT;
    add_camera_metadata_entry(metadata, ANDROID_LENS_FACING, &facing, 1);

    // Orientation
    int32_t orientation = cam.orientation;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_ORIENTATION, &orientation, 1);

    // Available stream configurations
    int32_t configs[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB, 1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB, cameraId == 0 ? 1920 : 1280, cameraId == 0 ? 1080 : 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB, cameraId == 0 ? 3264 : 1280, cameraId == 0 ? 2448 : 720, CAMERA3_STREAM_OUTPUT,
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, configs, cameraId == 0 ? sizeof(configs)/sizeof(int32_t) : 16);

    // Available min frame durations
    int64_t durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB, 1280, 720, 500000000LL,
        HAL_PIXEL_FORMAT_BLOB, cameraId == 0 ? 1920 : 1280, cameraId == 0 ? 1080 : 720, 500000000LL,
        HAL_PIXEL_FORMAT_BLOB, cameraId == 0 ? 3264 : 1280, cameraId == 0 ? 2448 : 720, 500000000LL,
    };
    int ret = add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, durations, cameraId == 0 ? sizeof(durations)/sizeof(int64_t) : 16);
    ALOGI("DEBUG: Added min frame durations, ret=%d", ret);

    // Available stall durations (format, width, height, stall_ns)
    int64_t stall_durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 0,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1280, 720, 0,
        HAL_PIXEL_FORMAT_BLOB, 1280, 720, 500000000LL,
        HAL_PIXEL_FORMAT_BLOB, cameraId == 0 ? 1920 : 1280, cameraId == 0 ? 1080 : 720, 500000000LL,
        HAL_PIXEL_FORMAT_BLOB, cameraId == 0 ? 3264 : 1280, cameraId == 0 ? 2448 : 720, 500000000LL,
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stall_durations, cameraId == 0 ? sizeof(stall_durations)/sizeof(int64_t) : 16);

    // Available processed sizes (for CameraWrapper synthesis of YUV_420_888)
    int32_t processed_sizes[] = {
        1280, 720,
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES, processed_sizes, sizeof(processed_sizes)/sizeof(int32_t));

    // Available JPEG sizes (for CameraWrapper synthesis)
    int32_t jpeg_sizes[] = {
        1280, 720,
        cameraId == 0 ? 1920 : 1280, cameraId == 0 ? 1080 : 720,
        cameraId == 0 ? 3264 : 1280, cameraId == 0 ? 2448 : 720,
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_JPEG_SIZES, jpeg_sizes, cameraId == 0 ? sizeof(jpeg_sizes)/sizeof(int32_t) : 2);

    // Max digital zoom
    float max_digital_zoom = 4.0f;
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &max_digital_zoom, 1);

    // Request max pipeline depth (removed - not available in 8.1)
    
    // Flash available
    uint8_t flash_available = 0;
    add_camera_metadata_entry(metadata, ANDROID_FLASH_INFO_AVAILABLE, &flash_available, 1);

    // Sensor info
    int32_t sensor_width = cam.maxResolution;
    int32_t sensor_height = (cameraId == 0) ? 2448 : 1944;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, (int32_t[]){0, 0, sensor_width, sensor_height}, 4);
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, (int32_t[]){sensor_width, sensor_height}, 2);
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, (int32_t[]){0, 0, sensor_width, sensor_height}, 4);

    // Physical sensor size (required for FOV calculation)
    // IMX179: 3.676mm x 2.757mm (1/3.2"), OV5693: 2.8mm x 2.1mm (1/4")
    float phys_size[2];
    if (cameraId == 0) {
        phys_size[0] = 3.676f;
        phys_size[1] = 2.757f;
    } else {
        phys_size[0] = 2.8f;
        phys_size[1] = 2.1f;
    }
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_PHYSICAL_SIZE, phys_size, 2);

    // Sensor timestamp source
    int32_t sensor_timestamp_source = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &sensor_timestamp_source, 1);

    // Supported hardware level
    uint8_t hw_level = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY;
    add_camera_metadata_entry(metadata, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hw_level, 1);

    // Request available capabilities
    uint8_t capabilities[] = {
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING,
    };
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, capabilities, sizeof(capabilities)/sizeof(uint8_t));

    // Available request keys
    int32_t request_keys[] = {
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
        ANDROID_CONTROL_AF_MODE,
        ANDROID_CONTROL_AF_TRIGGER,
        ANDROID_CONTROL_AWB_MODE,
        ANDROID_COLOR_CORRECTION_MODE,
        ANDROID_CONTROL_MODE,
        ANDROID_FLASH_MODE,
        ANDROID_JPEG_QUALITY,
        ANDROID_LENS_FOCUS_DISTANCE,
        ANDROID_NOISE_REDUCTION_MODE,
        ANDROID_REQUEST_ID,
        ANDROID_SCALER_CROP_REGION,
        ANDROID_SENSOR_FRAME_DURATION,
        ANDROID_SENSOR_EXPOSURE_TIME,
        ANDROID_SENSOR_SENSITIVITY,
        ANDROID_STATISTICS_FACE_DETECT_MODE,
    };
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, request_keys, sizeof(request_keys)/sizeof(int32_t));

    // Available result keys
    int32_t result_keys[] = {
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AE_STATE,
        ANDROID_CONTROL_AF_MODE,
        ANDROID_CONTROL_AF_TRIGGER,
        ANDROID_CONTROL_AWB_MODE,
        ANDROID_CONTROL_AWB_STATE,
        ANDROID_CONTROL_MODE,
        ANDROID_FLASH_MODE,
        ANDROID_JPEG_QUALITY,
        ANDROID_LENS_FOCUS_DISTANCE,
        ANDROID_LENS_STATE,
        ANDROID_REQUEST_ID,
        ANDROID_SCALER_CROP_REGION,
        ANDROID_SENSOR_EXPOSURE_TIME,
        ANDROID_SENSOR_FRAME_DURATION,
        ANDROID_SENSOR_SENSITIVITY,
        ANDROID_SENSOR_TIMESTAMP,
    };
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, result_keys, sizeof(result_keys)/sizeof(int32_t));

    // Available scene modes (required by deriveCameraCharacteristicsKeys)
    uint8_t scene_modes[] = {
        ANDROID_CONTROL_SCENE_MODE_DISABLED,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AVAILABLE_SCENE_MODES, scene_modes, sizeof(scene_modes)/sizeof(uint8_t));

    // Available AE modes (required by deriveCameraCharacteristicsKeys)
    uint8_t ae_modes[] = {
        ANDROID_CONTROL_AE_MODE_ON,
        ANDROID_CONTROL_AE_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_MODES, ae_modes, sizeof(ae_modes)/sizeof(uint8_t));

    // Available AF modes (required by deriveCameraCharacteristicsKeys)
    uint8_t af_modes[] = {
        ANDROID_CONTROL_AF_MODE_OFF,
        ANDROID_CONTROL_AF_MODE_AUTO,
        ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_AVAILABLE_MODES, af_modes,
                              cameraId == 0 ? sizeof(af_modes)/sizeof(uint8_t) : 1);

    // Available AWB modes (required by deriveCameraCharacteristicsKeys)
    uint8_t awb_modes[] = {
        ANDROID_CONTROL_AWB_MODE_AUTO,
        ANDROID_CONTROL_AWB_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awb_modes, sizeof(awb_modes)/sizeof(uint8_t));

    // Available AE target FPS ranges (required by Parameters::initialize)
    // Note: Reference HAL uses simple FPS units (15, 30), not milli-fps
    int32_t ae_fps_ranges[] = {
        15, 30,  // 15-30 fps
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, ae_fps_ranges, sizeof(ae_fps_ranges)/sizeof(int32_t));

    // Available JPEG thumbnail sizes (required by Parameters::initialize)
    int32_t jpeg_thumbnail_sizes[] = {
        0, 0,       // No thumbnail
        160, 120,   // Small
        320, 240,   // Medium
    };
    add_camera_metadata_entry(metadata, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, jpeg_thumbnail_sizes, sizeof(jpeg_thumbnail_sizes)/sizeof(int32_t));

    // Available hot pixel modes
    uint8_t hot_pixel_modes[] = {
        ANDROID_HOT_PIXEL_MODE_FAST,
        ANDROID_HOT_PIXEL_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES, hot_pixel_modes, sizeof(hot_pixel_modes)/sizeof(uint8_t));

    // Available edge modes
    uint8_t edge_modes[] = {
        ANDROID_EDGE_MODE_OFF,
        ANDROID_EDGE_MODE_FAST,
        ANDROID_EDGE_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_EDGE_AVAILABLE_EDGE_MODES, edge_modes, sizeof(edge_modes)/sizeof(uint8_t));

    // Available noise reduction modes
    uint8_t nr_modes[] = {
        ANDROID_NOISE_REDUCTION_MODE_OFF,
        ANDROID_NOISE_REDUCTION_MODE_FAST,
        ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, nr_modes, sizeof(nr_modes)/sizeof(uint8_t));

    // Available shading modes
    uint8_t shading_modes[] = {
        ANDROID_SHADING_MODE_OFF,
        ANDROID_SHADING_MODE_FAST,
        ANDROID_SHADING_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_SHADING_AVAILABLE_MODES, shading_modes, sizeof(shading_modes)/sizeof(uint8_t));

    // Available lens shading map modes
    uint8_t lsc_map_modes[] = {
        ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES, lsc_map_modes, sizeof(lsc_map_modes)/sizeof(uint8_t));

    // Max face count (required when STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES is present)
    int32_t max_face_count = 0;
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &max_face_count, 1);

    // Available face detect modes (required by OpenCamera deriveCameraCharacteristicsKeys)
    uint8_t face_detect_modes[] = { ANDROID_STATISTICS_FACE_DETECT_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, face_detect_modes, 1);

    // Available tonemap modes
    uint8_t tonemap_modes[] = {
        ANDROID_TONEMAP_MODE_CONTRAST_CURVE,
        ANDROID_TONEMAP_MODE_FAST,
        ANDROID_TONEMAP_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES, tonemap_modes, sizeof(tonemap_modes)/sizeof(uint8_t));

    // Available cfa layout
    uint8_t cfa_layout = ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &cfa_layout, 1);

    // AE lock available
    uint8_t ae_lock_available = ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_LOCK_AVAILABLE, &ae_lock_available, 1);

    // AWB lock available
    uint8_t awb_lock_available = ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &awb_lock_available, 1);

    // AE compensation range (required by Parameters::initialize)
    int32_t ae_comp_range[] = { -9, 9 };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_COMPENSATION_RANGE, ae_comp_range, 2);

    // AE compensation step (required)
    camera_metadata_rational ae_comp_step = { 1, 3 };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_COMPENSATION_STEP, &ae_comp_step, 1);

    // Sensor exposure time range (required)
    /* IMX179 V4L2 exposure control = 10..2500 lines, line ~15.8us */
    int64_t exposure_time_range[] = { 160000LL, 40000000LL };  // 160us to 40ms
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, exposure_time_range, 2);

    // Sensor sensitivity range (required)
    int32_t sensitivity_range[] = { 100, 1600 };
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, sensitivity_range, 2);

    // Max analog sensitivity (required)
    int32_t max_analog_sensitivity = 1600;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY, &max_analog_sensitivity, 1);

    // JPEG max size (required)
    int32_t jpeg_max_size = (cameraId == 0 ? 3264 * 2448 : 1280 * 720) * 2;
    add_camera_metadata_entry(metadata, ANDROID_JPEG_MAX_SIZE, &jpeg_max_size, 1);

    // Pipeline max depth (required)
    uint8_t pipeline_depth = 4;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &pipeline_depth, 1);

    // Max input streams (required)
    int32_t max_input_streams = 0;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS, &max_input_streams, 1);

    // Lens hyperfocal distance (required)
    float hyperfocal = 0.0f;
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, &hyperfocal, 1);

    // Lens minimum focus distance (required)
    float min_focus_distance = (cameraId == 0) ? 10.0f : 0.0f;  // diopters
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &min_focus_distance, 1);

    // Focus distance calibration (required)
    uint8_t focus_cal = ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED;
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &focus_cal, 1);

    // Available effects (required)
    uint8_t effects[] = { ANDROID_CONTROL_EFFECT_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AVAILABLE_EFFECTS, effects, 1);

    // Available antibanding modes: we quantize exposure to the mains
    // half-period inside the AE loop (CameraPipeline anti-banding).
    uint8_t antibanding[] = {
        ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO,
        ANDROID_CONTROL_AE_ANTIBANDING_MODE_50HZ,
        ANDROID_CONTROL_AE_ANTIBANDING_MODE_60HZ,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, antibanding, 3);

    // Available video stabilization modes (required)
    uint8_t video_stab[] = { ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, video_stab, 1);

    // Lens optical stabilization (required)
    uint8_t optical_stab[] = { ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION, optical_stab, 1);

    // Color correction aberration modes (required)
    uint8_t cc_aberration[] = { ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES, cc_aberration, 1);

    // Available modes
    uint8_t control_modes[] = {
        ANDROID_CONTROL_MODE_AUTO,
        ANDROID_CONTROL_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AVAILABLE_MODES, control_modes, sizeof(control_modes)/sizeof(uint8_t));

    // Test pattern data modes
    uint8_t test_pattern_modes[] = {
        ANDROID_SENSOR_TEST_PATTERN_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES, test_pattern_modes, sizeof(test_pattern_modes)/sizeof(uint8_t));

    // Lens focal length (required by buildFastInfo)
    float focal_lengths[] = { 3.5f };  // ~3.5mm typical for tablet cameras
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, focal_lengths, sizeof(focal_lengths)/sizeof(float));

    // Lens aperture (required by some framework paths)
    float apertures[] = { 2.8f };
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_APERTURES, apertures, sizeof(apertures)/sizeof(float));

    // Filter density
    float filter_densities[] = { 0.0f };
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES, filter_densities, sizeof(filter_densities)/sizeof(float));

    // Max 3A regions
    int32_t max_3a_regions[] = { 1, 1, 0 };  // AE, AWB, AF
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_MAX_REGIONS, max_3a_regions, sizeof(max_3a_regions)/sizeof(int32_t));

    // Request max num output streams
    int32_t max_output_streams[] = { 3, 3, 1 };  // PREVIEW, RECORD, MAX
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, max_output_streams, sizeof(max_output_streams)/sizeof(int32_t));

    // Partial result count
    int32_t partial_result_count = 1;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partial_result_count, 1);

    // Sync max latency
    int64_t sync_max_latency = ANDROID_SYNC_MAX_LATENCY_PER_FRAME_CONTROL;
    add_camera_metadata_entry(metadata, ANDROID_SYNC_MAX_LATENCY, &sync_max_latency, 1);

    // Available characteristics keys (required by OpenCamera deriveCameraCharacteristicsKeys)
    int32_t characteristics_keys[] = {
        ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
        ANDROID_CONTROL_AE_COMPENSATION_RANGE,
        ANDROID_CONTROL_AE_COMPENSATION_STEP,
        ANDROID_CONTROL_AE_LOCK_AVAILABLE,
        ANDROID_CONTROL_AF_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_EFFECTS,
        ANDROID_CONTROL_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
        ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
        ANDROID_CONTROL_AWB_AVAILABLE_MODES,
        ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
        ANDROID_CONTROL_MAX_REGIONS,
        ANDROID_EDGE_AVAILABLE_EDGE_MODES,
        ANDROID_FLASH_INFO_AVAILABLE,
        ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
        ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
        ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
        ANDROID_JPEG_MAX_SIZE,
        ANDROID_LENS_FACING,
        ANDROID_LENS_INFO_AVAILABLE_APERTURES,
        ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES,
        ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
        ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
        ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
        ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
        ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
        ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
        ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
        ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
        ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
        ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
        ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
        ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
        ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
        ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
        ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
        ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
        ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT,
        ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
        ANDROID_SENSOR_INFO_PHYSICAL_SIZE,
        ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
        ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
        ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY,
        ANDROID_SENSOR_ORIENTATION,
        ANDROID_SHADING_AVAILABLE_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
        ANDROID_STATISTICS_INFO_MAX_FACE_COUNT,
        ANDROID_SYNC_MAX_LATENCY,
        ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES,
    };
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, characteristics_keys, sizeof(characteristics_keys)/sizeof(int32_t));

    sort_camera_metadata(metadata);
    
    // Debug: dump stream configurations
    camera_metadata_entry_t debug_configs;
    if (find_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &debug_configs) == 0 && debug_configs.count > 0) {
        ALOGI("DEBUG: Stream configs count=%zu", debug_configs.count);
        for (size_t i = 0; i < debug_configs.count; i += 4) {
            ALOGI("DEBUG: Config[%zu] format=%d width=%d height=%d input=%d",
                  i/4, debug_configs.data.i32[i], debug_configs.data.i32[i+1],
                  debug_configs.data.i32[i+2], debug_configs.data.i32[i+3]);
        }
    } else {
        ALOGE("DEBUG: No stream configs found!");
    }
    
    // Debug: dump FPS ranges
    camera_metadata_entry_t debug_fps;
    if (find_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &debug_fps) == 0 && debug_fps.count > 0) {
        ALOGI("DEBUG: FPS ranges count=%zu", debug_fps.count);
        for (size_t i = 0; i < debug_fps.count; i += 2) {
            ALOGI("DEBUG: FPS[%zu] min=%d max=%d", i/2, debug_fps.data.i32[i], debug_fps.data.i32[i+1]);
        }
    }
    
    // Debug: dump focal lengths
    camera_metadata_entry_t debug_focal;
    if (find_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &debug_focal) == 0 && debug_focal.count > 0) {
        ALOGI("DEBUG: Focal lengths count=%zu", debug_focal.count);
        for (size_t i = 0; i < debug_focal.count; i++) {
            ALOGI("DEBUG: Focal[%zu] = %f", i, debug_focal.data.f[i]);
        }
    }
    
    ALOGI("DEBUG: Metadata sorted successfully for camera %d", cameraId);
    
    gCameraCharacteristics[cameraId] = metadata;
    return metadata;
}

static int camera_device_init(const hw_module_t *module, hw_device_t **device) {
    ALOGI("camera_device_init");

    fence_cleanup::start();

    mocha_camera_device_t *dev = new mocha_camera_device_t();
    if (!dev) {
        ALOGE("Failed to allocate camera device");
        return -ENOMEM;
    }

    memset(dev, 0, sizeof(mocha_camera_device_t));
    
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CAMERA_DEVICE_API_VERSION_3_2;
    dev->common.module = const_cast<hw_module_t *>(module);
    dev->common.close = camera_device_close;
    dev->ops = &camera_device_ops;
    
    dev->camera_id = 0;
    dev->callback_ops = nullptr;
    dev->is_initialized = false;
    dev->streams_configured = false;
    dev->pipeline = nullptr;
    dev->inflight_tracker = new InFlightTracker();
    dev->output_stream = nullptr;
    dev->last_config_width = 0;
    dev->last_config_height = 0;
    dev->jpeg_encoder = nullptr;
    dev->temp_rgba = nullptr;
    dev->temp_rgba_size = 0;
    dev->af_mode = dev->camera_id == 0 ? ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE
                                         : ANDROID_CONTROL_AF_MODE_OFF;
    dev->af_trigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    dev->af_trigger_handled = false;
    dev->af_auto_scanned = false;
    dev->blob_stream = nullptr;
    dev->pipelineLock = new std::mutex();

    *device = &dev->common;
    
    ALOGI("Camera device initialized");
    return 0;
}

static int camera_device_close(hw_device_t *device) {
    ALOGI("camera_device_close");
    
    if (!device) {
        return -EINVAL;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    dev->closing = true;

    /* Wait for any in-flight process_capture_request to finish before
       tearing the pipeline down (use-after-free / provider-death source). */
    std::unique_lock<std::mutex> pl(*dev->pipelineLock);

    if (dev->jpeg_encoder) {
        delete dev->jpeg_encoder;
        dev->jpeg_encoder = nullptr;
    }
    if (dev->temp_rgba) {
        free(dev->temp_rgba);
        dev->temp_rgba = nullptr;
        dev->temp_rgba_size = 0;
    }

    if (dev->inflight_tracker) {
        dev->inflight_tracker->markAllAsError();
        delete dev->inflight_tracker;
        dev->inflight_tracker = nullptr;
    }

    if (dev->pipeline) {
        mocha::CameraPipeline* pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        pipeline->close();
        delete pipeline;
        dev->pipeline = nullptr;
    }

    pl.unlock();
    delete dev->pipelineLock;
    delete dev;
    
    ALOGI("Camera device closed");
    return 0;
}

static int camera_device_initialize(const camera3_device_t *device, const camera3_callback_ops_t *ops) {
    ALOGI("camera_device_initialize");
    
    if (!device || !ops) {
        ALOGE("Invalid parameters");
        return -EINVAL;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    dev->callback_ops = ops;
    dev->is_initialized = true;

    ALOGI("Camera initialized with callbacks");
    return 0;
}

static int camera_device_configure_streams(const camera3_device_t *device, camera3_stream_configuration_t *config) {
    ALOGI("camera_device_configure_streams: num_streams=%d", config ? config->num_streams : -1);

    if (!device || !config) {
        ALOGE("Invalid parameters");
        return -EINVAL;
    }

    // Validate num_streams to catch garbled HAL1→HAL3 fallback
    if (config->num_streams == 0 || config->num_streams > 20) {
        ALOGE("Invalid num_streams=%d, rejecting garbled config", config->num_streams);
        return -EINVAL;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;

    std::lock_guard<std::mutex> cfgLock(*dev->pipelineLock);

    // Find pipeline stream (prefer RGBA_8888, then IMPLEMENTATION_DEFINED, then any non-BLOB)
    camera3_stream_t* pipelineStream = nullptr;
    camera3_stream_t* outputStream = nullptr;
    camera3_stream_t* rgbaStream = nullptr;
    const camera3_stream_t* blobStream = nullptr;
    for (uint32_t i = 0; i < config->num_streams; i++) {
        camera3_stream_t *stream = config->streams[i];
        ALOGI("Stream %d: type=%d, width=%d, height=%d, format=%d",
              i, stream->stream_type, stream->width, stream->height, stream->format);
        
        if (stream->width == 0 || stream->height == 0) {
            ALOGE("Invalid stream dimensions");
            return -EINVAL;
        }

        if (stream->stream_type == CAMERA3_STREAM_OUTPUT) {
            stream->max_buffers = 2;
            outputStream = stream;
            if (stream->format == HAL_PIXEL_FORMAT_BLOB) {
                blobStream = stream;
            }
            if (!pipelineStream && stream->format != HAL_PIXEL_FORMAT_BLOB) {
                // Prefer RGBA_8888 for pipeline to avoid YUV format issues
                if (stream->format == HAL_PIXEL_FORMAT_RGBA_8888) {
                    pipelineStream = stream;
                    rgbaStream = stream;
                } else if (!rgbaStream && stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
                    // IMPLEMENTATION_DEFINED will be overridden to RGBA_8888
                    rgbaStream = stream;
                } else if (!pipelineStream) {
                    pipelineStream = stream;
                }
            }
        }
    }
    dev->blob_stream = blobStream;
    
    // If we found RGBA/IMPLEMENTATION_DEFINED, use that as pipeline stream
    if (rgbaStream) pipelineStream = rgbaStream;

    if (!pipelineStream) pipelineStream = const_cast<camera3_stream_t*>(blobStream);
    if (!pipelineStream) return -EINVAL;
    // Every reconfiguration returns format/usage, even if capture mode is unchanged.
    for (uint32_t i = 0; i < config->num_streams; ++i) {
        camera3_stream_t* stream = config->streams[i];
        if (stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED)
            stream->format = HAL_PIXEL_FORMAT_RGBA_8888;
        stream->usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
        // Tegra gralloc accepts flexible YUV only for camera allocations.
        // Without this flag it rejects format 35 with EINVAL.
        if (stream->format == HAL_PIXEL_FORMAT_YCbCr_420_888)
            stream->usage |= GRALLOC_USAGE_HW_CAMERA_WRITE;
    }

    // Early exit if same resolution - avoids costly STREAMOFF/STREAMON cycle
    if (dev->last_config_width == pipelineStream->width &&
        dev->last_config_height == pipelineStream->height &&
        dev->streams_configured && dev->pipeline) {
        mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        if (p->getState() == mocha::PIPELINE_STREAMING) {
            ALOGI("configureStreams: same %ux%u capture as last - no-op reinit",
                  pipelineStream->width, pipelineStream->height);
            return 0;
        }
        // NOTE: removed PIPELINE_OPENED early exit - sensor can get into bad state,
        // need full reconfigure to power cycle it
        ALOGI("configureStreams: same resolution but pipeline not streaming, reconfiguring");
    }

    // Error-complete any in-flight requests before reconfiguring
    if (dev->inflight_tracker && dev->inflight_tracker->count() > 0) {
        ALOGI("configureStreams: draining %zu in-flight requests", dev->inflight_tracker->count());
        std::vector<uint32_t> frames = dev->inflight_tracker->drainAll();
        for (uint32_t frameNum : frames) {
            camera3_capture_result_t result;
            memset(&result, 0, sizeof(result));
            result.frame_number = frameNum;
            result.result = nullptr;
            result.num_output_buffers = 0;
            result.output_buffers = nullptr;
            result.partial_result = 0;
            dev->callback_ops->process_capture_result(dev->callback_ops, &result);
            ALOGI("Drained in-flight frame %u", frameNum);
        }
    }

    // Close previous pipeline if exists
    if (dev->pipeline) {
        ALOGI("Closing previous pipeline for reconfiguration");
        mocha::CameraPipeline* pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        pipeline->close();
        delete pipeline;
        dev->pipeline = nullptr;
    }

    mocha::CameraPipeline* pipeline = new mocha::CameraPipeline();
    if (!pipeline) {
        ALOGE("Failed to create pipeline");
        return -ENOMEM;
    }

    int ret = pipeline->open(dev->camera_id);
    if (ret != 0) {
        ALOGE("Failed to open pipeline: %d (V4L2 device may not be available)", ret);
        delete pipeline;
        pipeline = nullptr;
    }

    if (!pipeline) {
        ALOGE("configureStreams: cannot proceed without pipeline");
        return -ENODEV;
    }

    mocha::PipelineConfig pipelineConfig;
    pipelineConfig.width = pipelineStream->width;
    pipelineConfig.height = pipelineStream->height;
    /* IMX179 (back) = SRGGB10 → RGGB(0). OV5693 (front) = SBGGR10 → BGGR(3). */
    pipelineConfig.bayerPattern = (dev->camera_id == 0) ? 0 : 3;
    pipelineConfig.offset_x = 0;
    pipelineConfig.offset_y = 0;
    pipelineConfig.flipV = false;  // IMX179 mount normal; framework handles rotation

    pipelineConfig.enableISP = true;

    /* Black/white level in 8-bit domain (raw10>>2).
       IMX179 OB=64 (10-bit) -> 16. OV5693 OB=15 (10-bit) -> 3. */
    pipelineConfig.blackLevel = (dev->camera_id == 0) ? 16 : 3;
    pipelineConfig.whiteLevel = 255;
    pipelineConfig.wbGain[0] = 1.0f;  // R gain
    pipelineConfig.wbGain[1] = 1.0f;  // G gain
    pipelineConfig.wbGain[2] = 1.0f;  // B gain
    pipelineConfig.wbGain[3] = 1.0f;
    /* CCM = identidad (desactivado).
       La srgbMatrix del tuning NO es post-AWB: rowsum=[1.67,-0.01,1.34]
       -> aplicada a blanco neutro produce púrpura (G->0). Está pensada para
       señal RAW junto a su wbGain por CCT. Usamos gray-world AWB (blanco
       neutro por construcción) + gamma sRGB, que da colores reales sin CCM. */
    pipelineConfig.ccm[0] = 1.0f; pipelineConfig.ccm[1] = 0.0f; pipelineConfig.ccm[2] = 0.0f;
    pipelineConfig.ccm[3] = 0.0f; pipelineConfig.ccm[4] = 1.0f; pipelineConfig.ccm[5] = 0.0f;
    pipelineConfig.ccm[6] = 0.0f; pipelineConfig.ccm[7] = 0.0f; pipelineConfig.ccm[8] = 1.0f;
    pipelineConfig.gamma = 0.4545f;  /* sRGB-like (1/2.2) */

    // Auto Exposure y Auto White Balance en ambas cámaras.
    // (El frontal OV5693 usa AE por registros I2C; AWB por gray-world en host.)
    pipelineConfig.enableAE = true;
    pipelineConfig.enableAWB = true;
    pipelineConfig.targetLuma = 0.42f;
    pipelineConfig.digitalGain = 1.0f;
 
    // Override IMPLEMENTATION_DEFINED to RGBA_8888 (Tegra gralloc allocates
    // RGBA for non-YUV formats). Keep YCbCr_420_888 and BLOB as-is.
    for (uint32_t i = 0; i < config->num_streams; i++) {
        camera3_stream_t *stream = config->streams[i];
        if (stream->stream_type == CAMERA3_STREAM_OUTPUT &&
            stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
            stream->format = HAL_PIXEL_FORMAT_RGBA_8888;
        }
    }
    uint32_t v4l2Format = (dev->camera_id == 0) ? V4L2_PIX_FMT_SRGGB10 : V4L2_PIX_FMT_SBGGR10;

    pipelineConfig.pixelFormat = v4l2Format;

    ret = pipeline->configure(pipelineConfig);
    if (ret != 0) {
        ALOGE("Failed to configure pipeline: %d", ret);
        pipeline->close();
        delete pipeline;
        dev->pipeline = nullptr;
        return ret;
    }

    ret = pipeline->startStreaming();
    if (ret != 0) {
        ALOGE("Failed to start streaming: %d", ret);
        pipeline->close();
        delete pipeline;
        dev->pipeline = nullptr;
        return ret;
    }

    dev->pipeline = pipeline;
    dev->output_stream = outputStream;
    dev->pipeline_width = pipelineStream->width;
    dev->pipeline_height = pipelineStream->height;
    dev->last_config_width = pipelineStream->width;
    dev->last_config_height = pipelineStream->height;

    dev->af_trigger_handled = false;
    dev->af_auto_scanned = false;
    dev->af_trigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;

    dev->streams_configured = true;
    ALOGI("Streams configured successfully: pipeline=%dx%d preview=%dx%d BLOB=%s", 
          dev->pipeline_width, dev->pipeline_height,
          pipelineStream->width, pipelineStream->height,
          (outputStream->format == HAL_PIXEL_FORMAT_BLOB) ? "yes" : "no");
    return 0;
}

static const camera_metadata_t* camera_device_construct_default_request_settings(const camera3_device_t *device, int type) {
    ALOGI("camera_device_construct_default_request_settings: type=%d", type);
    
    if (!device) {
        ALOGE("Null device pointer");
        return nullptr;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    int cameraId = dev->camera_id;
    
    size_t entry_capacity = 20;
    size_t data_capacity = 80;
    camera_metadata_t* metadata = allocate_camera_metadata(entry_capacity, data_capacity);
    if (!metadata) {
        ALOGE("Failed to allocate metadata");
        return nullptr;
    }

    // Common settings for all templates
    uint8_t controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_CAPTURE_INTENT, &controlIntent, 1);
    uint8_t metadataMode = ANDROID_REQUEST_METADATA_MODE_FULL;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_METADATA_MODE, &metadataMode, 1);
    int32_t requestId = 0;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_ID, &requestId, 1);
    
    int32_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
    int32_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
    int32_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_MODE, &controlMode, 1);
    int32_t sceneMode = ANDROID_CONTROL_SCENE_MODE_DISABLED;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_SCENE_MODE, &sceneMode, 1);
    int32_t aeTargetFpsRange[] = {15, 30};
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, aeTargetFpsRange, 2);
    int32_t aePrecaptureTrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &aePrecaptureTrigger, 1);
    uint8_t afMode = cameraId == 0 ? ANDROID_CONTROL_AF_MODE_AUTO
                                   : ANDROID_CONTROL_AF_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_MODE, &afMode, 1);
    uint8_t afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_TRIGGER, &afTrigger, 1);
    int32_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
    int32_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_LOCK, &awbLock, 1);
    int32_t effectMode = ANDROID_CONTROL_EFFECT_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_EFFECT_MODE, &effectMode, 1);
    uint8_t antibandingMode = ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_ANTIBANDING_MODE, &antibandingMode, 1);
    int32_t videoStabilizationMode = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &videoStabilizationMode, 1);
    int32_t edgeMode = ANDROID_EDGE_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_EDGE_MODE, &edgeMode, 1);
    int32_t nrMode = ANDROID_NOISE_REDUCTION_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_NOISE_REDUCTION_MODE, &nrMode, 1);
    int32_t colorCorrectMode = ANDROID_COLOR_CORRECTION_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_COLOR_CORRECTION_MODE, &colorCorrectMode, 1);
    int32_t transformMatrix[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    add_camera_metadata_entry(metadata, ANDROID_COLOR_CORRECTION_TRANSFORM, transformMatrix, 9);
    int32_t gains[] = {1, 0, 1, 0};
    add_camera_metadata_entry(metadata, ANDROID_COLOR_CORRECTION_GAINS, gains, 4);
    int32_t tonemapMode = ANDROID_TONEMAP_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_TONEMAP_MODE, &tonemapMode, 1);
    int32_t shadingMode = ANDROID_SHADING_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_SHADING_MODE, &shadingMode, 1);
    int32_t lensShadingMapMode = ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, &lensShadingMapMode, 1);
    int32_t hotPixelMode = ANDROID_HOT_PIXEL_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_HOT_PIXEL_MODE, &hotPixelMode, 1);
    int32_t faceDetectMode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_FACE_DETECT_MODE, &faceDetectMode, 1);
    int32_t testPatternMode = ANDROID_SENSOR_TEST_PATTERN_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_TEST_PATTERN_MODE, &testPatternMode, 1);

    // Template-specific settings
    switch (type) {
        case CAMERA3_TEMPLATE_PREVIEW:
            ALOGI("Using preview template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
            break;
        case CAMERA3_TEMPLATE_VIDEO_RECORD:
            ALOGI("Using video record template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
            break;
        case CAMERA3_TEMPLATE_STILL_CAPTURE: {
            ALOGI("Using still capture template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
            uint8_t jpegQuality = 95;
            add_camera_metadata_entry(metadata, ANDROID_JPEG_QUALITY, &jpegQuality, 1);
            uint8_t thumbnailQuality = 95;
            add_camera_metadata_entry(metadata, ANDROID_JPEG_THUMBNAIL_QUALITY, &thumbnailQuality, 1);
            int32_t thumbnailSize[] = {320, 240};
            add_camera_metadata_entry(metadata, ANDROID_JPEG_THUMBNAIL_SIZE, thumbnailSize, 2);
            break;
        }
        case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
            ALOGI("Using ZSL template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG;
            break;
        case CAMERA3_TEMPLATE_MANUAL:
            ALOGI("Using manual template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
            aeMode = ANDROID_CONTROL_AE_MODE_OFF;
            add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
            awbMode = ANDROID_CONTROL_AWB_MODE_OFF;
            add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
            break;
        default:
            ALOGW("Unknown request template %d, using preview defaults", type);
            break;
    }
    
    // Update intent after override
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_CAPTURE_INTENT, &controlIntent, 1);

    sort_camera_metadata(metadata);
    ALOGI("Request template %d created for camera %d (entries=%zu)", type, cameraId, get_camera_metadata_entry_count(metadata));
    return metadata;
}

static camera_metadata_t* build_result_metadata(uint32_t frameNumber, int64_t timestamp, int64_t exposureNs, int32_t sensitivity, int afState, int focusPos, int64_t frameDurNs, uint8_t afMode, int cameraId) {
    camera_metadata_t* metadata = allocate_camera_metadata(30, 1024);
    if (!metadata) return nullptr;

    /* ANDROID_CONTROL_AE_MODE */
    uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_MODE, &aeMode, 1);

    /* ANDROID_CONTROL_AE_STATE */
    uint8_t aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_STATE, &aeState, 1);

    /* ANDROID_CONTROL_AWB_MODE */
    uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);

    /* ANDROID_CONTROL_AWB_STATE */
    uint8_t awbState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_STATE, &awbState, 1);

    /* ANDROID_CONTROL_MODE */
    uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_MODE, &controlMode, 1);

    /* ANDROID_FLASH_MODE */
    uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_FLASH_MODE, &flashMode, 1);

    /* ANDROID_JPEG_QUALITY */
    uint8_t jpegQuality = 95;
    add_camera_metadata_entry(metadata, ANDROID_JPEG_QUALITY, &jpegQuality, 1);

    /* ANDROID_LENS_FOCUS_DISTANCE: map position 0-1023 to diopters 0.0-10.0 */
    float focusDistance = (focusPos > 0) ? 10.0f * focusPos / 1023.0f : 0.0f;
    add_camera_metadata_entry(metadata, ANDROID_LENS_FOCUS_DISTANCE, &focusDistance, 1);

    /* ANDROID_LENS_STATE */
    uint8_t lensState;
    switch (afState) {
        case 0:  lensState = ANDROID_LENS_STATE_STATIONARY; break;
        case 1:  lensState = ANDROID_LENS_STATE_MOVING; break;
        case 2:  lensState = ANDROID_LENS_STATE_STATIONARY; break;
        default: lensState = ANDROID_LENS_STATE_STATIONARY; break;
    }
    add_camera_metadata_entry(metadata, ANDROID_LENS_STATE, &lensState, 1);

    /* ANDROID_CONTROL_AF_MODE */
    uint8_t resultAfMode = afMode;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_MODE, &resultAfMode, 1);

    /* ANDROID_CONTROL_AF_TRIGGER */
    uint8_t resultAfTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_TRIGGER, &resultAfTrigger, 1);

    /* ANDROID_CONTROL_AF_STATE: pipeline 0=inactive 1=scanning 2=locked 3=failed */
    uint8_t resultAfState;
    bool continuous = (afMode == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE);
    switch (afState) {
        case 1:  resultAfState = continuous ? ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN
                                            : ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN; break;
        case 2:  resultAfState = continuous ? ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED
                                            : ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED; break;
        case 3:  resultAfState = continuous ? ANDROID_CONTROL_AF_STATE_PASSIVE_UNFOCUSED
                                            : ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED; break;
        default: resultAfState = ANDROID_CONTROL_AF_STATE_INACTIVE; break;
    }
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_STATE, &resultAfState, 1);

    /* ANDROID_REQUEST_ID */
    int32_t requestId = (int32_t)frameNumber;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_ID, &requestId, 1);

    /* ANDROID_SCALER_CROP_REGION - full sensor */
    int32_t cropRegion[] = {0, 0, cameraId == 0 ? 3264 : 2592,
                            cameraId == 0 ? 2448 : 1944};
    add_camera_metadata_entry(metadata, ANDROID_SCALER_CROP_REGION, cropRegion, 4);

    /* ANDROID_SENSOR_EXPOSURE_TIME (ns, from measured line period) */
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_EXPOSURE_TIME, &exposureNs, 1);

    /* ANDROID_SENSOR_FRAME_DURATION (ns) */
    int64_t frameDuration = frameDurNs > 0 ? frameDurNs : 39682540LL;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_FRAME_DURATION, &frameDuration, 1);

    /* ANDROID_SENSOR_SENSITIVITY */
    int32_t sensorSensitivity = sensitivity;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_SENSITIVITY, &sensorSensitivity, 1);

    /* ANDROID_SENSOR_TIMESTAMP */
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_TIMESTAMP, &timestamp, 1);

    sort_camera_metadata(metadata);
    return metadata;
}

// Copy a captured frame from one gralloc buffer to another (same format/dimensions).
// Used to fan a single captured frame out to every output stream (preview + video).
static void copyFrameBetweenBuffers(const gralloc_module_t* grallocModule,
        const camera3_stream_buffer_t* src, const camera3_stream_buffer_t* dst) {
    if (!grallocModule || !src || !dst || !src->buffer || !dst->buffer) return;
    if (src->stream->format != dst->stream->format) {
        ALOGW("copyFrameBetweenBuffers: format mismatch src=%d dst=%d",
              src->stream->format, dst->stream->format);
        return;
    }
    if (src->stream->width != dst->stream->width || src->stream->height != dst->stream->height) {
        ALOGW("copyFrameBetweenBuffers: dimension mismatch src=%ux%u dst=%ux%u",
              src->stream->width, src->stream->height, dst->stream->width, dst->stream->height);
        return;
    }
    // Both src and dst handles may be dangling (framework freed the buffer while we
    // still hold it); lock/memcpy/unlock are all guarded against SIGSEGV.
    run_guarded([&]() {
        void* srcVaddr = nullptr;
        void* dstVaddr = nullptr;
        int ret = grallocModule->lock(grallocModule, *src->buffer, GRALLOC_USAGE_SW_READ_OFTEN,
                                      0, 0, src->stream->width, src->stream->height, &srcVaddr);
        if (ret != 0 || !srcVaddr) {
            ALOGE("copyFrameBetweenBuffers: failed to lock src buffer: %d", ret);
            return;
        }
        ret = grallocModule->lock(grallocModule, *dst->buffer, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                  0, 0, dst->stream->width, dst->stream->height, &dstVaddr);
        if (ret != 0 || !dstVaddr) {
            grallocModule->unlock(grallocModule, *src->buffer);
            ALOGE("copyFrameBetweenBuffers: failed to lock dst buffer: %d", ret);
            return;
        }
        // RGBA_8888: 4 bytes/pixel. (Both preview and video streams are RGBA_8888 here.)
        size_t size = (size_t)dst->stream->width * dst->stream->height * 4;
        memcpy(dstVaddr, srcVaddr, size);
        grallocModule->unlock(grallocModule, *dst->buffer);
        grallocModule->unlock(grallocModule, *src->buffer);
    });
}

// Cache the gralloc module. hw_get_module() re-runs hw_module_exists() (realpath +
// access on every variant path) and a fresh dlopen on EVERY call; that path has
// been observed to fail intermittently (returns -ENOENT) after a few frames, which
// aborts frame processing and trips Camera3-Device's "serious error". Load once,
// reuse the handle for the lifetime of the process (gralloc HAL is never unloaded).
static const gralloc_module_t* get_gralloc_module() {
    static const gralloc_module_t* cached = nullptr;
    if (!cached) {
        hw_module_t* module = nullptr;
        int ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t**)&module);
        if (ret == 0) {
            cached = reinterpret_cast<const gralloc_module_t*>(module);
        } else {
            ALOGE("get_gralloc_module: hw_get_module failed: %d", ret);
        }
    }
    return cached;
}

static void insertExifApp1(uint8_t* jpeg, size_t* jpegSize, size_t capacity,
                           int width, int height, int rotation) {
    // APP1 length is big endian; TIFF entries below use little endian.
    uint8_t exif[78] = {};
    if (*jpegSize < 2 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
        *jpegSize > capacity || capacity - *jpegSize < sizeof(exif)) return;
    exif[0] = 0xff; exif[1] = 0xe1; exif[3] = sizeof(exif) - 2;
    memcpy(exif + 4, "Exif", 4);
    uint8_t* tiff = exif + 10;
    tiff[0] = 'I'; tiff[1] = 'I'; tiff[2] = 42; tiff[4] = 8;
    auto put32 = [](uint8_t* dst, uint32_t value) {
        for (int i = 0; i < 4; ++i) dst[i] = value >> (8 * i);
    };
    auto entry = [&](uint8_t* dst, uint16_t tag, uint16_t type, uint32_t value) {
        dst[0] = tag; dst[1] = tag >> 8;
        dst[2] = type; dst[3] = type >> 8;
        put32(dst + 4, 1); put32(dst + 8, value);
    };
    const uint32_t orientation = rotation == 90 ? 6 : rotation == 180 ? 3 :
                                 rotation == 270 ? 8 : 1;
    tiff[8] = 2;
    entry(tiff + 10, 0x0112, 3, orientation);
    entry(tiff + 22, 0x8769, 4, 38);
    tiff[38] = 2;
    entry(tiff + 40, 0xa002, 4, width);
    entry(tiff + 52, 0xa003, 4, height);
    memmove(jpeg + 2 + sizeof(exif), jpeg + 2, *jpegSize - 2);
    memcpy(jpeg + 2, exif, sizeof(exif));
    *jpegSize += sizeof(exif);
}

static int camera_device_process_capture_request(const camera3_device_t *device, camera3_capture_request_t *request) {
    if (!device || !request) {
        ALOGE("Invalid parameters");
        return -EINVAL;
    }

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    ALOGI("camera_device_process_capture_request: frame_number=%llu", (unsigned long long)request->frame_number);

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;

    /* Serialize with close()/flush()/configure_streams() pipeline teardown
       and with sibling capture threads (the V4L2 fd + demosaic/AE state are
       single-instance). */
    std::lock_guard<std::mutex> capLock(*dev->pipelineLock);

    if (dev->closing) {
        return -ENOSYS;
    }

    if (!dev->is_initialized || !dev->callback_ops) {
        ALOGE("Camera not initialized");
        return -ENOSYS;
    }

    if (request->num_output_buffers < 1 || !request->output_buffers) {
        ALOGE("No output buffers");
        return -EINVAL;
    }

    // Check if this frame was marked as error by flush()
    uint32_t frameNum = request->frame_number;
    if (dev->inflight_tracker && dev->inflight_tracker->isError(frameNum)) {
        ALOGW("Frame %u marked as error by flush, returning EAGAIN", frameNum);
        dev->inflight_tracker->remove(frameNum);
        return -EAGAIN;
    }

    // Track this request
    if (dev->inflight_tracker) {
        dev->inflight_tracker->add(frameNum, request->output_buffers[0].buffer);
    }

    // Handle AF mode and trigger from request metadata
    if (request->settings) {
        camera_metadata_t* mutableSettings = const_cast<camera_metadata_t*>(request->settings);
        camera_metadata_entry_t entry;
        if (find_camera_metadata_entry(mutableSettings, ANDROID_CONTROL_AF_MODE, &entry) == 0 && entry.count > 0) {
            dev->af_mode = entry.data.u8[0];
        }
        if (find_camera_metadata_entry(mutableSettings, ANDROID_CONTROL_AF_TRIGGER, &entry) == 0 && entry.count > 0) {
            dev->af_trigger = entry.data.u8[0];
        }
        float focusDist = -1.0f;
        if (find_camera_metadata_entry(mutableSettings, ANDROID_LENS_FOCUS_DISTANCE, &entry) == 0 && entry.count > 0) {
            focusDist = entry.data.f[0];
        }
        ALOGI("AF request: mode=%d trigger=%d focusDist=%.2f", dev->af_mode, dev->af_trigger, focusDist);
    }

    if (dev->camera_id != 0) dev->af_mode = ANDROID_CONTROL_AF_MODE_OFF;
    if (dev->pipeline && dev->camera_id == 0) {
        mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        if (dev->af_mode == ANDROID_CONTROL_AF_MODE_AUTO ||
            dev->af_mode == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE) {
            if (dev->af_trigger == ANDROID_CONTROL_AF_TRIGGER_START && !dev->af_trigger_handled) {
                ALOGI("AF TRIGGER START received");
                dev->af_auto_scanned = true;
                p->startAfScan();
                dev->af_trigger_handled = true;
            } else if (dev->af_trigger == ANDROID_CONTROL_AF_TRIGGER_CANCEL) {
                ALOGI("AF TRIGGER CANCEL received");
                p->cancelAf();
                dev->af_trigger_handled = false;
                dev->af_auto_scanned = false;
            } else if (!dev->af_auto_scanned) {
                /* Basic camera apps (AOSP Camera2) send settings on the first
                   request only and never fire AF_TRIGGER: run one passive
                   scan at session start so initial framing is sharp. */
                ALOGI("AF: mode=%d with no AF_TRIGGER from app, running initial passive scan",
                      dev->af_mode);
                dev->af_auto_scanned = true;
                p->startAfScan();
            }
            /* Pre-shutter autofocus: a still-capture request with a stale
               lock means the user recomposed since the last scan. Rescan
               now (legacy-AF style: preview stalls ~2s) so the photo is
               focused on the subject actually aimed at. */
            if (request->num_output_buffers > 0) {
                bool wantsBlob = false;
                for (uint32_t i = 0; i < request->num_output_buffers; i++) {
                    const camera3_stream_t* s = request->output_buffers[i].stream;
                    if (s && s->format == HAL_PIXEL_FORMAT_BLOB) wantsBlob = true;
                }
                if (wantsBlob && p->afAgeMs() > 3000) {
                    ALOGI("AF: shutter with stale lock (%lld ms) - rescanning before capture",
                          (long long)p->afAgeMs());
                    p->startAfScan();
                }
            }
        }
    }

    bool frameCaptured = false;
    bool streamFrameCaptured = false;
    std::vector<uint8_t> bufferCaptured(request->num_output_buffers, 0);
    bool hasBlobOutput = false;

    if (dev->closing) {
        if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
        return -ENOSYS;
    }

    if (dev->pipeline && dev->streams_configured) {
        mocha::CameraPipeline* pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);

        if (pipeline->getState() == mocha::PIPELINE_OPENED) {
            ALOGI("process_capture_request: pipeline in OPENED state, re-starting streaming");
            int restartRet = pipeline->startStreaming();
            if (restartRet != 0) {
                ALOGE("Auto-restart streaming failed: %d", restartRet);
                if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                return -ENODEV;
            }
        }

        const gralloc_module_t* grallocModule = get_gralloc_module();

        if (!grallocModule) {
            ALOGW("Failed to get gralloc module");
        } else {
            for (uint32_t i = 0; i < request->num_output_buffers; i++) {
                const camera3_stream_buffer_t& outBuf = request->output_buffers[i];
                if (!outBuf.buffer) continue;

                buffer_handle_t handle = *outBuf.buffer;
                void* vaddr = nullptr;
                bool outputCaptured = false;

                int fence = outBuf.acquire_fence;
                if (fence >= 0) {
                    int waitRet = sync_wait(fence, 3000);
                    close(fence);
                    if (waitRet < 0) {
                        ALOGE("Output acquire fence wait failed: %s", strerror(errno));
                        continue;
                    }
                }

                ALOGI("Processing output %u: frame=%llu stream=%dx%d format=%d",
                      i, (unsigned long long)request->frame_number,
                      outBuf.stream->width, outBuf.stream->height, outBuf.stream->format);

                if (outBuf.stream->format == HAL_PIXEL_FORMAT_BLOB) {
                    hasBlobOutput = true;
                    uint32_t blobW = outBuf.stream->width;
                    uint32_t blobH = outBuf.stream->height;
                    uint32_t rgbaSize = blobW * blobH * 4;
                    if (!dev->temp_rgba || dev->temp_rgba_size < rgbaSize) {
                        if (dev->temp_rgba) free(dev->temp_rgba);
                        dev->temp_rgba = (uint8_t*)malloc(rgbaSize);
                        dev->temp_rgba_size = rgbaSize;
                    }
                    if (!dev->temp_rgba) {
                        ALOGE("Failed to allocate temp RGBA buffer");
                    } else {
                        int captureRet = pipeline->captureStill(dev->temp_rgba, blobW, blobH);
                        // Still capture can replace ISP storage when restoring preview.
                        streamFrameCaptured = false;
                        if (captureRet == 0) {
                            uint8_t* encodeSrc = dev->temp_rgba;
                            int encodeW = blobW, encodeH = blobH;
                            run_guarded([&]() {
                                int ret = grallocModule->lock(grallocModule, handle,
                                                               GRALLOC_USAGE_SW_WRITE_OFTEN,
                                                               0, 0, blobW, blobH, &vaddr);
                                if (ret != 0 || !vaddr) {
                                    ALOGE("Failed to lock BLOB buffer");
                                    return;
                                }
                                if (!dev->jpeg_encoder) {
                                    dev->jpeg_encoder = new mocha::JpegEncoder();
                                }
                                size_t jpegSize = 0;
                                // Match Camera3Device::getJpegBufferSize() in Android 10.
                                const uint32_t maxArea = dev->camera_id == 0 ? 3264 * 2448 : 1280 * 720;
                                const uint32_t minSize = 256 * 1024 + sizeof(camera3_jpeg_blob_t);
                                const float scale = float(blobW * blobH) / maxArea;
                                const uint32_t blobSize = scale * (maxArea * 2 - minSize) + minSize;
                                ret = dev->jpeg_encoder->encodeRGBA(encodeSrc,
                                                                     encodeW, encodeH,
                                                                     90, (uint8_t*)vaddr, blobSize - sizeof(camera3_jpeg_blob_t), &jpegSize);
                                if (ret == 0 && jpegSize > 0) {
                                    int rotation = 0;
                                    camera_metadata_ro_entry_t orientationEntry;
                                    if (request->settings && find_camera_metadata_ro_entry(
                                            request->settings, ANDROID_JPEG_ORIENTATION,
                                            &orientationEntry) == 0 && orientationEntry.count)
                                        rotation = orientationEntry.data.i32[0];
                                    insertExifApp1((uint8_t*)vaddr, &jpegSize,
                                                   blobSize - sizeof(camera3_jpeg_blob_t),
                                                   encodeW, encodeH, rotation);
                                    camera3_jpeg_blob_t blob;
                                    blob.jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
                                    blob.jpeg_size = jpegSize;
                                    uint8_t* blobPtr = (uint8_t*)vaddr + blobSize - sizeof(blob);
                                    memcpy(blobPtr, &blob, sizeof(blob));
                                    frameCaptured = outputCaptured = true;
                                    ALOGI("JPEG captured: encode=%dx%d blob=%dx%d -> %zu bytes (with EXIF)",
                                          encodeW, encodeH, blobW, blobH, jpegSize);
                                    {
                                        uint8_t* jp = (uint8_t*)vaddr;
                                        ALOGI("JPEG bytes[0..7]: %02X %02X %02X %02X %02X %02X %02X %02X",
                                              jp[0], jp[1], jp[2], jp[3], jp[4], jp[5], jp[6], jp[7]);
                                        ALOGI("JPEG bytes[end-3..end]: %02X %02X %02X",
                                              jp[jpegSize-3], jp[jpegSize-2], jp[jpegSize-1]);
                                    }
                                } else {
                                    ALOGE("JPEG encode failed: %d", ret);
                                }
                                grallocModule->unlock(grallocModule, handle);
                            });

                        } else if (captureRet == -EAGAIN) {
                            if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                            return -EAGAIN;
                        } else {
                            ALOGE("Failed to capture frame for JPEG: %d", captureRet);
                        }
                    }
                } else if (outBuf.stream->format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
                    struct android_ycbcr ycbcr = {};
                    int captureRet = -EINVAL;
                    run_guarded([&]() {
                        if (!grallocModule->lock_ycbcr) return;
                        int ret = grallocModule->lock_ycbcr(grallocModule, handle,
                                                            GRALLOC_USAGE_SW_WRITE_OFTEN,
                                                            0, 0, outBuf.stream->width, outBuf.stream->height, &ycbcr);
                        if (ret != 0 || !ycbcr.y) {
                            ALOGE("YUV lock failed: %d", ret);
                            return;
                        }
                        const size_t w = outBuf.stream->width;
                        const size_t h = outBuf.stream->height;
                        if (!ycbcr.cb || !ycbcr.cr || ycbcr.ystride < w ||
                            ycbcr.cstride < (w / 2 - 1) * ycbcr.chroma_step + 1 ||
                            (ycbcr.chroma_step != 1 && ycbcr.chroma_step != 2)) {
                            ALOGE("Invalid YUV plane layout");
                            grallocModule->unlock(grallocModule, handle);
                            return;
                        }
                        // The ISP emits tight NV12; gralloc may expose YV12,
                        // NV21, or padded planes. Respect the returned layout.
                        std::vector<uint8_t> nv12(w * h * 3 / 2);
                        captureRet = streamFrameCaptured
                                ? pipeline->copyCurrentFrame(nv12.data(), outBuf.stream->format)
                                : pipeline->captureFrame(nv12.data(), outBuf.stream->format);
                        if (captureRet == 0) {
                            auto* y = static_cast<uint8_t*>(ycbcr.y);
                            auto* cb = static_cast<uint8_t*>(ycbcr.cb);
                            auto* cr = static_cast<uint8_t*>(ycbcr.cr);
                            for (size_t row = 0; row < h; ++row)
                                memcpy(y + row * ycbcr.ystride, nv12.data() + row * w, w);
                            for (size_t row = 0; row < h / 2; ++row) {
                                const uint8_t* uv = nv12.data() + w * h + row * w;
                                for (size_t col = 0; col < w / 2; ++col) {
                                    cb[row * ycbcr.cstride + col * ycbcr.chroma_step] = uv[2 * col];
                                    cr[row * ycbcr.cstride + col * ycbcr.chroma_step] = uv[2 * col + 1];
                                }
                            }
                            frameCaptured = outputCaptured = true;
                        }
                        grallocModule->unlock(grallocModule, handle);
                    });
                    if (captureRet == -EAGAIN) {
                        if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                        return -EAGAIN;
                    }
                } else {
                    int usage = GRALLOC_USAGE_SW_WRITE_OFTEN;
                    int captureRet = 0;
                    run_guarded([&]() {
                        int ret = grallocModule->lock(grallocModule, handle, usage,
                                                      0, 0, outBuf.stream->width, outBuf.stream->height, &vaddr);
                        if (ret != 0 || !vaddr) {
                            return;
                        }
                        captureRet = streamFrameCaptured
                                ? pipeline->copyCurrentFrame(static_cast<uint8_t*>(vaddr), outBuf.stream->format)
                                : pipeline->captureFrame(static_cast<uint8_t*>(vaddr), outBuf.stream->format);
                        if (captureRet == 0) {
                            frameCaptured = outputCaptured = true;
                        }
                        grallocModule->unlock(grallocModule, handle);
                    });
                    if (captureRet == -EAGAIN) {
                        if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                        return -EAGAIN;
                    }
                }
                bufferCaptured[i] = outputCaptured;
                if (outputCaptured && outBuf.stream->format != HAL_PIXEL_FORMAT_BLOB)
                    streamFrameCaptured = true;
            }
        }
    }

    // Calculate processing time
    struct timespec ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    long elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 + (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
    ALOGI("Frame %u processing time: %ld ms", frameNum, elapsed_ms);

    // Determine if frame was flushed during capture
    bool frameFlushed = false;
    if (dev->inflight_tracker) {
        frameFlushed = dev->inflight_tracker->isError(frameNum);
        dev->inflight_tracker->remove(frameNum);
    }

    // Send SHUTTER notify callback BEFORE delivering the buffer
    if (frameCaptured && dev->callback_ops && dev->callback_ops->notify) {
        camera3_notify_msg_t notifyMsg;
        memset(&notifyMsg, 0, sizeof(notifyMsg));
        notifyMsg.type = CAMERA3_MSG_SHUTTER;
        notifyMsg.message.shutter.frame_number = request->frame_number;
        notifyMsg.message.shutter.timestamp = ((int64_t)ts_end.tv_sec * 1000000000LL) + (ts_end.tv_nsec);
        dev->callback_ops->notify(dev->callback_ops, &notifyMsg);
        ALOGI("Sent SHUTTER notify for frame %llu", (unsigned long long)request->frame_number);
    }

    // Determine buffer status: OK only if captured AND not flushed.
    // Build a result buffer for EVERY output stream (preview + video) so the
    // framework releases/uses all of them, not just the first one.
    static constexpr int kMaxOutputBuffers = 16;
    camera3_stream_buffer_t outputBufs[kMaxOutputBuffers];
    uint32_t numOutputBufs = 0;
    if (request->num_output_buffers > 0) {
        uint32_t n = request->num_output_buffers;
        if (n > kMaxOutputBuffers) n = kMaxOutputBuffers;
        for (uint32_t i = 0; i < n; i++) {
            outputBufs[i] = request->output_buffers[i];
            outputBufs[i].acquire_fence = -1;
            outputBufs[i].release_fence = -1;
            outputBufs[i].status = (bufferCaptured[i] && !frameFlushed) ?
                CAMERA3_BUFFER_STATUS_OK : CAMERA3_BUFFER_STATUS_ERROR;
        }
        numOutputBufs = n;
    }

    if (frameFlushed) {
        ALOGW("Frame %u was flushed during capture, sending result with ERROR status", frameNum);
    } else if (!frameCaptured) {
        ALOGW("Frame %u not captured, sending result with ERROR status", frameNum);
        if (dev->callback_ops && dev->callback_ops->notify) {
            camera3_notify_msg_t notifyMsg;
            memset(&notifyMsg, 0, sizeof(notifyMsg));
            notifyMsg.type = CAMERA3_MSG_ERROR;
            notifyMsg.message.error.frame_number = request->frame_number;
            notifyMsg.message.error.error_code = CAMERA3_MSG_ERROR_DEVICE;
            dev->callback_ops->notify(dev->callback_ops, &notifyMsg);
        }
    }

    camera_metadata_t* resultMetadata = nullptr;
    if (frameCaptured && !frameFlushed) {
        int32_t sensitivity = 128;
        int64_t exposureNs = 39682540LL, frameDurNs = 0;
        int afState = 0;
        int focusPos = 0;
        if (dev->pipeline) {
            mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
            sensitivity = p->getGain();
            afState = p->getAfState();
            focusPos = p->getFocusPosition();
            exposureNs = p->getExposureNs();
            frameDurNs = p->getFramePeriodNs();
        }
        int64_t timestamp = ((int64_t)ts_end.tv_sec * 1000000000LL) + (ts_end.tv_nsec);
        resultMetadata = build_result_metadata(frameNum, timestamp, exposureNs, sensitivity, afState, focusPos, frameDurNs, dev->af_mode, dev->camera_id);
    }

    camera3_capture_result_t result;
    memset(&result, 0, sizeof(result));
    result.frame_number = request->frame_number;
    result.result = resultMetadata;
    result.num_output_buffers = numOutputBufs;
    result.output_buffers = outputBufs;
    result.partial_result = 1;
    dev->callback_ops->process_capture_result(dev->callback_ops, &result);

    if (resultMetadata) {
        free_camera_metadata(resultMetadata);
    }

    ALOGI("Capture request completed: frame=%llu num_out=%u status=%d",
          (unsigned long long)request->frame_number, numOutputBufs,
          numOutputBufs > 0 ? outputBufs[0].status : -1);
    
    return 0;
}

static void camera_device_dump(const camera3_device_t *device, int fd) {
    ALOGI("camera_device_dump");
    
    if (!device) {
        return;
    }

    dprintf(fd, "Mocha Camera HAL - Device Dump\n");
    dprintf(fd, "================================\n");
    
    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    dprintf(fd, "Camera ID: %d\n", dev->camera_id);
    dprintf(fd, "Initialized: %s\n", dev->is_initialized ? "Yes" : "No");
    dprintf(fd, "Streams configured: %s\n", dev->streams_configured ? "Yes" : "No");
}

static int camera_device_flush(const camera3_device_t *device) {
    ALOGI("camera_device_flush");
    
    if (!device) {
        return -EINVAL;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;

    /* Runs after the currently-held capture finishes (max ~1.8 s poll
       budget, well inside the framework's drain timeout). */
    std::lock_guard<std::mutex> flLock(*dev->pipelineLock);

    // Stop V4L2 streaming IMMEDIATELY to interrupt any threads blocked
    // in captureFrame() (poll/DQBUF). Without this, those threads stay
    // blocked for 1800ms per retry, causing the framework's drain to
    // hit the 30-second timeout, which keeps the camera "in use".
    if (dev->pipeline && dev->streams_configured) {
        mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        p->stopStreaming();
    }
    
    // Mark any remaining in-flight requests as error and drain. The
    // threads that were in captureFrame() will return -EAGAIN (since
    // V4L2 is stopped), and the framework will handle those.
    if (dev->inflight_tracker) {
        dev->inflight_tracker->markAllAsError();
        std::vector<uint32_t> drained = dev->inflight_tracker->drainAll();
        ALOGI("Flush: drained %zu frames", drained.size());
    }
    
    ALOGI("Flush complete");
    return 0;
}

// Camera device open implementation
int MochaCameraHAL::openCamera(int cameraId, hw_device_t **device) {
    ensureCamerasProbed();
    ALOGI("openCamera: logicalId=%d", cameraId);

    if (cameraId < 0 || cameraId >= gNumPresent) {
        ALOGE("Invalid camera ID: %d", cameraId);
        return -EINVAL;
    }

    int physId = gPhysIdForLogical[cameraId];

    int ret = camera_device_init(&::HMI.common, device);
    if (ret != 0) {
        ALOGE("Failed to initialize camera device: %d", ret);
        return ret;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)*device;
    dev->camera_id = physId;
    dev->af_mode = cameraId == 0 ? ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE
                                : ANDROID_CONTROL_AF_MODE_OFF;

    ALOGI("Camera opened successfully: logical=%d phys=%d", cameraId, physId);
    return 0;
}

} // namespace mocha

namespace mocha {

// Implementation of MochaCameraHAL
/*static*/ int MochaCameraHAL::getNumberOfCameras() {
    ensureCamerasProbed();
    ALOGI("getNumberOfCameras: %d", gNumPresent);
    return gNumPresent;
}

/*static*/ int MochaCameraHAL::getCameraInfo(int cameraId, struct camera_info *info) {
    ensureCamerasProbed();
    if (cameraId < 0 || cameraId >= gNumPresent) {
        ALOGE("Invalid camera ID: %d", cameraId);
        return -EINVAL;
    }

    int physId = gPhysIdForLogical[cameraId];
    const MochaCameraInfo& cam = MochaCameraHAL::kCameras[physId];
    info->facing = cam.facing;
    info->orientation = cam.orientation;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_2;
    info->static_camera_characteristics = init_static_characteristics(physId);
    info->resource_cost = 100;
    info->conflicting_devices = nullptr;
    info->conflicting_devices_length = 0;

    ALOGI("Camera info: logical=%d phys=%d facing=%d orientation=%d",
          cameraId, physId, info->facing, info->orientation);

    return 0;
}

} // namespace mocha

// Module entry point
static int get_number_of_cameras() {
    return mocha::MochaCameraHAL::getNumberOfCameras();
}

static int get_camera_info(int camera_id, struct camera_info *info) {
    return mocha::MochaCameraHAL::getCameraInfo(camera_id, info);
}

static int open(const hw_module_t* module, const char* name, hw_device_t** device) {
    ALOGI("Camera HAL open: name=%s module=%p", name, module);

    if (!name) {
        ALOGE("Camera HAL open: null name");
        return -EINVAL;
    }

    int cameraId = atoi(name);
    int ret = mocha::MochaCameraHAL::openCamera(cameraId, device);
    ALOGI("Camera HAL open: returning %d", ret);
    return ret;
}

static hw_module_methods_t methods = {
    .open = open
};

// set_callbacks implementation
static int set_callbacks(const camera_module_callbacks_t *callbacks) {
    ALOGI("set_callbacks called");
    if (callbacks) {
        memcpy(&mocha::gModuleCallbacks, callbacks, sizeof(camera_module_callbacks_t));
    }
    return 0;
}

static int open_legacy(const hw_module_t* module, const char* id, uint32_t halVersion, hw_device_t** device) {
    ALOGI("Camera HAL open_legacy: id=%s halVersion=%u", id, halVersion);

    if (!id) {
        ALOGE("Camera HAL open_legacy: null id");
        return -EINVAL;
    }

    // halVersion=256 = 0x100 = CAMERA_DEVICE_API_VERSION_1_0
    // We reject HAL1 requests because we're HAL3-only.
    // For HAL3 requests (>= 0x30000), accept any version 3.x+.
    if (halVersion >= 0x30000) {
        int cameraId = atoi(id);
        int ret = mocha::MochaCameraHAL::openCamera(cameraId, device);
        ALOGI("Camera HAL open_legacy: returning %d", ret);
        return ret;
    }

    // HAL1 request - must return ENOSYS so CameraProviderManager
    // falls back to HAL3 detection via module version 2.4
    ALOGW("Camera HAL open_legacy: unsupported HAL version %u (we are HAL3 only)", halVersion);
    return -ENOSYS;
}

static int set_torch_mode(const char* camera_id, bool enabled) {
    ALOGI("Camera HAL set_torch_mode: camera_id=%s enabled=%d", camera_id, enabled);
    int cameraId = atoi(camera_id);
    if (cameraId < 0 || cameraId >= mocha::MochaCameraHAL::kNumCameras) {
        return -EINVAL;
    }
    if (cameraId != 0) {
        return -ENOSYS;
    }
    return 0;
}

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CAMERA_HARDWARE_MODULE_ID,
        .name = "Mocha Camera HAL",
        .author = "Mocha Team",
        .methods = &methods,
    },
    .get_number_of_cameras = get_number_of_cameras,
    .get_camera_info = get_camera_info,
    .set_callbacks = set_callbacks,
    .get_vendor_tag_ops = nullptr,
    .open_legacy = open_legacy,
    .set_torch_mode = set_torch_mode,
    .init = nullptr,
};
