/*
 * Copyright (C) 2015, The CyanogenMod Project
 *           (C) 2017, The LineageOS Project
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

#define LOG_NDEBUG 0
#define LOG_PARAMETERS

#define LOG_TAG "CameraWrapper"
#include <cutils/log.h>

#include <dlfcn.h>
#include <unistd.h>

#include "CameraWrapper.h"
#include "Camera3Wrapper.h"


//------------DEBUG-----------------
static int pfd[2];
static pthread_t thr;

static void *thread_func(void*)
{
    ssize_t rdsz;
    char buf[128];
    while((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
        if(buf[rdsz - 1] == '\n') --rdsz;
        buf[rdsz] = 0;  /* add null-terminator */
        __android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);
    }
    return 0;
}


int start_logger()
{

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    /* create the pipe and redirect stdout and stderr */
    pipe(pfd);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);

    /* spawn the logging thread */
    if(pthread_create(&thr, 0, thread_func, 0) == -1)
        return -1;
    pthread_detach(thr);
    return 0;
}


//-----------DEBUG-------------------------


typedef struct camera_metadata_buffer_entry {
    uint32_t tag;
    uint32_t count;
    union {
        uint32_t offset;
        uint8_t  value[4];
    } data;
    uint8_t  type;
    uint8_t  reserved[3];
} camera_metadata_buffer_entry_t;

typedef uint32_t metadata_uptrdiff_t;
typedef uint32_t metadata_size_t;

struct camera_metadata {
    metadata_size_t          size;
    uint32_t                 version;
    uint32_t                 flags;
    metadata_size_t          entry_count;
    metadata_size_t          entry_capacity;
    metadata_uptrdiff_t      entries_start; // Offset from camera_metadata
    metadata_size_t          data_count;
    metadata_size_t          data_capacity;
    metadata_uptrdiff_t      data_start; // Offset from camera_metadata
    uint8_t                  reserved[];
};

static camera_metadata_buffer_entry_t *get_entries(
        const camera_metadata_t *metadata) {
    return (camera_metadata_buffer_entry_t*)
            ((uint8_t*)metadata + metadata->entries_start);
}


static camera_module_t *gVendorModule = 0;
static char prop[PROPERTY_VALUE_MAX];
static camera_metadata_t* vendorInfo[2] = {0,0};
static camera_info vendor_camera_info;

static inline int normalize_camera_device_version(int version)
{
    if (version == CAMERA_DEVICE_API_VERSION_3_0) {
        return CAMERA_DEVICE_API_VERSION_3_2;
    }

    return version;
}

static bool has_valid_stream_configurations(camera_metadata_t* metadata, int camera_id, const char* stage)
{
    if (metadata == NULL) {
        ALOGE("%s[%s]: null metadata for camera %d", __FUNCTION__, stage, camera_id);
        return false;
    }

    camera_metadata_entry_t stream_configs;
    int rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
            &stream_configs);
    if (rc != 0) {
        ALOGE("%s[%s]: camera %d missing stream configurations", __FUNCTION__, stage, camera_id);
        return false;
    }

    if (stream_configs.count < 4 || (stream_configs.count % 4) != 0) {
        ALOGE("%s[%s]: camera %d invalid stream count=%u", __FUNCTION__, stage, camera_id, stream_configs.count);
        return false;
    }

    bool has_blob_output = false;
    bool has_yuv_output = false;
    for (size_t i = 0; i < stream_configs.count; i += 4) {
        int32_t format = stream_configs.data.i32[i];
        int32_t width = stream_configs.data.i32[i + 1];
        int32_t height = stream_configs.data.i32[i + 2];
        int32_t direction = stream_configs.data.i32[i + 3];
        if (width <= 0 || height <= 0) {
            ALOGE("%s[%s]: camera %d bad stream size %dx%d", __FUNCTION__, stage, camera_id, width, height);
            return false;
        }
        if (direction == ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT &&
                format == HAL_PIXEL_FORMAT_BLOB) {
            has_blob_output = true;
        }
        if (direction == ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT &&
                format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
            has_yuv_output = true;
        }
    }

    if (!has_blob_output) {
        ALOGE("%s[%s]: camera %d missing BLOB output configuration", __FUNCTION__, stage, camera_id);
        return false;
    }

    if (!has_yuv_output) {
        ALOGE("%s[%s]: camera %d missing YUV_420_888 output configuration", __FUNCTION__, stage, camera_id);
        return false;
    }

    ALOGI("%s[%s]: camera %d stream configurations count=%u", __FUNCTION__, stage, camera_id, stream_configs.count);
    return true;
}

static bool upsert_metadata_entry(camera_metadata_t** metadata_ptr,
        uint32_t tag,
        const void* entries,
        size_t entry_count,
        int camera_id,
        ssize_t existing_index)
{
    camera_metadata_t* metadata = *metadata_ptr;
    int rc = 0;
    if (existing_index >= 0) {
        rc = update_camera_metadata_entry(metadata,
                existing_index,
                entries,
                entry_count,
                NULL);
    } else {
        rc = add_camera_metadata_entry(metadata,
                tag,
                entries,
                entry_count);
    }

    if (rc == 0) {
        return true;
    }

    size_t entry_size = sizeof(uint8_t);
    int tag_type = get_camera_metadata_tag_type(tag);
    switch (tag_type) {
        case TYPE_BYTE:
            entry_size = sizeof(uint8_t);
            break;
        case TYPE_INT32:
            entry_size = sizeof(int32_t);
            break;
        case TYPE_FLOAT:
            entry_size = sizeof(float);
            break;
        case TYPE_INT64:
            entry_size = sizeof(int64_t);
            break;
        case TYPE_DOUBLE:
            entry_size = sizeof(double);
            break;
        case TYPE_RATIONAL:
            entry_size = sizeof(camera_metadata_rational_t);
            break;
        default:
            ALOGE("%s: unknown tag type %d for tag 0x%x camera %d", __FUNCTION__, tag_type, tag, camera_id);
            return false;
    }

    size_t extra_data = entry_count * entry_size + 512;
    camera_metadata_t* expanded = allocate_camera_metadata(
            metadata->entry_count + 4,
            metadata->data_count + extra_data);
    if (expanded == NULL) {
        ALOGE("%s: failed to allocate expanded metadata for camera %d", __FUNCTION__, camera_id);
        return false;
    }

    int append_rc = append_camera_metadata(expanded, metadata);
    if (append_rc != 0) {
        ALOGE("%s: failed appending metadata for camera %d", __FUNCTION__, camera_id);
        free_camera_metadata(expanded);
        return false;
    }

    if (existing_index >= 0) {
        rc = update_camera_metadata_entry(expanded,
                existing_index,
                entries,
                entry_count,
                NULL);
    } else {
        rc = add_camera_metadata_entry(expanded,
                tag,
                entries,
                entry_count);
    }
    if (rc != 0) {
        ALOGE("%s: failed upserting tag 0x%x for camera %d", __FUNCTION__, tag, camera_id);
        free_camera_metadata(expanded);
        return false;
    }

    free_camera_metadata(metadata);
    *metadata_ptr = expanded;
    return true;
}

