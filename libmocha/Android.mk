include $(call all-makefiles-under,$(LOCAL_PATH))

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        camera/Camera.cpp \
        camera/CameraMetadata.cpp \
        camera/CaptureResult.cpp \
        camera/CameraParameters2.cpp \
        camera/ICamera.cpp \
        camera/ICameraClient.cpp \
        camera/ICameraService.cpp \
        camera/ICameraServiceListener.cpp \
        camera/ICameraServiceProxy.cpp \
        camera/ICameraRecordingProxy.cpp \
        camera/ICameraRecordingProxyListener.cpp \
        camera/camera2/ICameraDeviceUser.cpp \
        camera/camera2/ICameraDeviceCallbacks.cpp \
        camera/camera2/CaptureRequest.cpp \
        camera/camera2/OutputConfiguration.cpp \
        camera/CameraBase.cpp \
        camera/CameraUtils.cpp \
        camera/VendorTagDescriptor.cpp \
        camera/CameraParameters.cpp \
        #sensor.c \
        camera.c


LOCAL_SHARED_LIBRARIES := \
        libcutils \
        libutils \
        liblog \
        libbinder \
        libhardware \
        libui \
        libgui \
        libcamera_metadata

LOCAL_C_INCLUDES += \
        $(LOCAL_PATH)/camera/include \
        system/media/camera/include \
        system/media/private/camera/include

LOCAL_HEADER_LIBRARIES := liblog_headers libsystem_headers libutils_headers

LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE := libmocha_camera
# Legacy-ABI camera client reimplementation (Abobo7 design).  The NVIDIA
# blob camera.vendor.tegra.so is DT_NEEDED-patched (patchelf) to depend on
# this library instead of the Android 12 system libcamera_client.so.
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_SRC_FILES := mocha_omx.cpp
LOCAL_SHARED_LIBRARIES := libbinder
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE := libmocha_omx
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)


include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
    sensor.c \
    fence_metadata_shim.cpp

LOCAL_SHARED_LIBRARIES := \
    libsensor \
    libbinder \
    libcutils \
    libutils \
    liblog \
    libcamera_client

LOCAL_C_INCLUDES += \
    frameworks/av/camera/include \
    frameworks/native/libs/sensor/include \
    system/media/camera/include

LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE := libmocha_sensor_shim
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MULTILIB := 32
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_SRC_FILES := powerservice_client.c 
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE := libpowerservice_client
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
    bionic/bionic_time_conversions.cpp \
    bionic/pthread_cond.cpp
LOCAL_SHARED_LIBRARIES := libc
LOCAL_MODULE := libmocha_libc
LOCAL_CLANG := true
LOCAL_CXX_STL := none
LOCAL_SANITIZE := never
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
include $(BUILD_SHARED_LIBRARY)

