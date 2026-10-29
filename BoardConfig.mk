#
# Copyright (C) 2014 The CyanogenMod Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#


# Path
LOCAL_PATH := device/xiaomi/mocha

# Allow duplicate rules to override them
BUILD_BROKEN_DUP_RULES := true

# Legacy device: allow duplicate sysprop assignments (first one wins)
BUILD_BROKEN_DUP_SYSPROP := true

# Audio
USE_XML_AUDIO_POLICY_CONF := 1
BOARD_USES_GENERIC_AUDIO := false
BOARD_USES_ALSA_AUDIO := true
BOARD_USES_TINYHAL_AUDIO := true

# Architecture
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_ARCH := arm
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_CPU_VARIANT := cortex-a15
TARGET_CPU_SMP := true
TARGET_NOT_USE_GZIP_RECOVERY_RAMDISK := true

# Binder API
TARGET_USES_64_BIT_BINDER := true

# Bluetooth
BOARD_HAVE_BLUETOOTH := true
BOARD_HAVE_BLUETOOTH_BCM := true
BOARD_BLUETOOTH_USE_BRCM_SHARED_TRANSPORT := true
BCM_BLUETOOTH_MANTA_BUG := true
BOARD_BLUETOOTH_BDROID_BUILDCFG_INCLUDE_DIR := $(LOCAL_PATH)/bluetooth

# Board
TARGET_BOARD_PLATFORM := tegra
TARGET_NO_BOOTLOADER := true
TARGET_NO_RADIOIMAGE := true

# Boot animation
TARGET_SCREEN_HEIGHT := 2048
TARGET_SCREEN_WIDTH := 1536
TARGET_BOOTANIMATION_HALF_RES := true

# Camera
TARGET_HAS_LEGACY_CAMERA_HAL1 := true
TARGET_NEEDS_PLATFORM_TEXT_RELOCATIONS := true
#TARGET_LD_SHIM_LIBS += /system/vendor/lib/hw/camera.tegra.so|/system/vendor/lib/libcamera_shim.so

# dexpre-opt
  ifeq ($(HOST_OS),linux)
  ifneq ($(TARGET_BUILD_VARIANT),eng)
    WITH_DEXPREOPT_DEBUG_INFO := false
    USE_DEX2OAT_DEBUG := false
    DONT_DEXPREOPT_PREBUILTS := true
    WITH_DEXPREOPT_BOOT_IMG_AND_SYSTEM_SERVER_ONLY := true
  endif
endif

#ELF
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true
BUILD_BROKEN_PREBUILT_ELF_FILES := true
LOCAL_CHECK_ELF_FILES := false

# Exclude AudioFX
TARGET_EXCLUDES_AUDIOFX := true

# FM
BOARD_HAVE_BCM_FM := true
BOARD_HAVE_BCM_FM_SYSFS := "/sys/devices/platform/bcm_ldisc/"
BOARD_BRCM_HCI_NUM := 26

# FS
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_EXFAT_DRIVER := sdfat
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USES_MKE2FS := true

# Display
TARGET_SCREEN_DENSITY := 320

# Graphics
NUM_FRAMEBUFFER_SURFACE_BUFFERS := 3
SF_VSYNC_EVENT_PHASE_OFFSET_NS := 5000000
VSYNC_EVENT_PHASE_OFFSET_NS := 7500000

# Gralloc
#TARGET_ADDITIONAL_GRALLOC_10_USAGE_BITS := 0x2000U | 0x02000000U

# HIDL Manifest
DEVICE_MANIFEST_FILE := $(LOCAL_PATH)/manifest.xml
PRODUCT_ENFORCE_VINTF_MANIFEST_OVERRIDE := false

# Include
TARGET_SPECIFIC_HEADER_PATH := $(LOCAL_PATH)/include

# Vendor Init
TARGET_INIT_VENDOR_LIB      := libinit_mocha
TARGET_LIBINIT_DEFINES_FILE := $(LOCAL_PATH)/libmocha/init_mocha.cpp

# Include an expanded selection of fonts
EXTENDED_FONT_FOOTPRINT := true