static bool ensure_stream_configurations(camera_metadata_t** metadata_ptr, int camera_id)
{
    camera_metadata_t* metadata = *metadata_ptr;
    if (metadata == NULL) {
        return false;
    }

    camera_metadata_entry_t stream_configs;
    camera_metadata_entry_t min_durations;
    camera_metadata_entry_t stall_durations;
    int rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
            &stream_configs);
    int min_rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
            &min_durations);
    int stall_rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
            &stall_durations);

    if (rc == 0 && stream_configs.count >= 4 && (stream_configs.count % 4 == 0) &&
            min_rc == 0 && stall_rc == 0 &&
            min_durations.count == stall_durations.count &&
            min_durations.count == stream_configs.count &&
            has_valid_stream_configurations(metadata, camera_id, "existing")) {
        return true;
    }

    camera_metadata_entry_t processed_sizes;
    camera_metadata_entry_t jpeg_sizes;
    int processed_rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES,
            &processed_sizes);
    int jpeg_rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_JPEG_SIZES,
            &jpeg_sizes);

    if (processed_rc != 0 || jpeg_rc != 0 ||
            processed_sizes.count < 2 || jpeg_sizes.count < 2 ||
            (processed_sizes.count % 2) != 0 || (jpeg_sizes.count % 2) != 0) {
        ALOGE("%s: cannot synthesize stream configurations for camera %d", __FUNCTION__, camera_id);
        return false;
    }

    const int32_t stream_field_count = 4;
    const size_t processed_pair_count = processed_sizes.count / 2;
    const size_t jpeg_pair_count = jpeg_sizes.count / 2;
    const size_t processed_formats_per_size = 2;
    const size_t total_stream_count =
            (processed_pair_count * processed_formats_per_size + jpeg_pair_count) * stream_field_count;

    int32_t* synthesized = (int32_t*)calloc(total_stream_count, sizeof(int32_t));
    int64_t* min_frame_durations = (int64_t*)calloc(total_stream_count, sizeof(int64_t));
    int64_t* stall_durations_data = (int64_t*)calloc(total_stream_count, sizeof(int64_t));
    if (synthesized == NULL) {
        ALOGE("%s: failed to allocate synthesized stream configurations", __FUNCTION__);
        free(min_frame_durations);
        free(stall_durations_data);
        return false;
    }

    if (min_frame_durations == NULL || stall_durations_data == NULL) {
        ALOGE("%s: failed to allocate synthesized duration metadata", __FUNCTION__);
        free(synthesized);
        free(min_frame_durations);
        free(stall_durations_data);
        return false;
    }

    size_t out = 0;
    size_t duration_out = 0;
    const int64_t min_duration_ns = 33333334LL;
    const int64_t jpeg_stall_ns = 200000000LL;

    for (size_t i = 0; i < processed_sizes.count; i += 2) {
        if (processed_sizes.data.i32[i] <= 0 || processed_sizes.data.i32[i + 1] <= 0) {
            continue;
        }
        const int32_t width = processed_sizes.data.i32[i];
        const int32_t height = processed_sizes.data.i32[i + 1];

        synthesized[out++] = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
        synthesized[out++] = width;
        synthesized[out++] = height;
        synthesized[out++] = ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT;

        min_frame_durations[duration_out++] = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
        min_frame_durations[duration_out++] = width;
        min_frame_durations[duration_out++] = height;
        min_frame_durations[duration_out++] = min_duration_ns;

        stall_durations_data[duration_out - 4] = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
        stall_durations_data[duration_out - 3] = width;
        stall_durations_data[duration_out - 2] = height;
        stall_durations_data[duration_out - 1] = 0;

        synthesized[out++] = HAL_PIXEL_FORMAT_YCbCr_420_888;
        synthesized[out++] = width;
        synthesized[out++] = height;
        synthesized[out++] = ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT;

        min_frame_durations[duration_out++] = HAL_PIXEL_FORMAT_YCbCr_420_888;
        min_frame_durations[duration_out++] = width;
        min_frame_durations[duration_out++] = height;
        min_frame_durations[duration_out++] = min_duration_ns;

        stall_durations_data[duration_out - 4] = HAL_PIXEL_FORMAT_YCbCr_420_888;
        stall_durations_data[duration_out - 3] = width;
        stall_durations_data[duration_out - 2] = height;
        stall_durations_data[duration_out - 1] = 0;
    }

    for (size_t i = 0; i < jpeg_sizes.count; i += 2) {
        if (jpeg_sizes.data.i32[i] <= 0 || jpeg_sizes.data.i32[i + 1] <= 0) {
            continue;
        }
        synthesized[out++] = HAL_PIXEL_FORMAT_BLOB;
        synthesized[out++] = jpeg_sizes.data.i32[i];
        synthesized[out++] = jpeg_sizes.data.i32[i + 1];
        synthesized[out++] = ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT;

        min_frame_durations[duration_out++] = HAL_PIXEL_FORMAT_BLOB;
        min_frame_durations[duration_out++] = jpeg_sizes.data.i32[i];
        min_frame_durations[duration_out++] = jpeg_sizes.data.i32[i + 1];
        min_frame_durations[duration_out++] = min_duration_ns;

        stall_durations_data[duration_out - 4] = HAL_PIXEL_FORMAT_BLOB;
        stall_durations_data[duration_out - 3] = jpeg_sizes.data.i32[i];
        stall_durations_data[duration_out - 2] = jpeg_sizes.data.i32[i + 1];
        stall_durations_data[duration_out - 1] = jpeg_stall_ns;
    }

    if (out < 4 || (out % 4) != 0 || duration_out != out) {
        ALOGE("%s: synthesized stream configurations invalid for camera %d", __FUNCTION__, camera_id);
        free(synthesized);
        free(min_frame_durations);
        free(stall_durations_data);
        return false;
    }

    ssize_t existing_index = (rc == 0) ? (ssize_t)stream_configs.index : -1;
    ssize_t existing_min_index = (min_rc == 0) ? (ssize_t)min_durations.index : -1;
    ssize_t existing_stall_index = (stall_rc == 0) ? (ssize_t)stall_durations.index : -1;

    bool added = upsert_metadata_entry(metadata_ptr,
            ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
            synthesized,
            out,
            camera_id,
            existing_index);

    if (added) {
        added = upsert_metadata_entry(metadata_ptr,
                ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                min_frame_durations,
                duration_out,
                camera_id,
                existing_min_index);
    }

    if (added) {
        added = upsert_metadata_entry(metadata_ptr,
                ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
                stall_durations_data,
                duration_out,
                camera_id,
                existing_stall_index);
    }

    free(synthesized);
    free(min_frame_durations);
    free(stall_durations_data);

    if (!added) {
        return false;
    }

    metadata = *metadata_ptr;
    return has_valid_stream_configurations(metadata, camera_id, "synthesized");
}

