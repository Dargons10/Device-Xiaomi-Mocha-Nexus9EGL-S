LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE       := fstab.tn8
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := fstab.tn8
LOCAL_VENDOR_MODULE  := true
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.cal.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.cal.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.comms.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.comms.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.hdcp.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.hdcp.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.t124.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.t124.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.tegra.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.tegra.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.tlk.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.tlk.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.tn8.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.tn8.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.tn8.usb.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.tn8.usb.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := init.tn8_common.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := init.tn8_common.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
# Vehiculo para meter etiquetas de la RAIZ (/gl) en el file_contexts.bin que usa
# e2fsdroid: LOCAL_FILE_CONTEXTS -> ALL_MODULES.<m>.FILE_CONTEXTS ->
# file_contexts.modules.tmp (build/make/core/Makefile:1601) -> file_contexts.bin
# (SELINUX_FC, build/make/core/Makefile:1606). Es el unico canal del device que la
# via Make del sepolicy garantiza en este arbol; ver BoardConfig.mk y
# sepolicy/mocha/root_file_contexts.
LOCAL_FILE_CONTEXTS := $(LOCAL_PATH)/../sepolicy/mocha/root_file_contexts
include $(BUILD_PREBUILT)

#include $(CLEAR_VARS)
#LOCAL_MODULE       := init.ussrd.rc
#LOCAL_MODULE_CLASS := ETC
#LOCAL_SRC_FILES    := init.ussrd.rc
#LOCAL_VENDOR_MODULE := true
#LOCAL_MODULE_RELATIVE_PATH := init/hw
#include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := power.mocha.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := power.mocha.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := init/hw
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := power.tn8.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := power.tn8.rc
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE       := ueventd.tn8.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES    := ueventd.tn8.rc
LOCAL_MODULE_PATH  := $(TARGET_ROOT_OUT)
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE               := init.nvgpu_shims.rc
LOCAL_MODULE_CLASS         := ETC
LOCAL_SRC_FILES            := init.nvgpu_shims.rc
LOCAL_VENDOR_MODULE        := true
LOCAL_MODULE_RELATIVE_PATH := init
include $(BUILD_PREBUILT)