# Kernel
# otg_wakelock.enabled=0: el kernel (CONFIG_USB_OTG_WAKELOCK=y, drivers/usb/phy/otg-wakelock.c)
# mantiene un wake_lock legacy "vbus-tegra-otg" mientras haya VBUS (cargador u host USB).
# Ese wake_lock usa __pm_stay_awake() (kernel/power/wakelock.c:218), deja el contador de
# wakeup events "in progress" != 0 y pm_get_wakeup_count(block=true) (drivers/base/power/wakeup.c)
# duerme para siempre: ni el HAL android.system.suspend@1.0-service (SystemSuspend.cpp:262 lee
# /sys/power/wakeup_count antes de escribir /sys/power/state) ni el autosleep del kernel pueden
# suspender mientras el tablet esta cargando/conectado por USB.
# Medido en el dispositivo: /sys/kernel/debug/wakeup_sources mostraba
#   vbus-tegra-otg  active_since=31435754  (distinto de 0; el unico activo)
# con usb.online=1 y status=Charging; al escribir 0 en
# /sys/module/otg_wakelock/parameters/enabled el contador de intentos del HAL dejo de
# bloquearse y el kernel volvio a dormir de verdad (dmesg:
#   PM: suspend entry ... / Wake39 for irq=52 / Restarting tasks ... done. / PM: suspend exit).
# Es el knob que el propio driver documenta (MODULE_PARM_DESC(enabled, "enable wakelock when
# VBUS present")). Si alguna vez hiciera falta revertirlo: quitar este parametro del cmdline.
BOARD_KERNEL_CMDLINE := vpr_resize androidboot.selinux=permissive vmalloc=400M otg_wakelock.enabled=0
BOARD_KERNEL_BASE := 0x10000000
BOARD_RAMDISK_OFFSET := 0x02000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_KERNEL_TAGS_OFFSET := 0x00000100
TARGET_KERNEL_SOURCE := kernel/xiaomi/mocha
TARGET_KERNEL_CONFIG := tegra12_android_defconfig
# Tegra124 kernel (Linux 3.10) uses GCC-specific flags (-march=armv5t, -Wa,-mfpu=softvfp+vfp)
# that Clang 12 does not support. Build the kernel with the GCC cross-compiler instead of Clang.
TARGET_KERNEL_CLANG_COMPILE := false
# Suppress -Werror in kernel sub-Makefiles: the GCC 4.9 cross-compiler emits
# a spurious '-ftarget=arm-linux-gnu' warning that -Werror promotes to an error.
TARGET_KERNEL_ADDITIONAL_FLAGS := CFLAGS_KERNEL="-Wno-error"
BOARD_KERNEL_IMAGE_NAME := zImage
BOARD_KERNEL_SEPARATED_DT := true
BOARD_MKBOOTIMG_ARGS := --ramdisk_offset $(BOARD_RAMDISK_OFFSET) --tags_offset $(BOARD_KERNEL_TAGS_OFFSET)
BOARD_CUSTOM_BOOTIMG_MK := $(LOCAL_PATH)/mkbootimg.mk
#BOARD_SYSTEMIMAGE_PARTITION_SIZE := 671088640 # 640 Mb stock partition table
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 3001024512 # 2.8 Gb
BOARD_USERDATAIMAGE_PARTITION_SIZE := 11196694528
BOARD_CACHEIMAGE_PARTITION_SIZE := 387973120
BOARD_BOOTIMAGE_PARTITION_SIZE := 20971520
BOARD_PERSISTIMAGE_PARTITION_SIZE := 16777216
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 20971520
BOARD_FLASH_BLOCK_SIZE := 131072

