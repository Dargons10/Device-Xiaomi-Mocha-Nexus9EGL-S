/*
 * Copyright (C) 2012, The CyanogenMod Project
 * Copyright (C) 2017, The LineageOS Project
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

#define LOG_TAG "Camera3Wrapper"
#include <cutils/log.h>

#include "CameraWrapper.h"
#include "Camera3Wrapper.h"

typedef struct wrapper_camera3_device {
    camera3_device_t base;
    int id;
    camera3_device_t *vendor;
    struct wrapper_camera3_callback_ops* callback_ops;
} wrapper_camera3_device_t;

typedef struct wrapper_camera3_callback_ops {
    camera3_callback_ops_t base;
    const camera3_callback_ops_t* real;
    int camera_id;
} wrapper_camera3_callback_ops_t;

#define VENDOR_CALL(device, func, ...) ({ \
    wrapper_camera3_device_t *__wrapper_dev = (wrapper_camera3_device_t*) device; \
    __wrapper_dev->vendor->ops->func(__wrapper_dev->vendor, ##__VA_ARGS__); \
})

#define CAMERA_ID(device) (((wrapper_camera3_device_t *)(device))->id)

static camera_module_t *gVendorModule = 0;

static void sanitize_result_metadata(android::CameraMetadata* metadata)
{
    static const uint32_t kLegacyBadResultTags[] = {
        1048578,
        589834,
        589835,
        1638400,
        1638401,
        1638402,
        1703936,
        1769472,
        1769473,
        1835013,
    };

    size_t removed = 0;
    for (size_t i = 0; i < sizeof(kLegacyBadResultTags) / sizeof(kLegacyBadResultTags[0]); ++i) {
        uint32_t tag = kLegacyBadResultTags[i];
        if (metadata->exists(tag)) {
            ALOGI("%s: removing problematic result tag %u", __FUNCTION__, tag);
            metadata->erase(tag);
            removed++;
        }
    }
    if (removed > 0) {
        ALOGI("%s: removed %zu problematic result tags", __FUNCTION__, removed);
    }
}

static void camera3_notify_callback(const camera3_callback_ops_t* callback_ops,
        const camera3_notify_msg_t* msg)
{
    const wrapper_camera3_callback_ops_t* wrapper =
            reinterpret_cast<const wrapper_camera3_callback_ops_t*>(callback_ops);
    if (wrapper == NULL || wrapper->real == NULL || wrapper->real->notify == NULL) {
        return;
    }
    wrapper->real->notify(wrapper->real, msg);
}

static void camera3_process_capture_result_callback(const camera3_callback_ops_t* callback_ops,
        const camera3_capture_result_t* result)
{
    const wrapper_camera3_callback_ops_t* wrapper =
            reinterpret_cast<const wrapper_camera3_callback_ops_t*>(callback_ops);
    if (wrapper == NULL || wrapper->real == NULL || wrapper->real->process_capture_result == NULL) {
        return;
    }

    if (result != NULL) {
        ALOGI("%s: frame=%u partial=%u result_ptr=%p output_buffers=%p", __FUNCTION__,
                result->frame_number, result->partial_result, result->result, result->output_buffers);
    }

    // Drop null results from legacy Tegra blobs to prevent HIDL layer crashes
    if (result == NULL) {
        ALOGE("%s: dropping null result from vendor blob", __FUNCTION__);
        return;
    }

    if (result->result == NULL) {
        wrapper->real->process_capture_result(wrapper->real, result);
        return;
    }

    android::CameraMetadata sanitized;
    camera_metadata_t* cloned = clone_camera_metadata(result->result);
    if (cloned == NULL) {
        wrapper->real->process_capture_result(wrapper->real, result);
        return;
    }
    sanitized.acquire(cloned);
    sanitize_result_metadata(&sanitized);

    camera3_capture_result_t patched = *result;
    const camera_metadata_t* locked_result = sanitized.getAndLock();
    if (locked_result == NULL) {
        ALOGE("%s: frame=%u null locked_result, skipping", __FUNCTION__, patched.frame_number);
        return;
    }

    // More tolerant to broken vendor blob results
    // Vendor blob may pass garbage partial_result or corrupted metadata pointers
    // Use our sanitized metadata regardless of vendor's state
    ALOGI_IF(patched.partial_result == 0 || patched.partial_result > 16 || patched.result == NULL,
            "%s: frame=%u vendor has garbage partial_result=%u, using sanitized",
            __FUNCTION__, patched.frame_number, patched.partial_result);

    patched.partial_result = 0;
    patched.result = locked_result;
    wrapper->real->process_capture_result(wrapper->real, &patched);

    // The metadata is now owned by the framework
}

static int check_vendor_module()
{
    int rv = 0;
    ALOGV("%s", __FUNCTION__);

    if(gVendorModule)
        return 0;

    rv = hw_get_module_by_class("camera", "vendor", (const hw_module_t **)&gVendorModule);
    if (rv)
        ALOGE("failed to open vendor camera module");
    return rv;
}

/*******************************************************************
 * Camera3 wrapper fixup functions
 *******************************************************************/

