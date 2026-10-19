/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "MochaCameraMetadata"
#include "LegacyMetadata.h"

#include <algorithm>
#include <vector>
#include <hardware/camera3.h>
#include <log/log.h>

namespace mocha {
using android::CameraMetadata;

// These controls are present in the stock templates/results. Do not expose
// manual sensor, RAW, reprocessing or constrained-high-speed capabilities merely
// because the old module called itself FULL.
static const int32_t kRequestKeys[] = {
    ANDROID_CONTROL_AE_ANTIBANDING_MODE, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
    ANDROID_CONTROL_AE_LOCK, ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AE_REGIONS,
    ANDROID_CONTROL_AE_TARGET_FPS_RANGE, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
    ANDROID_CONTROL_AF_MODE, ANDROID_CONTROL_AF_REGIONS, ANDROID_CONTROL_AF_TRIGGER,
    ANDROID_CONTROL_AWB_LOCK, ANDROID_CONTROL_AWB_MODE, ANDROID_CONTROL_AWB_REGIONS,
    ANDROID_CONTROL_CAPTURE_INTENT, ANDROID_CONTROL_EFFECT_MODE, ANDROID_CONTROL_MODE,
    ANDROID_CONTROL_SCENE_MODE, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
    ANDROID_EDGE_MODE, ANDROID_FLASH_MODE,
    ANDROID_JPEG_GPS_COORDINATES, ANDROID_JPEG_GPS_PROCESSING_METHOD,
    ANDROID_JPEG_GPS_TIMESTAMP, ANDROID_JPEG_ORIENTATION, ANDROID_JPEG_QUALITY,
    ANDROID_JPEG_THUMBNAIL_QUALITY, ANDROID_JPEG_THUMBNAIL_SIZE,
    ANDROID_LENS_FOCAL_LENGTH, ANDROID_LENS_FOCUS_DISTANCE,
    ANDROID_LENS_OPTICAL_STABILIZATION_MODE, ANDROID_NOISE_REDUCTION_MODE,
    ANDROID_SCALER_CROP_REGION, ANDROID_STATISTICS_FACE_DETECT_MODE,
};

template <typename T, size_t N>
static bool put(CameraMetadata* m, uint32_t tag, const T (&values)[N]) {
    return m->update(tag, values, N) == 0;
}

template <typename T>
static bool scalar(CameraMetadata* m, uint32_t tag, T value) {
    return m->update(tag, &value, 1) == 0;
}

bool buildCharacteristics(const camera_metadata_t* stock, CameraMetadata* output) {
    if (!stock || !output) return false;
    camera_metadata_t* copy = clone_camera_metadata(stock);
    if (!copy) return false;
    CameraMetadata m(copy);
    std::vector<int32_t> configurations;
    std::vector<int64_t> minimums, stalls;
    const int formats[] = {HAL_PIXEL_FORMAT_RGBA_8888, HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
                           HAL_PIXEL_FORMAT_YCbCr_420_888, HAL_PIXEL_FORMAT_BLOB};
    const auto available = m.find(ANDROID_SCALER_AVAILABLE_FORMATS);
    for (int format : formats) {
        if (std::find(available.data.i32, available.data.i32 + available.count, format) ==
                available.data.i32 + available.count) return false;
        bool jpeg = format == HAL_PIXEL_FORMAT_BLOB;
        const auto sizes = m.find(jpeg ? ANDROID_SCALER_AVAILABLE_JPEG_SIZES :
                                        ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES);
        const auto durations = m.find(jpeg ? ANDROID_SCALER_AVAILABLE_JPEG_MIN_DURATIONS :
                                            ANDROID_SCALER_AVAILABLE_PROCESSED_MIN_DURATIONS);
        if (!sizes.count || sizes.count % 2 || durations.count != sizes.count / 2) return false;
        for (size_t i = 0; i < durations.count; ++i) {
            int32_t width = sizes.data.i32[i * 2], height = sizes.data.i32[i * 2 + 1];
            int64_t duration = durations.data.i64[i];
            if (width <= 0 || height <= 0 || duration <= 0) return false;
            // Camera1's Oreo client requires one <=30fps range to work at every
            // advertised opaque preview size. The front sensor's larger modes
            // only run at 15/20fps; keep those for YUV and full-resolution JPEG,
            // and advertise its 30fps modes for preview/video.
            if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED && duration > 33334334) continue;
            configurations.insert(configurations.end(), {format, width, height,
                ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT});
            minimums.insert(minimums.end(), {format, width, height, duration});
            // HAL3.0 supplied a combined JPEG duration; use it as a conservative
            // bound for the stall as well, without claiming a faster JPEG path.
            stalls.insert(stalls.end(), {format, width, height, jpeg ? duration : 0});
        }
    }
    bool ok = true;
    ok &= m.update(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                   configurations.data(), configurations.size()) == 0;
    ok &= m.update(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                   minimums.data(), minimums.size()) == 0;
    ok &= m.update(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stalls.data(), stalls.size()) == 0;

