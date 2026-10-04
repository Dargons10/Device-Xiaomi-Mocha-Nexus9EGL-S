LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# New Mocha Camera HAL (HAL3)
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
    frameworks/av/include \
    external/libjpeg-turbo

LOCAL_ARM_NEON := true
LOCAL_CFLAGS := -DLOG_TAG=\"MochaCameraHAL\" -std=c++11 -D__ARM_NEON__ -mfpu=neon -mfloat-abi=softfp

LOCAL_32_BIT_ONLY := true
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_VENDOR_MODULE := true

LOCAL_MODULE := camera.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