static const camera_metadata_t * camera3_fixup_construct_default_request_settings(android::CameraMetadata metadata)
{
    static const uint32_t kLegacyBadTemplateTags[] = {
        1048578, // availableModes (bad type on legacy blob)
        589834,  // unknown legacy vendor/depth-adjacent tag
        589835,  // unknown legacy vendor/depth-adjacent tag
        1638400, // maxDepthSamples (bad type on legacy blob)
        1638401, // availableDepthStreamConfigurations (bad type)
        1638402, // availableDepthMinFrameDurations (bad type)
        1703936, // unknown depth-adjacent tag
        1769472, // unknown depth-adjacent tag
        1769473, // unknown depth-adjacent tag
    };

    for (size_t i = 0; i < sizeof(kLegacyBadTemplateTags) / sizeof(kLegacyBadTemplateTags[0]); ++i) {
        uint32_t tag = kLegacyBadTemplateTags[i];
        if (metadata.exists(tag)) {
            ALOGI("%s: removing problematic template tag %u", __FUNCTION__, tag);
            metadata.erase(tag);
        }
    }

    return metadata.release();
}

/*******************************************************************
 * implementation of camera_device_ops functions
 *******************************************************************/

static int camera3_initialize(const camera3_device_t *device, const camera3_callback_ops_t *callback_ops)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
        (uintptr_t)(((wrapper_camera3_device_t*)device)->vendor));

    if (!device)
        return -1;

    wrapper_camera3_device_t* wrapper_dev = (wrapper_camera3_device_t*)device;
    if (wrapper_dev->callback_ops == NULL) {
        wrapper_dev->callback_ops = (wrapper_camera3_callback_ops_t*)calloc(1,
                sizeof(wrapper_camera3_callback_ops_t));
        if (wrapper_dev->callback_ops == NULL) {
            ALOGE("%s: callback ops allocation failed", __FUNCTION__);
            return -ENOMEM;
        }
    }

    // Set up callback wrapping to intercept vendor results
    // This allows us to filter null results and sanitize malformed metadata
    wrapper_dev->callback_ops->real = callback_ops;
    wrapper_dev->callback_ops->camera_id = wrapper_dev->id;
    wrapper_dev->callback_ops->base.process_capture_result = camera3_process_capture_result_callback;
    wrapper_dev->callback_ops->base.notify = camera3_notify_callback;

    return VENDOR_CALL(device, initialize, &wrapper_dev->callback_ops->base);
}