static bool ensure_jpeg_metadata(camera_metadata_t** metadata_ptr, int camera_id)
{
    camera_metadata_t* metadata = *metadata_ptr;
    if (metadata == NULL) {
        return false;
    }

    camera_metadata_entry_t jpeg_sizes;
    int jpeg_sizes_rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_JPEG_SIZES,
            &jpeg_sizes);

    camera_metadata_entry_t stream_configs;
    int stream_rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
            &stream_configs);

    size_t max_pairs = 0;
    if (jpeg_sizes_rc == 0 && (jpeg_sizes.count % 2) == 0) {
        max_pairs += jpeg_sizes.count / 2;
    }
    if (stream_rc == 0 && (stream_configs.count % 4) == 0) {
        max_pairs += stream_configs.count / 4;
    }

    if (max_pairs == 0) {
        ALOGE("%s: camera %d has no source for JPEG sizes", __FUNCTION__, camera_id);
        return false;
    }

    int32_t* sanitized_sizes = (int32_t*)calloc(max_pairs * 2, sizeof(int32_t));
    if (sanitized_sizes == NULL) {
        ALOGE("%s: camera %d failed to allocate JPEG sizes buffer", __FUNCTION__, camera_id);
        return false;
    }

    size_t size_out = 0;
    if (jpeg_sizes_rc == 0 && (jpeg_sizes.count % 2) == 0) {
        for (size_t i = 0; i < jpeg_sizes.count; i += 2) {
            int32_t width = jpeg_sizes.data.i32[i];
            int32_t height = jpeg_sizes.data.i32[i + 1];
            if (width <= 0 || height <= 0) {
                continue;
            }
            sanitized_sizes[size_out++] = width;
            sanitized_sizes[size_out++] = height;
        }
    }

    if (size_out == 0 && stream_rc == 0 && (stream_configs.count % 4) == 0) {
        for (size_t i = 0; i < stream_configs.count; i += 4) {
            int32_t format = stream_configs.data.i32[i];
            int32_t width = stream_configs.data.i32[i + 1];
            int32_t height = stream_configs.data.i32[i + 2];
            int32_t direction = stream_configs.data.i32[i + 3];
            if (format != HAL_PIXEL_FORMAT_BLOB ||
                    direction != ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
                continue;
            }
            if (width <= 0 || height <= 0) {
                continue;
            }
            sanitized_sizes[size_out++] = width;
            sanitized_sizes[size_out++] = height;
        }
    }

    if (size_out < 2 || (size_out % 2) != 0) {
        ALOGE("%s: camera %d has no valid JPEG sizes after sanitization", __FUNCTION__, camera_id);
        free(sanitized_sizes);
        return false;
    }

    ssize_t jpeg_sizes_index = (jpeg_sizes_rc == 0) ? (ssize_t)jpeg_sizes.index : -1;
    if (!upsert_metadata_entry(metadata_ptr,
            ANDROID_SCALER_AVAILABLE_JPEG_SIZES,
            sanitized_sizes,
            size_out,
            camera_id,
            jpeg_sizes_index)) {
        ALOGE("%s: camera %d failed to upsert JPEG sizes", __FUNCTION__, camera_id);
        free(sanitized_sizes);
        return false;
    }

    free(sanitized_sizes);

    metadata = *metadata_ptr;
    camera_metadata_entry_t jpeg_durations;
    int durations_rc = find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_JPEG_MIN_DURATIONS,
            &jpeg_durations);
    size_t expected_duration_count = size_out / 2;

    bool needs_duration_fix = (durations_rc != 0 || jpeg_durations.count != expected_duration_count);
    int64_t* sanitized_durations = NULL;
    if (needs_duration_fix) {
        sanitized_durations = (int64_t*)calloc(expected_duration_count, sizeof(int64_t));
        if (sanitized_durations == NULL) {
            ALOGE("%s: camera %d failed to allocate JPEG durations", __FUNCTION__, camera_id);
            return false;
        }

        const int64_t fallback_jpeg_duration_ns = 200000000LL;
        for (size_t i = 0; i < expected_duration_count; ++i) {
            sanitized_durations[i] = fallback_jpeg_duration_ns;
        }

        ssize_t existing_durations_index = (durations_rc == 0) ? (ssize_t)jpeg_durations.index : -1;
        if (!upsert_metadata_entry(metadata_ptr,
                ANDROID_SCALER_AVAILABLE_JPEG_MIN_DURATIONS,
                sanitized_durations,
                expected_duration_count,
                camera_id,
                existing_durations_index)) {
            ALOGE("%s: camera %d failed to upsert JPEG durations", __FUNCTION__, camera_id);
            free(sanitized_durations);
            return false;
        }

        free(sanitized_durations);
    }

    return true;
}

static bool ensure_request_capabilities(camera_metadata_t** metadata_ptr, int camera_id)
{
    camera_metadata_t* metadata = *metadata_ptr;
    if (metadata == NULL) {
        return false;
    }

    camera_metadata_entry_t capabilities;
    int rc = find_camera_metadata_entry(metadata,
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
            &capabilities);
    if (rc == 0 && capabilities.count > 0) {
        return true;
    }

    const int32_t fallback_capabilities[] = {
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
    };

    ssize_t existing_index = (rc == 0) ? (ssize_t)capabilities.index : -1;
    bool added = upsert_metadata_entry(metadata_ptr,
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
            fallback_capabilities,
            sizeof(fallback_capabilities) / sizeof(fallback_capabilities[0]),
            camera_id,
            existing_index);
    if (!added) {
        ALOGE("%s: failed adding request capabilities for camera %d", __FUNCTION__, camera_id);
        return false;
    }

    metadata = *metadata_ptr;
    rc = find_camera_metadata_entry(metadata,
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
            &capabilities);
    if (rc != 0 || capabilities.count == 0) {
        ALOGE("%s: request capabilities still missing for camera %d", __FUNCTION__, camera_id);
        return false;
    }

    return true;
}

