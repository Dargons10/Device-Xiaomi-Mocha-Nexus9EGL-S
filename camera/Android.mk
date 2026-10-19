LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# Mocha camera HAL - wrapper over the NVIDIA Tegra vendor blob
# (camera.vendor.tegra.so).  This module is what the camera provider
# loads as hw_get_module("camera"):  Camera3Wrapper adapts the legacy
# HAL3 blob to the Android 12 camera3 device interface, CameraWrapper
# shims the HAL1 module/device entry points, LegacyMetadata sanitizes
# the metadata tags coming out of the old blob.
LOCAL_SRC_FILES := \
    Camera3Wrapper.cpp \
    CameraWrapper.cpp \
    LegacyMetadata.cpp

LOCAL_SHARED_LIBRARIES := \
    libhardware \
    libhardware_legacy \
    liblog \
    libdl \
    libcamera_client \
    libutils \
    libcutils \
    libcamera_metadata \
    android.hardware.graphics.bufferqueue@1.0

LOCAL_C_INCLUDES += \
    system/core/include \
    system/core/base/include \
    system/media/camera/include \
    frameworks/native/libs/nativebase/include \
    frameworks/native/libs/arect/include \
    frameworks/av/camera/include \
    system/libhidl/transport/token/1.0/utils/include \
    system/libhidl/base/include

LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_MODULE := camera.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

# New Mocha Camera HAL (HAL3, v4l2 pipeline - F1 repair line)
# Named camera.mocha so it no longer collides with the wrapper module
# name camera.tegra; installed in the vendor hw directory for manual
# A/B testing (rename over camera.tegra.so on /vendor).
LOCAL_SRC_FILES := \
    MochaCameraHAL.cpp \
    CameraPipeline.cpp \
    JpegEncoder.cpp \
    isp/DemosaicNEON.cpp \
    isp/ColorConvNEON.cpp

LOCAL_SHARED_LIBRARIES := \
    libhardware \
    liblog \
    libutils \
    libcutils \
    libcamera_metadata \
    libjpeg \
    libsync

LOCAL_C_INCLUDES := \
    system/core/include \
    system/media/camera/include \
    hardware/libhardware/include \
    frameworks/native/include \
    frameworks/av/camera/include \
    external/libjpeg-turbo

LOCAL_ARM_NEON := true
LOCAL_CFLAGS := -DLOG_TAG=\"MochaCameraHAL\" -std=c++11 -D__ARM_NEON__ -mfpu=neon -mfloat-abi=softfp

LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_MODULE := camera.mocha
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
