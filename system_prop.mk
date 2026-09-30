# VNDK
# ro.product.first_api_level is set in vendor.prop (it is blacklisted from the
# system build.prop, so it must live in the vendor build.prop to be seen at boot).
PRODUCT_PROPERTY_OVERRIDES += \
    ro.vndk.version=9

# ADB
PRODUCT_PROPERTY_OVERRIDES += \
  persist.service.adb.enable=1 \
  persist.service.debuggable=1 \
  persist.sys.usb.config=mtp,adb \
  persist.sys.root_access=2 \
  ro.secure=0 \
  ro.adb.secure=0 \
  ro.debuggable = 1

# AptX
PRODUCT_PROPERTY_OVERRIDES += \
    persist.bt.enableAptXHD=true \
    persist.service.btui.use_aptx=1 \
    persist.vendor.bt.a2dp_offload_cap=sbc-aptx-aptxtws-aptxhd-aac-ldac \
    persist.vendor.btstack.a2dp_offload_cap=sbc-aptx-aptxtws-aptxhd-aac-ldacs

# BPF
PRODUCT_PROPERTY_OVERRIDES += \
    ro.kernel.ebpf.supported=false

# BT
PRODUCT_PROPERTY_OVERRIDES += \
    ro.bt.bdaddr_path=/data/mocha_btmacaddr.txt

# Timezone (default; TimeZoneDetector auto-detect falls back to a wrong zone)
PRODUCT_PROPERTY_OVERRIDES += \
    persist.sys.timezone=Europe/Madrid

# Dalvik
PRODUCT_PROPERTY_OVERRIDES += \
    dalvik.vm.heapstartsize=16m \
    dalvik.vm.heapgrowthlimit=192m \
    dalvik.vm.heapsize=512m \
    dalvik.vm.heaptargetutilization=0.75 \
    dalvik.vm.heapminfree=2m \
    dalvik.vm.heapmaxfree=8m

# DIDIM
PRODUCT_PROPERTY_OVERRIDES += \
   persist.tegra.didim.enable = 1 \
   persist.tegra.didim.video = 5 \
   persist.tegra.didim.normal = 3

# DRM
PRODUCT_PROPERTY_OVERRIDES += \
    drm.service.enabled=true

# fastbootd
PRODUCT_PROPERTY_OVERRIDES += \
    ro.fastbootd.available=true


# Graphics
PRODUCT_PROPERTY_OVERRIDES += \
    ro.opengles.version=196610 \
    ro.sf.lcd_density=320 \
    persist.tegra.compression=0 \
    persist.tegra.compositor=glcomposer \
    persist.tegra.decompression=disabled \
    ro.surface_flinger.use_frame_rate_api=false \
    debug.sf.latch_unsignaled=0 \
    debug.sf.disable_backpressure=0 \
    ro.input.noresample=1 \
    ro.zygote.disable_gl_preload=true \
    persist.vendor.tegra.composite.policy=composite-yuv \
    debug.hwc.force_gpu=0 \
    persist.sys.ui.hw=true \
    debug.hwui.use_buffer_age=false \
    debug.hwui.renderer=opengl \
    ro.surface_flinger.max_frame_buffer_acquired_buffers=2 \
    debug.sf.disable_hwc=1 \
    ro.sf.use_hwc_vsync=0 \
    persist.sf.force_gpu_composition=1 \
    debug.renderengine.backend=threaded

# Lineage genuine
PRODUCT_PROPERTY_OVERRIDES += \
    persist.lineage.nofool=true

# LMKD options
PRODUCT_PROPERTY_OVERRIDES += \
   ro.lmk.low=1001 \
   ro.lmk.medium=800  \
   ro.lmk.critical=0 \
   ro.lmk.critical_upgrade=false \
   ro.lmk.upgrade_pressure=100 \
   ro.lmk.downgrade_pressure=100 \
   ro.lmk.kill_heaviest_task=true \
   ro.lmk.kill_timeout_ms=100 \
   ro.lmk.use_minfree_levels=true

# OMX
PRODUCT_PROPERTY_OVERRIDES += \
   persist.media.treble_omx=false

# PBC
PRODUCT_PROPERTY_OVERRIDES += \
  pbc.enabled=0 \
  pbc.log=0 \
  pbc.board_power_threshold=20000 \
  pbc.low_polling_freq_threshold=1000 \
  pbc.rails=cpu,core,dram,gpu \
  pbc.cpu.power=/sys/bus/i2c/devices/7-0045/power1_input \
  pbc.cpu.cap=/dev/cpu_freq_max \
  pbc.cpu.cap.af=/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies \
  pbc.core.power=/sys/bus/i2c/devices/7-0043/power1_input \
  pbc.dram.power=/sys/bus/i2c/devices/7-0049/power1_input \
  pbc.gpu.power=/sys/bus/i2c/devices/7-004b/power1_input \
  pbc.gpu.cap=/dev/gpu_freq_max \
  pbc.gpu.cap.af=/sys/devices/platform/host1x/gk20a.0/devfreq/gk20a.0/available_frequencies

# Radio
PRODUCT_PROPERTY_OVERRIDES += \
    ro.radio.noril=yes
    
# Storage
PRODUCT_PROPERTY_OVERRIDES += \
    ro.sys.sdcardfs=true \
    persist.sys.fuse=false \
    persist.sys.fuse.passthrough=false \
    persist.sys.fflag.override.settings_fuse=false


# MediaProvider FUSE passthrough deadlocks /storage/emulated (ENOTCONN until
# reboot) whenever the provider restarts (storage_session_controller reset,
# user lifecycle) - the 3.10 sdcardfs path is the stable one on this device.
PRODUCT_PROPERTY_OVERRIDES += \
    persist.sys.fuse=0

# Wifi
PRODUCT_PROPERTY_OVERRIDES += \
    persist.debug.wfd.enable=1 \
    wifi.interface=wlan0 \
    ap.interface=wlan0 \
    persist.tegra.nvmmlite = 1 \
    persist.wlan.ti.calibrated = 0