static bool sanitize_control_regions_and_overrides(camera_metadata_t** metadata_ptr, int camera_id)
{
    camera_metadata_t* metadata = *metadata_ptr;
    if (metadata == NULL) {
        return false;
    }

    camera_metadata_entry_t af_modes;
    bool has_af = false;
    bool af_off_only = true;
    if (find_camera_metadata_entry(metadata,
            ANDROID_CONTROL_AF_AVAILABLE_MODES,
            &af_modes) == 0 && af_modes.count > 0) {
        has_af = true;
        for (size_t i = 0; i < af_modes.count; ++i) {
            if (af_modes.data.u8[i] != ANDROID_CONTROL_AF_MODE_OFF) {
                af_off_only = false;
                break;
            }
        }
    }

    camera_metadata_entry_t max_regions;
    int max_regions_rc = find_camera_metadata_entry(metadata,
            ANDROID_CONTROL_MAX_REGIONS,
            &max_regions);
    if (max_regions_rc != 0 || max_regions.count != 3) {
        int32_t ae_awb_default = 0;
        if (max_regions_rc == 0 && max_regions.count > 0) {
            ae_awb_default = max_regions.data.i32[0];
            if (ae_awb_default < 0) {
                ae_awb_default = 0;
            }
        }

        int32_t sanitized_regions[3] = {
            ae_awb_default,
            ae_awb_default,
            (has_af && !af_off_only) ? ae_awb_default : 0,
        };

        ssize_t existing_index = (max_regions_rc == 0) ? (ssize_t)max_regions.index : -1;
        if (!upsert_metadata_entry(metadata_ptr,
                ANDROID_CONTROL_MAX_REGIONS,
                sanitized_regions,
                sizeof(sanitized_regions) / sizeof(sanitized_regions[0]),
                camera_id,
                existing_index)) {
            ALOGE("%s: failed to sanitize max regions for camera %d", __FUNCTION__, camera_id);
            return false;
        }

        metadata = *metadata_ptr;
    }

    camera_metadata_entry_t scene_overrides;
    int scene_rc = find_camera_metadata_entry(metadata,
            ANDROID_CONTROL_SCENE_MODE_OVERRIDES,
            &scene_overrides);
    if (scene_rc == 0 && scene_overrides.count > 0) {
        delete_camera_metadata_entry(metadata, scene_overrides.index);
    }

    return true;
}

static bool force_legacy_hardware_level(camera_metadata_t** metadata_ptr, int camera_id)
{
    camera_metadata_t* metadata = *metadata_ptr;
    if (metadata == NULL) {
        return false;
    }

    camera_metadata_entry_t hw_level;
    int rc = find_camera_metadata_entry(metadata,
            ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
            &hw_level);

    const uint8_t legacy_level[] = {
        ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY,
    };

    ssize_t existing_index = (rc == 0) ? (ssize_t)hw_level.index : -1;
    if (!upsert_metadata_entry(metadata_ptr,
            ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
            legacy_level,
            sizeof(legacy_level) / sizeof(legacy_level[0]),
            camera_id,
            existing_index)) {
        ALOGE("%s: failed to force legacy hw level for camera %d", __FUNCTION__, camera_id);
        return false;
    }

    return true;
}

static bool force_legacy_request_limits(camera_metadata_t** metadata_ptr, int camera_id)
{
    camera_metadata_t* metadata = *metadata_ptr;
    if (metadata == NULL) {
        return false;
    }

    camera_metadata_entry_t max_outputs;
    int outputs_rc = find_camera_metadata_entry(metadata,
            ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
            &max_outputs);

    const int32_t legacy_max_outputs[] = {
        0, // RAW
        1, // PROC
        1, // PROC_STALLING
    };

    ssize_t max_outputs_index = (outputs_rc == 0) ? (ssize_t)max_outputs.index : -1;
    if (!upsert_metadata_entry(metadata_ptr,
            ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
            legacy_max_outputs,
            sizeof(legacy_max_outputs) / sizeof(legacy_max_outputs[0]),
            camera_id,
            max_outputs_index)) {
        ALOGE("%s: failed to force max output streams for camera %d", __FUNCTION__, camera_id);
        return false;
    }

    metadata = *metadata_ptr;

    camera_metadata_entry_t max_inputs;
    int inputs_rc = find_camera_metadata_entry(metadata,
            ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
            &max_inputs);
    const int32_t legacy_max_inputs[] = { 0 };
    ssize_t max_inputs_index = (inputs_rc == 0) ? (ssize_t)max_inputs.index : -1;
    if (!upsert_metadata_entry(metadata_ptr,
            ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
            legacy_max_inputs,
            sizeof(legacy_max_inputs) / sizeof(legacy_max_inputs[0]),
            camera_id,
            max_inputs_index)) {
        ALOGE("%s: failed to force max input streams for camera %d", __FUNCTION__, camera_id);
        return false;
    }

    metadata = *metadata_ptr;

    camera_metadata_entry_t io_formats_map;
    if (find_camera_metadata_entry(metadata,
            ANDROID_SCALER_AVAILABLE_INPUT_OUTPUT_FORMATS_MAP,
            &io_formats_map) == 0) {
        delete_camera_metadata_entry(metadata, io_formats_map.index);
    }

    return true;
}

/*
 * Descubrimiento de sensores del stack de cámara NVIDIA stock (puerto de
 * Abobo7 lineage-17.1, android_device_xiaomi_mocha/camera/CameraWrapper.cpp).
 *
 * El blob vendor (libnvmm_camera + /etc/nvcamera.conf) identifica los sensores
 * por los nodos misc /dev/imx179 y /dev/ov5693. Esos nodos los crea
 * misc_register() en kernel/.../drivers/media/platform/tegra/{imx179,ov5693}.c
 * sólo cuando pcl-generic instancia el módulo
 * (camera_probe -> camera_add_drivers -> camera_i2c_probe), y eso ocurre sólo
 * si alguien abre /dev/camera.pcl y emite los ioctls NVC.
 * libnvodm_imager.so (vendor/lib/libnvodm_imager.so, instalada por
 * vendor/xiaomi/mocha/mocha-vendor.mk) es quien hace esa apertura: sin este
 * paso el vendor HAL enumera 0 cámaras (F8) aunque el kernel tenga el stack.
 *
 * El handle se mantiene vivo a propósito: la librería deja tablas internas de
 * dispositivos detectados que el HAL reutiliza en NvOdmImagerCreate2().
 */
static void *gImagerHandle = nullptr;

static bool sensor_nodes_ready()
{
    if (access("/dev/imx179", R_OK | W_OK) != 0)
        return false;
    /* ov5693.c usa pdata->dev_name y puede añadir ".<num>" al nombre */
    return access("/dev/ov5693", R_OK | W_OK) == 0 ||
           access("/dev/ov5693.0", R_OK | W_OK) == 0;
}