    auto regions = m.find(ANDROID_CONTROL_MAX_REGIONS);
    if (regions.count != 1 && regions.count != 3) return false;
    auto focus = m.find(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE);
    bool fixedFocus = !focus.count || focus.data.f[0] == 0;
    int32_t maxRegions[] = {regions.data.i32[0], regions.data.i32[0],
                            fixedFocus ? 0 : regions.data.i32[0]};
    ok &= put(&m, ANDROID_CONTROL_MAX_REGIONS, maxRegions);
    ok &= scalar<uint8_t>(&m, ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
                          ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE);
    ok &= scalar<uint8_t>(&m, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
                          ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED);
    ok &= scalar<uint8_t>(&m, ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
                          ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE);
    const int32_t maxStreams[] = {0, 3, 1};
    ok &= put(&m, ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, maxStreams);
    ok &= scalar<int32_t>(&m, ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS, 0);
    ok &= scalar<uint8_t>(&m, ANDROID_REQUEST_PIPELINE_MAX_DEPTH, kPipelineDepth);
    ok &= scalar<int32_t>(&m, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, 1);
    ok &= scalar<int32_t>(&m, ANDROID_SYNC_MAX_LATENCY, ANDROID_SYNC_MAX_LATENCY_UNKNOWN);
    ok &= scalar<uint8_t>(&m, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
                          ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN);
    ok &= scalar<uint8_t>(&m, ANDROID_SCALER_CROPPING_TYPE, ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY);
    const uint8_t edgeModes[] = {ANDROID_EDGE_MODE_OFF, ANDROID_EDGE_MODE_FAST};
    const uint8_t noiseModes[] = {ANDROID_NOISE_REDUCTION_MODE_OFF, ANDROID_NOISE_REDUCTION_MODE_FAST};
    ok &= put(&m, ANDROID_EDGE_AVAILABLE_EDGE_MODES, edgeModes);
    ok &= put(&m, ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, noiseModes);
    ok &= scalar<uint8_t>(&m, ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES, ANDROID_HOT_PIXEL_MODE_OFF);
    ok &= scalar<uint8_t>(&m, ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES, ANDROID_TONEMAP_MODE_FAST);
    // The stock IMU-based video stabilization node needs sensor timestamps
    // that this ROM cannot provide (NvIMUBasedVStabCalcTransforms: requested
    // timestamp does not exist). When it runs it stalls the VI/ISP pipeline
    // after the first frame and no output buffers are returned, so advertise
    // OFF only. Camera1 then reports video-stabilization-supported=false and
    // applications do not enable it.
    ok &= scalar<uint8_t>(&m, ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
                          ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF);

    // This is an old {width,height,fps} triple, not the HAL3.2 four-element
    // configuration. Do not let CameraModule iterate past its end.
    m.erase(ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS);
    m.erase(ANDROID_QUIRKS_USE_PARTIAL_RESULT);
    const uint32_t oldScalerTags[] = {
        ANDROID_SCALER_AVAILABLE_FORMATS, ANDROID_SCALER_AVAILABLE_JPEG_SIZES,
        ANDROID_SCALER_AVAILABLE_JPEG_MIN_DURATIONS, ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES,
        ANDROID_SCALER_AVAILABLE_PROCESSED_MIN_DURATIONS, ANDROID_SCALER_AVAILABLE_RAW_SIZES,
        ANDROID_SCALER_AVAILABLE_RAW_MIN_DURATIONS,
    };
    for (uint32_t tag : oldScalerTags) m.erase(tag);
    ok &= put(&m, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, kRequestKeys);
    std::vector<int32_t> resultKeys(std::begin(kRequestKeys), std::end(kRequestKeys));
    const int32_t measured[] = {
        ANDROID_CONTROL_AE_STATE, ANDROID_CONTROL_AF_STATE, ANDROID_CONTROL_AWB_STATE,
        ANDROID_SENSOR_EXPOSURE_TIME, ANDROID_SENSOR_TIMESTAMP,
        ANDROID_LENS_STATE, ANDROID_LENS_FOCUS_RANGE, ANDROID_LENS_APERTURE,
        ANDROID_REQUEST_PIPELINE_DEPTH, ANDROID_FLASH_STATE,
        ANDROID_STATISTICS_FACE_RECTANGLES, ANDROID_STATISTICS_FACE_SCORES,
    };
    resultKeys.insert(resultKeys.end(), std::begin(measured), std::end(measured));
    std::sort(resultKeys.begin(), resultKeys.end());
    resultKeys.erase(std::unique(resultKeys.begin(), resultKeys.end()), resultKeys.end());
    ok &= m.update(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, resultKeys.data(), resultKeys.size()) == 0;

