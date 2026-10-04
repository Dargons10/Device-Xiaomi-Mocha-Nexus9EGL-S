LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := mocha_camera_isp_test
LOCAL_SRC_FILES := DemosaicTest.cpp ../isp/DemosaicNEON.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/..
LOCAL_ARM_NEON := true
LOCAL_CFLAGS := -std=c++11
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_TAGS := tests
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := mocha_gralloc_probe
LOCAL_SRC_FILES := GrallocProbe.cpp
LOCAL_CFLAGS := -std=c++11
LOCAL_SHARED_LIBRARIES := libhardware liblog libcutils
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_TAGS := tests
include $(BUILD_EXECUTABLE)