/*
 * Android 12 ya NO consume TARGET_LD_SHIM_LIBS (build/make y linkerconfig lo retiraron), y
 * los blobs NVIDIA de cámara heredados de Android 5/6 referencian símbolos que no están en su
 * cierre de DT_NEEDED: p.ej. android::Singleton<android::SensorManager>::sLock vive en
 * libsensor.so, que el provider nunca carga. El enlazador resuelve los símbolos indefinidos de
 * una librería recién dlopen()ada contra (1) su grupo local (su cierre de dependencias) y
 * (2) el GRUPO GLOBAL del proceso (ejecutable + LD_PRELOAD + librerías cargadas con
 * RTLD_GLOBAL, ver bionic/linker/linker_namespaces.cpp: "This group consists of the main
 * executable, LD_PRELOAD and DF_1_GLOBAL libraries").
 *
 * Por eso metemos aquí los shims en el grupo global, ANTES de que hw_get_module_by_class()
 * cargue camera.vendor.tegra.so: al ser RTLD_GLOBAL entran también sus dependencias
 * (libsensor, libbinder, libcamera_client, ...), que es exactamente lo que el blob necesita.
 */
static void loadGlobalShims()
{
    static const char *const shims[] = {
        "libmocha_cpp_shim.so",      /* arrastra libc++ (std::ios_base::Init) */
        "libmocha_sensor_shim.so",   /* Singleton<SensorManager>, SensorManager, CameraMetadata, Fence */
        "libshim_atomic.so",         /* android_atomic_inc / android_atomic_dec */
    };

    /*
     * Diagnostico (2026-10-28): libbeautify.so, DT_NEEDED de camera.vendor.tegra.so,
     * declara como indefinidos ~60 simbolos de la ABI C++ de 2015 (std::string old-ABI,
     * std::cout/cerr, basic_ifstream, std::ios_base::Init) y de OpenCV 2.4. El unico
     * donante en el tree es libFaceProc.so (6923 simbolos: enlaza libstdc++ estaicamente),
     * pero tiene DT_TEXTREL y Android 12 rechaza precargarlo. Aqui se intenta el dlopen
     * normal para saber si la politica de text relocations tambien aplica a la carga por
     * DT_NEEDED/dlopen dentro del namespace del provider.
     */
    static const char *const legacy[] = {
        "libopencv24_tegra.so",
        "libFaceProc.so",
    };
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;

    for (size_t i = 0; i < sizeof(shims) / sizeof(shims[0]); i++) {
        void *h = dlopen(shims[i], RTLD_NOW | RTLD_GLOBAL);
        if (h)
            ALOGI("%s: shim %s en grupo GLOBAL", __FUNCTION__, shims[i]);
        else
            ALOGE("%s: shim GLOBAL %s falló: %s", __FUNCTION__, shims[i], dlerror());
    }

    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        void *h = dlopen(legacy[i], RTLD_NOW | RTLD_GLOBAL);
        if (h)
            ALOGI("%s: legacy %s cargada OK en grupo GLOBAL", __FUNCTION__, legacy[i]);
        else
            ALOGE("%s: legacy %s NO cargable: %s", __FUNCTION__, legacy[i], dlerror());
    }
}

static void prepareImagerDevices()
{
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;

    void *imager = dlopen("libnvodm_imager.so", RTLD_NOW | RTLD_LOCAL);
    if (!imager) {
        ALOGE("%s: dlopen(libnvodm_imager.so) falló: %s",
                __FUNCTION__, dlerror());
        return;
    }
    gImagerHandle = imager;   /* no se cierra: conserva el estado del detect */

    typedef int (*imager_init_fn)();
    typedef int (*imager_detect_fn)();
    imager_init_fn detInit = (imager_init_fn)dlsym(imager, "NvOdmImagerDetInit");
    imager_detect_fn detect =
            (imager_detect_fn)dlsym(imager, "NvOdmImagerDeviceDetect");
    if (!detInit || !detect) {
        ALOGE("%s: faltan símbolos imager (DetInit=%p Detect=%p)",
                __FUNCTION__, detInit, detect);
        return;
    }

    detInit();
    detect();

    for (int i = 0; i < 100; i++) {
        if (sensor_nodes_ready()) {
            ALOGD("%s: nodos de sensor listos (imager=%p)",
                    __FUNCTION__, gImagerHandle);
            return;
        }
        usleep(20000);   /* 2 s en total: el probe I2C + misc_register es lento */
    }
    ALOGE("%s: timeout esperando /dev/imx179 y /dev/ov5693", __FUNCTION__);
}

static int check_vendor_module()
{
    int rv = 0;
    ALOGV("%s", __FUNCTION__);

    if(gVendorModule)
        return 0;

    loadGlobalShims();
    prepareImagerDevices();

    rv = hw_get_module_by_class("camera", "vendor", (const hw_module_t **)&gVendorModule);
    if (rv)
        ALOGE("failed to open vendor camera module");
    return rv;
}

static struct hw_module_methods_t camera_module_methods = {
        open: camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
         .tag = HARDWARE_MODULE_TAG,
         .module_api_version = CAMERA_MODULE_API_VERSION_2_3,
         .hal_api_version = HARDWARE_HAL_API_VERSION,
         .id = CAMERA_HARDWARE_MODULE_ID,
         .name = "MI PAD Camera Wrapper",
         .author = "The LineageOS Project",
         .methods = &camera_module_methods,
         .dso = NULL,
         .reserved = {0},
    },
    .get_number_of_cameras = camera_get_number_of_cameras,
    .get_camera_info = camera_get_camera_info,
    .set_callbacks = camera_set_callbacks,
    .get_vendor_tag_ops = camera_get_vendor_tag_ops,
    .open_legacy = NULL,
    .set_torch_mode = NULL,
    .init = NULL,
    .reserved = {0},
};

static int camera_device_open(const hw_module_t* module, const char* name,
                hw_device_t** device)
{
    int rv = -EINVAL;
    start_logger(); //DEBUG

    if (name != NULL) {
        if (check_vendor_module())
            return -EINVAL;

        rv = camera3_device_open(module, name, device);
    }

    return rv;
}