    std::vector<int32_t> characteristicKeys;
    const camera_metadata_t* raw = m.getAndLock();
    for (size_t i = 0; i < get_camera_metadata_entry_count(raw); ++i) {
        camera_metadata_ro_entry_t entry;
        if (get_camera_metadata_ro_entry(raw, i, &entry)) { ok = false; break; }
        characteristicKeys.push_back(entry.tag);
    }
    m.unlock(raw);
    characteristicKeys.push_back(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
    std::sort(characteristicKeys.begin(), characteristicKeys.end());
    characteristicKeys.erase(std::unique(characteristicKeys.begin(), characteristicKeys.end()),
                              characteristicKeys.end());
    ok &= m.update(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                    characteristicKeys.data(), characteristicKeys.size()) == 0;
    if (ok) output->acquire(m);
    return ok;
}

bool normalizeRequest(CameraMetadata* request, const camera_metadata_t* characteristics) {
    camera_metadata_ro_entry_t modes = {};
    bool ok = true;
    const uint32_t controls[] = {ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AF_MODE};
    const uint32_t supported[] = {ANDROID_CONTROL_AE_AVAILABLE_MODES, ANDROID_CONTROL_AF_AVAILABLE_MODES};
    for (size_t i = 0; i < 2; ++i) {
        auto mode = request->find(controls[i]);
        if (mode.count != 1) continue;
        if (find_camera_metadata_ro_entry(characteristics, supported[i], &modes) || !modes.count)
            return false;
        if (std::find(modes.data.u8, modes.data.u8 + modes.count, mode.data.u8[0]) ==
                modes.data.u8 + modes.count) {
            uint8_t replacement = i == 0 ? ANDROID_CONTROL_AE_MODE_ON : ANDROID_CONTROL_AF_MODE_OFF;
            ok &= scalar<uint8_t>(request, controls[i], replacement);
        }
    }
    auto fps = request->find(ANDROID_CONTROL_AE_TARGET_FPS_RANGE);
    if (fps.count != 2 || fps.data.i32[0] <= 0 || fps.data.i32[1] < fps.data.i32[0]) {
        if (find_camera_metadata_ro_entry(characteristics, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                                          &modes) || modes.count < 2) return false;
        ok &= request->update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, modes.data.i32, 2) == 0;
    }
    // Never let the stock VStab node run: see buildCharacteristics().
    auto stabilization = request->find(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE);
    if (stabilization.count != 1 ||
            stabilization.data.u8[0] != ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF)
        ok &= scalar<uint8_t>(request, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
                              ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF);
    return ok;
}

bool mergeMetadata(CameraMetadata* destination, const camera_metadata_t* source) {
    if (!source) return true;
    for (size_t i = 0; i < get_camera_metadata_entry_count(source); ++i) {
        camera_metadata_ro_entry_t entry;
        if (get_camera_metadata_ro_entry(source, i, &entry)) return false;
        if (entry.tag != ANDROID_QUIRKS_PARTIAL_RESULT && destination->update(entry)) return false;
    }
    return true;
}

bool finishResult(CameraMetadata* result, const CameraMetadata& partials) {
    // The stock final packet echoes request fields, including exposure time.
    // Retain the measured 3A values from its earlier partial packets on overlap.
    const camera_metadata_t* raw = partials.getAndLock();
    bool ok = mergeMetadata(result, raw);
    partials.unlock(raw);
    result->erase(ANDROID_QUIRKS_PARTIAL_RESULT);
    ok &= scalar<uint8_t>(result, ANDROID_REQUEST_PIPELINE_DEPTH, kPipelineDepth);
    ok &= scalar<uint8_t>(result, ANDROID_FLASH_STATE, ANDROID_FLASH_STATE_UNAVAILABLE);
    return ok;
}
}  // namespace mocha