static int camera3_configure_streams(const camera3_device *device, camera3_stream_configuration_t *stream_list)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
        (uintptr_t)(((wrapper_camera3_device_t*)device)->vendor));

    if (!device)
        return -1;

    android_dataspace_t original_dataspace[16];
    memset(original_dataspace, 0, sizeof(original_dataspace));

    if (stream_list != NULL && stream_list->streams != NULL) {
        ALOGI("%s: camera %d operation_mode=%u num_streams=%zu",
                __FUNCTION__, CAMERA_ID(device), stream_list->operation_mode,
                stream_list->num_streams);

        for (size_t i = 0; i < stream_list->num_streams; ++i) {
            camera3_stream_t* stream = stream_list->streams[i];
            if (stream == NULL) {
                ALOGI("%s: camera %d stream[%zu] is null", __FUNCTION__, CAMERA_ID(device), i);
                continue;
            }

            ALOGI("%s: camera %d stream[%zu] type=%d format=0x%x %ux%u usage=0x%llx dataspace=%d rotation=%d",
                    __FUNCTION__, CAMERA_ID(device), i, stream->stream_type, stream->format,
                    stream->width, stream->height,
                    static_cast<unsigned long long>(stream->usage),
                    static_cast<int>(stream->data_space),
                    stream->rotation);

            if (i < (sizeof(original_dataspace) / sizeof(original_dataspace[0]))) {
                original_dataspace[i] = stream->data_space;
            }

            if (stream->stream_type == CAMERA3_STREAM_OUTPUT &&
                    stream->format == HAL_PIXEL_FORMAT_BLOB &&
                    stream->data_space != static_cast<android_dataspace_t>(0)) {
                ALOGI("%s: camera %d stream[%zu] forcing BLOB dataspace %d -> 0",
                        __FUNCTION__, CAMERA_ID(device), i,
                        static_cast<int>(stream->data_space));
                stream->data_space = static_cast<android_dataspace_t>(0);
            }
        }
    }

    int ret = VENDOR_CALL(device, configure_streams, stream_list);

    if (stream_list != NULL && stream_list->streams != NULL) {
        for (size_t i = 0; i < stream_list->num_streams; ++i) {
            camera3_stream_t* stream = stream_list->streams[i];
            if (stream == NULL) {
                continue;
            }

            if (stream->stream_type == CAMERA3_STREAM_OUTPUT &&
                    stream->format == HAL_PIXEL_FORMAT_BLOB &&
                    i < (sizeof(original_dataspace) / sizeof(original_dataspace[0])) &&
                    stream->data_space != original_dataspace[i]) {
                ALOGI("%s: camera %d stream[%zu] restoring BLOB dataspace %d -> %d",
                        __FUNCTION__, CAMERA_ID(device), i,
                        static_cast<int>(stream->data_space),
                        static_cast<int>(original_dataspace[i]));
                stream->data_space = original_dataspace[i];
            }
        }
    }

    // Observabilidad del camino de captura. El retorno del vendor y los max_buffers
    // que el HAL Tegra asigna a cada stream permiten distinguir
    // 'configure_streams rechazo la configuracion' de 'configuro y no llegan frames',
    // que es exactamente el segundo cuadro del 'Waiting on the syncpt semaphore'.
    ALOGI("%s: camera %d vendor configure_streams retorno=%d num_streams=%zu",
            __FUNCTION__, CAMERA_ID(device), ret,
            stream_list ? stream_list->num_streams : (size_t)0);
    if (stream_list != NULL && stream_list->streams != NULL) {
        for (size_t i = 0; i < stream_list->num_streams; ++i) {
            camera3_stream_t* stream = stream_list->streams[i];
            if (stream == NULL) {
                continue;
            }
            ALOGI("%s: camera %d stream[%zu] resultado max_buffers=%u format=0x%x %ux%u type=%d",
                    __FUNCTION__, CAMERA_ID(device), i, stream->max_buffers,
                    stream->format, stream->width, stream->height, stream->stream_type);
        }
    }

    return ret;
}

static int camera3_register_stream_buffers(const camera3_device *device, const camera3_stream_buffer_set_t *buffer_set)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
        (uintptr_t)(((wrapper_camera3_device_t*)device)->vendor));

    if (!device)
        return -1;

    return VENDOR_CALL(device, register_stream_buffers, buffer_set);
}

static const camera_metadata_t *camera3_construct_default_request_settings(const camera3_device_t *device, int type)
{
    ALOGI("%s: device=%p vendor=%p type=%d", __FUNCTION__, device,
        (void*)(((wrapper_camera3_device_t*)device)->vendor), type);

    if (!device) {
        ALOGE("%s: null device", __FUNCTION__);
        return NULL;
    }

    ALOGI("%s: calling vendor construct_default_request_settings", __FUNCTION__);
    android::CameraMetadata metadata;
    metadata = VENDOR_CALL(device, construct_default_request_settings, type);
    
    if (metadata.isEmpty()) {
        ALOGE("%s: vendor returned empty metadata for type %d", __FUNCTION__, type);
        return NULL;
    }
    
    ALOGI("%s: vendor returned metadata with %zu entries", __FUNCTION__, metadata.entryCount());
    return camera3_fixup_construct_default_request_settings(metadata);
}

static int camera3_process_capture_request(const camera3_device_t *device, camera3_capture_request_t *request)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
        (uintptr_t)(((wrapper_camera3_device_t*)device)->vendor));

    if (!device)
        return -1;

    if (request == NULL || request->num_output_buffers == 0 || request->output_buffers == NULL) {
        ALOGE("%s: invalid capture request - dropping", __FUNCTION__);
        return -EINVAL;
    }

    return VENDOR_CALL(device, process_capture_request, request);
}

static void camera3_get_metadata_vendor_tag_ops(const camera3_device *device, vendor_tag_query_ops_t* ops)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
        (uintptr_t)(((wrapper_camera3_device_t*)device)->vendor));

    if (!device)
        return;

    VENDOR_CALL(device, get_metadata_vendor_tag_ops, ops);
}