/*
 * Semilla de metadata estatica HAL3 (2026-10-28): camera.vendor.tegra.so carga y sondea los
 * sensores (dmesg: 'imx179 module vendor ID: 0x08 06' -> I2C del sensor trasero responde),
 * pero su get_camera_info() devuelve static_camera_characteristics = NULL (medido:
 * 'camera_get_camera_info: camara 0 HAL=0x0 facing=0 orientacion=90 version=0x300').
 * El provider AOSP 'CameraModule::getCameraInfo' -> 'CameraMetadata::append' /
 * 'deriveCameraCharacteristicsKeys' no tolera NULL -> SIGSEGV y reinicio perpetuo del
 * provider ('Number of camera devices: 0').
 *
 * Si el HAL no entrega metadata, sembramos un blob con lo minimo que los sintetizadores del
 * wrapper (ensure_jpeg_metadata, ensure_stream_configurations, ensure_request_capabilities,
 * force_legacy_hardware_level, force_legacy_request_limits) necesitan para completarlas,
 * que es precisamente la lista de tamano de 'processed' (lo que falta en el HAL).
 *
 * Identidad y resoluciones, de las fuentes del propio tree (no suposiciones):
 *   device/xiaomi/mocha/camera/nvcamera.conf  -> camera0=/dev/imx179,back,0,mono
 *                                                camera1=/dev/ov5693,front,270,mono
 *   kernel/.../tegra/imx179.c                  -> mode_1920x1080 y mode_1280x720_90fps
 *   kernel/.../tegra/v4l2/sensors/ov5693.c     -> 2592x1944@15, 1920x1080@30,
 *                                                  1280x720@60, 640x480@90
 * ORIENTACION: 'camera_info.orientation' debe ser el giro que hace falta aplicar para que
 * la imagen quede en la orientacion natural del dispositivo (grados horarios). Los valores
 * de nvcamera.conf (0 y 270) son el angulo fisico del montaje del sensor; en Android ese
 * angulo se invierte (sentido antihorario): Android = (360 - angulo) % 360, de ahi 0 y 90.
 * Que el HAL stock reportase 90 para la camara 0 se comprueba aqui de forma medible: si la
 * foto saliera girada, se ajusta usando la constante de cada camara.
 */
static const struct {
    const char *name;
    int facing;      /* ANDROID_LENS_FACING_* */
    int orientation; /* ANDROID_SENSOR_ORIENTATION (grados horarios a aplicar) */
    unsigned int active_w, active_h; /* tamano activo del array, px */
    struct { unsigned int w, h; int fps; } sizes[4];
    int nsizes;
    int cfa;         /* ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_* */
    unsigned int iso_min, iso_max;
} kMochaCameras[2] = {
    /* camera0: IMX179, montaje 'back', angulo fisico 0 */
    { "imx179(back)", ANDROID_LENS_FACING_BACK, 0,
      1920, 1080,
      { { 1920, 1080, 30 }, { 1280, 720, 90 }, { 640, 480, 90 }, { 0, 0, 0 } }, 3,
      ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB,
      100, 1600 },
    /* camera1: OV5693, montaje 'front', angulo fisico 270 */
    { "ov5693(front)", ANDROID_LENS_FACING_FRONT, 90,
      2592, 1944,
      { { 2592, 1944, 15 }, { 1920, 1080, 30 }, { 1280, 720, 60 }, { 640, 480, 90 } }, 4,
      ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB,
      100, 1600 },
};

/*
 * Firma real del helper local: upsert_metadata_entry(&blob, tag, datos, numero_de_elementos,
 * camera_id, indice_o_-1). El tipo de datos NO se pasa: camera_metadata.c lo deriva de la
 * propia etiqueta, de ahi que los valores se almacenen en variables del tipo nativo de cada
 * etiqueta (int32_t / uint8_t / int64_t / float) y 'count' sea numero de elementos, no bytes.
 *
 * Etiqueta critica: ANDROID_CONTROL_AVAILABLE_SCENE_MODES.
 * En A12, hardware/interfaces/camera/common/1.0/default/CameraModule.cpp::
 * deriveCameraCharacteristicsKeys() (lineas 47-49) hace:
 *     camera_metadata_entry entry = chars.find(ANDROID_CONTROL_AVAILABLE_SCENE_MODES);
 *     if (entry.count > 1 || entry.data.u8[0] != ANDROID_CONTROL_SCENE_MODE_DISABLED)
 * Si la etiqueta no existe, 'entry.count' vale 0 pero 'entry.data' vale NULL y la condicion
 * evalua igualmente 'entry.data.u8[0]' -> lectura en la direccion 0x0 -> SIGSEGV con
 * 'fault addr 0x0' (130 crashes del provider registrados en sistema.log) y 'Number of camera
 * devices: 0'. Por eso esta etiqueta se siembra SIEMPRE.
 */