# LINEAGEHW
JAVA_SOURCE_OVERLAYS := org.lineageos.hardware|$(LOCAL_PATH)/lineagehw|**/*.java

# Malloc
MALLOC_SVELTE := true

# Memfd
TARGET_HAS_MEMFD_BACKPORT := true

# Offmode Charging
HEALTHD_ENABLE_TRICOLOR_LED := true
BOARD_CHARGER_ENABLE_SUSPEND := true
BOARD_CHARGER_SHOW_PERCENTAGE := true
BOARD_CHARGER_DISABLE_INIT_BLANK := true
BACKLIGHT_PATH := /sys/devices/platform/tegra12-i2c.0/i2c-0/0-002c/backlight/lcd-backlight/brightness
RED_LED_PATH := /sys/class/leds/red/brightness
GREEN_LED_PATH := /sys/class/leds/green/brightness
BLUE_LED_PATH := /sys/class/leds/blue/brightness


# OTA mocha
TARGET_RELEASETOOLS_EXTENSIONS := device/xiaomi/mocha/releasetools

# Per-application sizes for shader cache
MAX_EGL_CACHE_SIZE := 4194304
MAX_EGL_CACHE_ENTRY_SIZE := 262144

# Root symlinks baked into the (read-only) system image. Consumed by
# system/core/rootdir/Android.mk as <target>:<link_name>.
#
# /gl es OBLIGATORIO para el stack grafico NVIDIA/Tegra: libGLESv2_tegra.so
# (wrapper) rellena su mesa de funciones "client" en su helper de init haciendo
# NvOsLibraryLoad("/gl/libEGL_tegra.so") -> NvOsLibraryLoad pasa la literal tal
# cual a dlopen(). Si ese path no existe, dlopen falla (0x30008), el helper
# devuelve la mesa SIN poblar y la primera llamada gl* hace blx 0 -> pc=0
# (nuestro crash "glGetString+20", r0=0x1F03=GL_EXTENSIONS, sin ningun mensaje
# del driver). Desde Android 10 el root viene de system.img en solo lectura, así
# que el `symlink /system/lib/egl /gl` de init falla con EROFS y /gl jamas
# existe: hay que hornearlo en la imagen (mismo mecanismo que el arbol
# lineage-17.1 de Abobo7, que documenta exactamente esto).
BOARD_ROOT_EXTRA_SYMLINKS += /system/lib/egl:gl
# /lbh no aplica en mocha: no existe vendor/lbh en este volcado.
# /oem ya lo aporta rootdir (directorio en ROOT), no symlink.

# PowerHAL
TARGET_POWERHAL_VARIANT := tegra

# Recovery
LZMA_RAMDISK_TARGETS := recovery
TARGET_RECOVERY_DEVICE_DIRS += device/xiaomi/mocha
TARGET_RECOVERY_FSTAB := device/xiaomi/mocha/initfiles/fstab.tn8
BOARD_NO_SECURE_DISCARD := true

# RenderScript
OVERRIDE_RS_DRIVER := libnvRSDriver.so
BOARD_OVERRIDE_RS_CPU_VARIANT_32 := cortex-a15
TARGET_DISABLE_POSTRENDER_CLEANUP := true

# SELinux
SELINUX_IGNORE_NEVERALLOWS := true
# OJO: en A12 el nombre canónico es BOARD_VENDOR_SEPOLICY_DIRS. BOARD_SEPOLICY_DIRS
# SOLO se lo pasa a Soong (build/make/core/soong_config.mk:194 lo fusiona con
# BoardVendorSepolicyDirs), pero la parte Make (system/sepolicy/Android.mk,
# macro build_vendor_policy -> file_contexts.device.tmp -> file_contexts.bin, que es
# SELINUX_FC: la etiqueta que usa e2fsdroid al empaquetar system/vendor.img) NO lo
# ve. Por eso el '/gl u:object_r:rootfs:s0' de sepolicy/mocha/file_contexts entraba
# en out/.../vendor_file_contexts (Soong) pero no en META/file_contexts.bin, y el
# packaging abortaba con:
#   set_selinux_xattr: No such file or directory searching for label "/gl"
# Trampa adicional: los directorios se descubren con $(wildcard) en tiempo de kati,
# y kati NO se re-ejecuta si ningún Android.mk/BoardConfig.mk cambió. Al añadir un
# fichero nuevo de sepolicy hay que tocar un BoardConfig.mk/Android.mk (o makespec
# clean) para que aparezca en el grafo de ninja.
# Además: ni con BOARD_SEPOLICY_DIRS ni con BOARD_VENDOR_SEPOLICY_DIRS llegan nuestros
# directorios al merge Make en este árbol. Comprobado en el ninja generado por kati
# (out/build-lineage_mocha.ninja): los inputs de file_contexts.bin_intermediates/
# file_contexts.device.tmp son solo system/sepolicy/vendor/file_contexts y
# device/lineage/sepolicy/common/vendor/file_contexts (los del hook Lineage de
# build/make/core/config.mk:1168). Consecuencia: NI '/gl' ni las etiquetas de nuestros
# binarios custom (/vendor/bin/hw/*-tn8, /dev/tegra-*) estaban llegando nunca a las
# particiones; solo la política runtime de Soong los veía.
# Canales que SÍ funcionan (usados ya):
#   1) LOCAL_FILE_CONTEXTS en un modulo de PRODUCT_PACKAGES ->
#      ALL_MODULES.<m>.FILE_CONTEXTS -> file_contexts.modules.tmp
#      (build/make/core/Makefile:1601) -> file_contexts.bin = SELINUX_FC (:1606).
#      Lo usa initfiles/Android.mk con sepolicy/mocha/root_file_contexts para '/gl'.
#   2) Las etiquetas de los modulos Soong con `file_contexts:` también acaban ahí.
# TODO F4 (sepolicy enforcing): reorganizar sepolicy/common/{public,private,vendor} y
# sepolicy/lineage/{private,vendor} a las variables canónicas de A12
# (BOARD_VENDOR_SEPOLICY_DIRS / BOARD_PLAT_PUBLIC_SEPOLICY_DIR /
#  BOARD_PLAT_PRIVATE_SEPOLICY_DIR o SYSTEM_EXT_*), y verificar con
# 'strings out/target/product/mocha/obj/ETC/file_contexts.bin_intermediates/file_contexts.bin'
# que las rutas del device aparecen en la etiqueta de las particiones.
# Se usan rutas literales (no $(LOCAL_PATH)) por coherencia con Lineage/Abobo7: en el
# parseo Make del BoardConfig LOCAL_PATH no está garantizado.
# 2026-10-29: se anaden sepolicy/mocha/file.te y sepolicy/mocha/init.te (nuevos ficheros
# en un directorio ya declarado). Se toca este BoardConfig a proposito, solo con este
# comentario, para que kati re-parsee y el $(wildcard) de los directorios de sepolicy
# vuelva a listarlos (trampa documentada unas lineas arriba: kati no se re-ejecuta si
# ningun Android.mk/BoardConfig.mk cambia). Verificado en los outputs del build anterior:
# las etiquetas de sepolicy/mocha/file_contexts SI aparecen en el vendor_file_contexts
# staged (124 lineas: /dev/imx179, /dev/ov5693, /dev/focuser vienen solo de aqui) y
# tambien en obj/ETC/file_contexts.bin_intermediates/file_contexts.bin (ov5693 sale 2
# veces = variante plana + '\..*', igual que en nuestro fichero); '/gl' sale en el .bin
# gracias al canal LOCAL_FILE_CONTEXTS. La etiqueta de /data NO depende del .bin (que es
# lo que usa e2fsdroid al empaquetar): la aplica init en runtime.
BOARD_VENDOR_SEPOLICY_DIRS += \
    device/xiaomi/mocha/sepolicy/mocha \
    device/xiaomi/mocha/sepolicy/lineage-common \
    device/xiaomi/mocha/sepolicy/common
                      
# SHIMS
# OJO: en Android 12 TARGET_LD_SHIM_LIBS NO se consume (build/make y system/linkerconfig
# ya no lo leen; /linkerconfig/ld.config.txt no lleva entradas shim). El mecanismo real
# es LD_PRELOAD: ver initfiles/init.nvgpu_shims.rc y patches/system/core/0001-*.rc.
# Se mantienen las entradas como referencia/para recovery y builds antiguos.
TARGET_LD_SHIM_LIBS := \
    /system/vendor/lib/libnvomxadaptor.so|libmocha_omx.so \
    /system/lib/hw/camera.vendor.tegra.so|libmocha_camera.so \
    /system/lib/hw/camera.vendor.tegra.so|libmocha_libc.so \
    /system/vendor/lib/hw/hwcomposer.tegra.so|libshim_camera.so \
    /system/vendor/lib/libnvcap_video.so|libshim_camera.so \
    /system/vendor/lib/libnvgr.so|libshim_atomic.so \
    /system/vendor/lib/hw/camera.vendor.tegra.so|libnvomxadaptor_shim.so \
    /system/vendor/lib/libnvomxadaptor.so|libnvomxadaptor_shim.so 

# ThermalHAL
TARGET_THERMALHAL_VARIANT := tegra

#WEBGL in WebKit
ENABLE_WEBGL := true

# Wifi related defines
BOARD_WPA_SUPPLICANT_DRIVER      := NL80211
WPA_SUPPLICANT_VERSION           := VER_0_8_X
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_bcmdhd
BOARD_WLAN_DEVICE                := bcmdhd
BOARD_HOSTAPD_DRIVER             := NL80211
BOARD_HOSTAPD_PRIVATE_LIB        := lib_driver_cmd_bcmdhd
WIFI_DRIVER_FW_PATH_STA          := "/vendor/firmware/fw_bcmdhd.bin"
WIFI_DRIVER_FW_PATH_AP           := "/vendor/firmware/fw_bcmdhd_apsta.bin"
WIFI_DRIVER_FW_PATH_P2P          := "/vendor/firmware/fw_bcmdhd.bin"
WIFI_DRIVER_FW_PATH_PARAM        := "/sys/module/bcmdhd/parameters/firmware_path"
#WIFI_DRIVER_MODULE_ARG           := "iface_name=wlan0"
#WIFI_DRIVER_MODULE_NAME          := "bcmdhd"
WIFI_HIDL_UNIFIED_SUPPLICANT_SERVICE_RC_ENTRY := true

# workaround for devices that uses old GPU blobs
#BOARD_EGL_WORKAROUND_BUG_10194508 := true
                       
# Zygote whitelist extra paths
ZYGOTE_WHITELIST_PATH_EXTRA := \"/dev/nvhost-ctrl\",\"/dev/nvmap\",

# Security patch level
VENDOR_SECURITY_PATCH := 2018-01-01