static void camera3_dump(const camera3_device_t *device, int fd)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
        (uintptr_t)(((wrapper_camera3_device_t*)device)->vendor));

    if (!device)
        return;

    VENDOR_CALL(device, dump, fd);
}

static int camera3_flush(const camera3_device_t* device)
{
    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
        (uintptr_t)(((wrapper_camera3_device_t*)device)->vendor));

    if (!device)
        return -1;

    return VENDOR_CALL(device, flush);
}

static int camera3_device_close(hw_device_t *device)
{
    int ret = 0;
    wrapper_camera3_device_t *wrapper_dev = NULL;

    ALOGV("%s", __FUNCTION__);

    android::Mutex::Autolock lock(gCameraWrapperLock);

    if (!device) {
        ret = -EINVAL;
        goto done;
    }

    wrapper_dev = (wrapper_camera3_device_t*) device;

    wrapper_dev->vendor->common.close((hw_device_t*)wrapper_dev->vendor);
    if (wrapper_dev->callback_ops) {
        free(wrapper_dev->callback_ops);
        wrapper_dev->callback_ops = NULL;
    }
    if (wrapper_dev->base.ops)
        free(wrapper_dev->base.ops);
    free(wrapper_dev);
done:
    return ret;
}

/*******************************************************************
 * implementation of camera_module functions
 *******************************************************************/

/* open device handle to one of the cameras
 *
 * assume camera service will keep singleton of each camera
 * so this function will always only be called once per camera instance
 */

int camera3_device_open(const hw_module_t *module, const char *name,
        hw_device_t **device)
{
    int rv = 0;
    int num_cameras = 0;
    int cameraid;
    wrapper_camera3_device_t *camera3_device = NULL;
    camera3_device_ops_t *camera3_ops = NULL;

    android::Mutex::Autolock lock(gCameraWrapperLock);

    ALOGV("%s", __FUNCTION__);

    if (name != NULL) {
        if (check_vendor_module())
            return -EINVAL;

        cameraid = atoi(name);
        num_cameras = gVendorModule->get_number_of_cameras();

        if (cameraid > num_cameras) {
            ALOGE("camera service provided cameraid out of bounds, "
                    "cameraid = %d, num supported = %d",
                    cameraid, num_cameras);
            rv = -EINVAL;
            goto fail;
        }

        camera3_device = (wrapper_camera3_device_t*)malloc(sizeof(*camera3_device));
        if (!camera3_device) {
            ALOGE("camera3_device allocation fail");
            rv = -ENOMEM;
            goto fail;
        }
        memset(camera3_device, 0, sizeof(*camera3_device));
        camera3_device->id = cameraid;

        rv = gVendorModule->common.methods->open((const hw_module_t*)gVendorModule, name, (hw_device_t**)&(camera3_device->vendor));
        if (rv)
        {
            ALOGE("vendor camera open fail");
            goto fail;
        }
        ALOGV("%s: got vendor camera device 0x%08X", __FUNCTION__, (uintptr_t)(camera3_device->vendor));

        camera3_ops = (camera3_device_ops_t*)malloc(sizeof(*camera3_ops));
        if (!camera3_ops) {
            ALOGE("camera3_ops allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        memset(camera3_ops, 0, sizeof(*camera3_ops));

        camera3_device->base.common.tag = HARDWARE_DEVICE_TAG;
        camera3_device->base.common.version = CAMERA_DEVICE_API_VERSION_3_2;
        camera3_device->base.common.module = (hw_module_t *)(module);
        camera3_device->base.common.close = camera3_device_close;
        camera3_device->base.ops = camera3_ops;

        camera3_ops->initialize = camera3_initialize;
        camera3_ops->configure_streams = camera3_configure_streams;
        camera3_ops->register_stream_buffers = camera3_register_stream_buffers;
        camera3_ops->construct_default_request_settings = camera3_construct_default_request_settings;
        camera3_ops->process_capture_request = camera3_process_capture_request;
        camera3_ops->get_metadata_vendor_tag_ops = camera3_get_metadata_vendor_tag_ops;
        camera3_ops->dump = camera3_dump;
        camera3_ops->flush = camera3_flush;

        *device = &camera3_device->base.common;
    }

    return rv;

fail:
    if (camera3_device) {
        free(camera3_device);
        camera3_device = NULL;
    }
    if (camera3_ops) {
        free(camera3_ops);
        camera3_ops = NULL;
    }
    *device = NULL;
    return rv;
}