static bool seed_static_metadata(camera_metadata_t **metadata_ptr, int camera_id)
{
    if (camera_id < 0 || camera_id >= (int)(sizeof(kMochaCameras) / sizeof(kMochaCameras[0]))) {
        ALOGE("%s: camara %d fuera del catalogo mocha", __FUNCTION__, camera_id);
        return false;
    }
    const auto &cam = kMochaCameras[camera_id];
    camera_metadata_t *md = *metadata_ptr;
    /*
     * Capacidad generosa: en esta version de libcamera_metadata add_camera_metadata_entry()
     * RECIBE el blob por valor ('camera_metadata_t *dst', verificado en
     * system/media/camera/include/system/camera_metadata.h:350) y NO lo agranda: si se agota el
     * espacio devuelve fallo y la etiqueta simplemente no existe. Hay que reservar de sobra para
     * lo que siembra esta funcion (~24 entradas) MAS lo que anaden despues
     * ensure_jpeg_metadata() / ensure_stream_configurations() (JPEG_SIZES,
     * STREAM_CONFIGURATIONS, MIN_FRAME_DURATIONS, STALL_DURATIONS, JPEG_*_DURATIONS: con 4
     * resoluciones son ~36 triples y duraciones int64). 96 entradas / 8 KiB de datos.
     */
    if (md == NULL) md = allocate_camera_metadata(96, 8192);
    if (md == NULL) return false;

    /*
     * Tipos verificados contra la tabla autoritativa system/media/camera/src/
     * camera_metadata_tag_info.c (nombre+tipo por etiqueta), que es la que usa
     * libcamera_metadata al validar cada entrada. Si el tipo no coincide, add_camera_metadata_entry
     * rechaza la etiqueta y el resultado es la misma ausencia que provoca el SIGSEGV:
     *   LENS_FACING ................ TYPE_BYTE   (ojo: no int32)
     *   SENSOR_ORIENTATION ......... TYPE_INT32
     *   SENSOR_INFO_PIXEL_ARRAY_SIZE / ACTIVE_ARRAY_SIZE ... TYPE_INT32
     *   SENSOR_INFO_COLOR_FILTER_ARRANGEMENT ........ TYPE_BYTE
     *   SENSOR_INFO_EXPOSURE_TIME_RANGE ............. TYPE_INT64
     *   SENSOR_INFO_SENSITIVITY_RANGE ............... TYPE_INT32  (no int64)
     *   CONTROL_AVAILABLE_SCENE_MODES ............... TYPE_BYTE   (la critica)
     *   CONTROL_AE/AF/AWB_AVAILABLE_MODES ........... TYPE_BYTE
     *   CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES ...... TYPE_INT32
     *   CONTROL_AE_COMPENSATION_RANGE ............... TYPE_INT32
     *   CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES . TYPE_BYTE
     *   NOISE_REDUCTION_AVAILABLE_* / FLASH_INFO_AVAILABLE / FLASH_MODE ... TYPE_BYTE
     *   REQUEST_AVAILABLE_CAPABILITIES .............. TYPE_BYTE   (no int32)
     *   SCALER_AVAILABLE_PROCESSED_SIZES / REQUEST_AVAILABLE_CHARACTERISTICS_KEYS ... TYPE_INT32
     */
    uint8_t facing = (uint8_t)cam.facing;
    int32_t orientation = cam.orientation;
    uint8_t cfa = (uint8_t)cam.cfa;
    uint8_t nr_modes[] = { ANDROID_NOISE_REDUCTION_MODE_OFF, ANDROID_NOISE_REDUCTION_MODE_FAST,
                           ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY };
    uint8_t ae_modes[] = { ANDROID_CONTROL_AE_MODE_ON };
    uint8_t af_modes[] = { ANDROID_CONTROL_AF_MODE_OFF };
    uint8_t awb_modes[] = { ANDROID_CONTROL_AWB_MODE_OFF };
    uint8_t flash_avail[] = { ANDROID_FLASH_INFO_AVAILABLE_FALSE };
    uint8_t flash_mode = ANDROID_FLASH_MODE_OFF;
    /*
     * Estabilizacion de video OFF, heredado de Abobo7 (lineage-17.1, README de
     * device/xiaomi/mocha/patches): el nodo IMU del pipeline stock pide timestamps de sensor
     * que esta ROM no puede proveer ('NvIMUBasedVStabCalcTransforms: requested timestamp does
     * not exist') y, cuando se ejecuta, estanca el pipeline VI/ISP tras el primer frame sin
     * devolver buffers. Anunciar solo OFF hace que Camera1 reporte video-stabilization=false
     * y las aplicaciones no activen el nodo. Si no, la camara enumeraria pero el preview se
     * colgaria a la primera captura.
     */
    uint8_t vstab_modes[] = { ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF };
    uint8_t scene_modes[] = { (uint8_t)ANDROID_CONTROL_SCENE_MODE_DISABLED };
    int32_t active[] = { 0, 0, (int32_t)cam.sizes[0].w, (int32_t)cam.sizes[0].h };
    /*
     * pixelArraySize es UNA PAREJA (width, height); activeArraySize son CUATRO valores
     * (x, y, width, height). Compartir el array de 4 haria que cualquier lector interprete
     * pixelArraySize como 0x0 (cero tallas disponibles -> el mismo tipo de fallo
     * 'length=0; index=0' que muestra el log de la app de camara).
     */
    int32_t pix_array[] = { (int32_t)cam.sizes[0].w, (int32_t)cam.sizes[0].h };
    int32_t fps_min[] = { 1, cam.sizes[0].fps };
    int32_t ae_comp[] = { 0, 0 };
    uint8_t caps[] = { (uint8_t)ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE };
    int64_t exp_range[] = { 1000000LL, 30000000000LL };
    int32_t iso_range[] = { (int32_t)cam.iso_min, (int32_t)cam.iso_max };
    int32_t ch_keys[] = { ANDROID_LENS_FACING, ANDROID_SENSOR_ORIENTATION,
                          ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
                          ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
                          ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT,
                          ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
                          ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
                          ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES,
                          ANDROID_SCALER_AVAILABLE_JPEG_SIZES,
                          ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                          ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                          ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
                          ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
                          ANDROID_CONTROL_AE_AVAILABLE_MODES,
                          ANDROID_CONTROL_AF_AVAILABLE_MODES,
                          ANDROID_CONTROL_AWB_AVAILABLE_MODES,
                          ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                          ANDROID_CONTROL_AE_COMPENSATION_RANGE,
                          ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
                          ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
                          ANDROID_FLASH_INFO_AVAILABLE,
                          ANDROID_REQUEST_AVAILABLE_CAPABILITIES };

    bool ok = true;
    ok = upsert_metadata_entry(&md, ANDROID_LENS_FACING, &facing, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_SENSOR_ORIENTATION, &orientation, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, pix_array, 2, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, active, 4, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &cfa, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, exp_range, 2, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, iso_range, 2, camera_id, -1) && ok;

    /* LA ETIQUETA QUE EVITA EL SIGSEGV DE AOSP: si falta, derive() lee NULL. */
    ok = upsert_metadata_entry(&md, ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
                               scene_modes, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_CONTROL_AE_AVAILABLE_MODES, ae_modes, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_CONTROL_AF_AVAILABLE_MODES, af_modes, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awb_modes, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fps_min, 2, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_CONTROL_AE_COMPENSATION_RANGE, ae_comp, 2, camera_id, -1) && ok;

    ok = upsert_metadata_entry(&md, ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
                               nr_modes, sizeof(nr_modes), camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_FLASH_INFO_AVAILABLE, flash_avail, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
                               vstab_modes, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_FLASH_MODE, &flash_mode, 1, camera_id, -1) && ok;
    ok = upsert_metadata_entry(&md, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, caps, 1, camera_id, -1) && ok;

    /*
     * Semilla de tamanos: ensure_jpeg_metadata() y ensure_stream_configurations() construyen
     * JPEG_SIZES / STREAM_CONFIGURATIONS / MIN_FRAME_DURATIONS / STALL_DURATIONS a partir de
     * PROCESSED_SIZES ('has no source for JPEG sizes' y 'cannot synthesize stream
     * configurations' aparecian porque el blob del HAL estaba vacio).
     * Duraciones derivadas de los fps maximos declarados por los drivers del kernel.
     */
    int32_t psizes[16]; int np = 0;
    for (int i = 0; i < cam.nsizes; i++) {
        psizes[np++] = (int32_t)cam.sizes[i].w;
        psizes[np++] = (int32_t)cam.sizes[i].h;
    }
    ok = upsert_metadata_entry(&md, ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES, psizes, np, camera_id, -1) && ok;

    ok = upsert_metadata_entry(&md, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                               ch_keys, sizeof(ch_keys)/sizeof(int32_t), camera_id, -1) && ok;

    *metadata_ptr = md;
    ALOGI("%s: camara %d sembrada (%s): facing=%d orientacion=%d max=%ux%u %d tamano(s) - incluye CONTROL_AVAILABLE_SCENE_MODES",
          __FUNCTION__, camera_id, cam.name, facing, orientation, cam.sizes[0].w, cam.sizes[0].h, cam.nsizes);
    return ok;
}

