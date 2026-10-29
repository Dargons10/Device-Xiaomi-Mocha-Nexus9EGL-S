LOCAL_PATH := $(call my-dir)

# Sondeo de la pila grafica NVIDIA/Tegra (debug). No entra en la configuracion normal
# salvo que se anada "mocha_glprobe" a PRODUCT_PACKAGES.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := glprobe.cpp
LOCAL_MODULE := mocha_glprobe
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := 32
LOCAL_SHARED_LIBRARIES := \
    libEGL \
    libGLESv2 \
    libdl
LOCAL_CFLAGS := -O1 -Wno-unused-variable -Wno-unused-parameter
include $(BUILD_EXECUTABLE)