static int camera_get_number_of_cameras(void)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;
    return gVendorModule->get_number_of_cameras();
}

static int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    ALOGV("%s camera_id: = %d", __FUNCTION__, camera_id);
    if (check_vendor_module())
        return 0;

    /*
     * El HAL Tegra rellena static_camera_characteristics; si no lo hace (medido 2026-10-28:
     * devolvia NULL y el provider AOSP 'CameraModule::getCameraInfo' ->
     * 'CameraMetadata::append' -> 'get_camera_metadata_entry_count(NULL)' -> SIGSEGV en el
     * arranque del provider) le entregamos antes un blob valido vacio para que el HAL pueda
     * anadirle entradas, y JAMAS devolvemos NULL al framework: los sintetizadores ensure_ y
     * force_legacy_ rellenan las etiquetas minimas sobre un blob vacio.
     */
    /*
     * 2026-10-28: ya NO se pre-asigna un blob antes de llamar al HAL. El sistema.log muestra
     * 795x 'E Camera2-Metadata: update: CameraMetadata is locked' dentro del propio proceso
     * del provider (pid 2513, justo tras 'NvIspAFinit' y 'ardbeg_imx179_power_off'), es decir:
     * el traductor del blob (NvMetadataTranslator) INTENTA escribir su metadata estatica y no
     * consigue escribirla en el objeto que gestiona 'NvCameraHal3Core::mStaticCameraInfoMap'.
     * Entregarle un blob nuestro (ajeno a ese mapa) no lo ayuda: el HAL devuelve
     * static_camera_characteristics = NULL igualmente. La metadata se construye despues, en
     * 'seed_static_metadata', sobre un blob nuestro.
     */
//    int ret = gVendorModule->get_camera_info(camera_id, info);
    int ret = gVendorModule->get_camera_info(camera_id, &vendor_camera_info);
//    fillStaticInfo();

    ALOGI("%s: camara %d HAL=%p facing=%d orientacion=%d version=0x%x",
            __FUNCTION__, camera_id, vendor_camera_info.static_camera_characteristics,
            vendor_camera_info.facing, vendor_camera_info.orientation,
            vendor_camera_info.device_version);

    info->facing = vendor_camera_info.facing;
    info->orientation = vendor_camera_info.orientation;
    info->device_version = normalize_camera_device_version(vendor_camera_info.device_version);

    if (vendorInfo[camera_id] == 0 ) {
        vendorInfo[camera_id] = clone_camera_metadata(vendor_camera_info.static_camera_characteristics);
        if (vendorInfo[camera_id] == NULL) {
            ALOGE("%s: clone_camera_metadata failed for camera %d (el HAL Tegra no rellena "
                  "static_camera_characteristics: NvMetadataTranslator no publica metadata estatica)",
                  __FUNCTION__, camera_id);
            /*
             * El HAL devuelve NULL de forma sistematica ('E NvMetadataTranslator: hwLevel=1, to 1'
             * y 795x 'E Camera2-Metadata: update: CameraMetadata is locked': su traductor interno
             * no consigue escribir en su propio blob). Sembramos nosotros la metadata estatica con
             * el catalogo del dispositivo; asi deriveCameraCharacteristicsKeys() tiene todas las
             * etiquetas que desreferencia y el provider sobrevive.
             */
            if (!seed_static_metadata(&vendorInfo[camera_id], camera_id)) {
                ALOGE("%s: camera %d seed_static_metadata fallo", __FUNCTION__, camera_id);
            }
        }

        if (vendorInfo[camera_id] == NULL) {
            ALOGE("%s: camera %d sin metadata y sin memoria: HAL no utilizable", __FUNCTION__, camera_id);
            info->static_camera_characteristics = NULL;
            return -ENODEV;
        }

        /*
         * Red de seguridad: si el HAL SI devolvio un blob pero le falta la etiqueta que AOSP
         * lee sin comprobar count, se siembra tambien sobre el blob del HAL.
         */
        camera_metadata_entry_t scene_entry;
        if (find_camera_metadata_entry(vendorInfo[camera_id],
                ANDROID_CONTROL_AVAILABLE_SCENE_MODES, &scene_entry) != 0) {
            ALOGE("%s: camera %d el blob del HAL carece de CONTROL_AVAILABLE_SCENE_MODES: sembrada",
                    __FUNCTION__, camera_id);
            seed_static_metadata(&vendorInfo[camera_id], camera_id);
        }

        camera_metadata_entry_t found_entry;
        int rc = find_camera_metadata_entry(
                vendorInfo[camera_id],
                ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS,
                &found_entry);
        if (rc == 0) {
            delete_camera_metadata_entry(vendorInfo[camera_id], found_entry.index);
        }

        if (!ensure_jpeg_metadata(&vendorInfo[camera_id], camera_id)) {
            ALOGE("%s: camera %d JPEG metadata synthesis failed", __FUNCTION__, camera_id);
        }

        if (!ensure_stream_configurations(&vendorInfo[camera_id], camera_id)) {
            ALOGE("%s: camera %d stream synthesis failed, keeping high-speed cleanup", __FUNCTION__, camera_id);
        }

        if (!ensure_request_capabilities(&vendorInfo[camera_id], camera_id)) {
            ALOGE("%s: camera %d request capabilities synthesis failed", __FUNCTION__, camera_id);
        }

        if (!sanitize_control_regions_and_overrides(&vendorInfo[camera_id], camera_id)) {
            ALOGE("%s: camera %d control region sanitization failed", __FUNCTION__, camera_id);
        }

        if (!force_legacy_hardware_level(&vendorInfo[camera_id], camera_id)) {
            ALOGE("%s: camera %d failed to force legacy hw level", __FUNCTION__, camera_id);
        }

        if (!force_legacy_request_limits(&vendorInfo[camera_id], camera_id)) {
            ALOGE("%s: camera %d failed to force legacy request limits", __FUNCTION__, camera_id);
        }

        has_valid_stream_configurations(vendorInfo[camera_id], camera_id, "final");
    }

    info->static_camera_characteristics = vendorInfo[camera_id];
    dump_camera_metadata(info->static_camera_characteristics, 1, 2);

    return ret;
}

static int camera_set_callbacks(const camera_module_callbacks_t *callbacks)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;
    return gVendorModule->set_callbacks(callbacks);
}

static void camera_get_vendor_tag_ops(vendor_tag_ops_t* ops)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return;
    return gVendorModule->get_vendor_tag_ops(ops);
}

static int camera_open_legacy(const struct hw_module_t* module, const char* id, uint32_t halVersion, struct hw_device_t** device)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;

    return gVendorModule->open_legacy(module, id, halVersion, device);
}
