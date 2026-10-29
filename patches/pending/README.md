# `patches/pending/` — parches portados de Abobo7 (lineage-17.1) **aún no aplicados**

Nada de lo que hay aquí está aplicado al árbol. Cada fichero está escrito **contra
nuestros árboles 19.1** (anclajes verificados con `grep`/`sed` el 2026-10-27, números de
línea reales de nuestro árbol) para que `git apply --check` funcione al aplicarlo.

Regla de oro learnta en esta pasada: **los parches de Abobo7 son de un árbol Android 10**
(lmkd en `system/core`, `camera.provider@2.4-impl`, rutas `system/lib`, skia sin
`GrPersistentCacheUtils`). `git apply` de sus ficheros contra 19.1 falla casi siempre →
siempre portar hunk a hunk.

## Qué **NO** hay que traer (ya está en nuestro árbol o en 19.1 upstream) ✅

| item suyo | por qué no |
|---|---|
| `external_skia/0002-key-program-binaries-by-shader-language` | Skia de 19.1 ya claves la caché persistente por lenguaje: `external/skia/src/gpu/gl/builders/GrGLProgramBuilder.cpp:191,202` (`GrPersistentCacheUtils::ShaderMetadata`, `kGLSL_Tag`/`kSKSL_Tag`), `:63-66` load por key, `:263,301` unpack |
| `frameworks_base/0015` (`android_os_Debug.cpp` VmallocUsed→vmallocinfo) | ya presente: `frameworks/base/core/jni/android_os_Debug.cpp:623-624` (`if (i == MEMINFO_VMALLOC_USED && mem[i] == 0) outArray[i] = smi.ReadVmallocInfo()/1024;`) |
| `frameworks_base/0015` (`RenderThread.cpp` preload EGL inline) | nuestro `patches/frameworks/base/0006-hwui-preload-egl-on-render-thread.patch` hace lo mismo |
| `frameworks_base/0015` (`TaskPersister.java`) | nuestro `0005-task-snapshot-copy-null-guard` |
| binder lifetime (`proc->tsk = current->group_leader`) | ya en `kernel/xiaomi/mocha/drivers/android/binder.c:3446,3547-3548,3561` |
| padding `__u32 pad` en `binder_fd_array_object` | ya en `kernel/xiaomi/mocha/include/uapi/linux/android/binder.h` (`struct binder_fd_array_object` con `__u32 pad;`) |
| `bionic/0001-fcntl-fortify-old-kernel-headers` | nuestros headers 3.10 **sí** definen `O_TMPFILE` (`kernel/xiaomi/mocha/include/uapi/asm-generic/fcntl.h:87-92`) y 19.1 ya trae fallback runtime (`system/core/debuggerd/tombstoned.cpp:146-148,477`) |
| `system_core/0002` + `0003` (cgroups.json / task_profiles.json) | nuestros `0003-cgroups-json-v1-only`, `0004-legacy-kernel-cgroup-profiles`, `0005-init-skip-unconfigured-cgroup-writepid` |
| GraphicBuffer/Parcel ABI, Fence lifetime | nuestro `frameworks/native/0004-preserve-120-byte-graphicbuffer-and-48-byte-parcel-abi` (+ verificación de símbolos en `libui.so`) |
| EGL display resilience | nuestro `frameworks/base/0004-legacy-egl-display-resilience` + `EglManager.cpp:37-38,456-458` |
| camera provider binder `/dev/binder` | nuestro `hardware/interfaces/0003-camera-provider-legacy-framework-binder` + `system_prop.mk:94` (`ro.camera.legacy_system_binder=true`) |
| `hardware/nvidia/power` (repo 15.1) | no lo necesitamos: `patches/hardware/lineage/interfaces/0001-interfaces-add-vendor.lineage.power-1.0.patch` ya da HAL de power |
| Snap | no se construye (usamos Camera2 + `0001-selfie-preview-npe-fallback`) |
| ashmemd / apexd / vold-fuse / sepolicy ashmemd.te | repos ausentes o no usados en 19.1; arranque Apex y FUSE OK en nuestro build |

## Qué **SÍ** conviene traer (pendiente de aplicar) ❌

Detallo el anclaje exacto verificado en nuestro árbol para cada uno.

### 1. `pending/camera-01-legacy-text-relocations.patch` — ❌ **DESCARTADO (2026-10-27)**
El linker de A12 **solo rechaza TEXTREL en 64 bits**: en
`bionic/linker/linker.cpp:3138-3145` (`case DT_TEXTREL`) y `:3156-3162` (`DF_TEXTREL`) el
`DL_ERR(... has text relocations)` está dentro de `#if defined(__LP64__)`; en 32 bits
únicamente marca `has_text_relocations = true` y la carga continúa. Nuestro árbol es
32 bits (`device/xiaomi/mocha/camera/Android.mk: LOCAL_32_BIT_ONLY := true`), así que
`libFaceProc.so` (TEXTREL real, medido con `readelf -d`: `0x00000016 (TEXTREL)`) carga sin
trampa. No portar `ro.camera.legacy_text_relocations`, ni
`android_set_application_target_sdk_version(22)`, ni `libdl_android`.

Nota histórica del port que íbamos a hacer (por si cambia el análisis):

Port de su `hardware_interfaces/0002-allow-legacy-camera-system-binder` (segunda mitad):
* `hardware/interfaces/camera/provider/2.4/default/service.cpp` — nosotros ya tenemos el
  bloque del binder en `:46-49` (`if (property_get_bool("ro.camera.legacy_system_binder", …))`);
  añadir detrás:
  ```cpp
  char legacyTextRelocations[PROP_VALUE_MAX] = {};
  __system_property_get("ro.camera.legacy_text_relocations", legacyTextRelocations);
  if (strcmp(legacyTextRelocations, "true") == 0) {
      // libFaceProc.so del stack NVIDIA stock usa text relocations.
      android_set_application_target_sdk_version(22);
  }
  ```
  con `extern "C" void android_set_application_target_sdk_version(int);` (no es API NDK: es
  el entry point de `libdl_android`) ✓
* `hardware/interfaces/camera/provider/2.4/default/Android.bp` — añadir `"libdl_android"` a
  `shared_libs` del módulo del servicio (`:15` `shared_libs:` del bloque `cc_binary`).
* `device/xiaomi/mocha/system_prop.mk` — añadir `ro.camera.legacy_text_relocations=true`
  (junto a `ro.camera.legacy_system_binder=true`, línea 94).

### 2. `pending/camera-02-imager-discovery.patch` — ✅ **APLICADO (2026-10-27)**
Port aplicado en `device/xiaomi/mocha/camera/CameraWrapper.cpp`: `prepareImagerDevices()`
(`dlopen("libnvodm_imager.so")` → `NvOdmImagerDetInit()` → `NvOdmImagerDeviceDetect()` →
espera 20 ms×100 a que `/dev/imx179` y `/dev/ov5693` sean RW, handle vivo) llamado desde
`check_vendor_module()` antes de `hw_get_module_by_class("camera", "vendor")`.
Símbolos verificados en nuestro blob con `readelf --dyn-syms
vendor/xiaomi/mocha/proprietary/vendor/lib/libnvodm_imager.so`:
`NvOdmImagerDetInit` (#33) y `NvOdmImagerDeviceDetect` (#37); instalada en
`out/target/product/mocha/system/vendor/lib/libnvodm_imager.so` ✓

**Condición resuelta**: el kernel SÍ puede crear los nodos legacy, y ya estaba todo en el
árbol — lo que faltaba era la tabla de `camera_data_blob` que `of_camera_create_pdata()`
usa para dar `platform_data` a los clientes I2C. Cadena completa medida y corregida en
`plan_fixes_F8.md` §F9 (avance 2026-10-27):
* `patches/kernel/xiaomi/mocha/0001-board-ardbeg-sensors-restore-camera-pdata-blobs.patch`
  (`#if 0`→`#if 1`, stub fuera, CSI A/B/E sin DPD en tn8/mocha).
* `/dev/ov5693 0660 media camera` en `initfiles/ueventd.tn8.rc` (faltaba; imx179 y ad5823
  ya estaban).
* Etiquetas de cámara movidas a `sepolicy/mocha/file_contexts` (directorio **leaf**, el único
  que el merge de `vendor_file_contexts` consume): `camera.pcl`, `mipi-cal`, `imx179`,
  `ov5693`, `ad5823`, `focuser`. En `sepolicy/common/vendor/file_contexts` estaban desde el
  principio pero nunca llegaban al dispositivo (`ls -Z /dev/camera.pcl` daba `device`).

### 3. `pending/kernel-01-defconfig-camera-and-debug.patch`
En `kernel/xiaomi/mocha/arch/arm/configs/tegra12_android_defconfig`:
* ~~`CONFIG_VIDEO_DW9718=y`~~ → **no hace falta**: en `tegra124-mocha-camera.dtsi` el focuser
  del módulo trasero es `ad5823` (ya `CONFIG_VIDEO_AD5823=y`, y `ad5823` aparece enlazado en
  `/sys/bus/i2c/drivers/`) y el frontal no lleva VCM. `dw9718.c` existe en el árbol pero no
  lo referencia ningún `compatible` del DT nuestro.
* `# CONFIG_DEBUG_VM is not set` y `# CONFIG_CGROUP_DEBUG is not set` (nosotros los tenemos
  `=y` en el `.config` compilado; él no) → menos sobrecarga de memoria en 2 GB.
* **NO** tocar `CONFIG_ANDROID_BINDER_IPC_32BIT`: él usa wire-ABI de 32 bits
  (`binder_size_t = __u32`, `CONFIG_ANDROID_BINDER_IPC_32BIT=y`) para sus blobs; el nuestro
  fija `__u64` (`include/uapi/linux/android/binder.h:52-53`) y todo nuestro HIDL/FD-passing
  funciona sobre ese wire. Cambiarlo rompería el arranque gráfico ✓✗✗

### 4. `pending/codec2-01-use-gralloc-row-pitch.patch` — alta prioridad (vídeo HW)
Su `frameworks_av/0001-legacy-gralloc-codec2` fija lo que nosotros solo mitad:
nuestro `frameworks/av/media/codec2/components/avc/C2SoftAvcDec.cpp:538-549` hace
```cpp
uint32_t displayStride = mStride;
if (outBuffer) {
    // Decode into the CPU YV12 buffer (Tegra gralloc cannot allocate YUV buffers).
    displayStride = ALIGN32(mWidth);
}
```
es decir **asume** un pitch de 32-align, pero gralloc0 de Tegra alinea las filas YV12 a
**512 bytes** (aunque el ancho sea 128) → el decoder escribe con un stride distinto del
real → imagen sesgada / "Output buffer too small". Su versión lee el pitch real:
```cpp
const C2PlanarLayout &layout = outBuffer->layout();
const auto &y = layout.planes[C2PlanarLayout::PLANE_Y];
const auto &u = layout.planes[C2PlanarLayout::PLANE_U];
const auto &v = layout.planes[C2PlanarLayout::PLANE_V];
if (layout.type != C2PlanarLayout::TYPE_YUV || layout.numPlanes != 3
        || y.colInc != 1 || u.colInc != 1 || v.colInc != 1
        || y.rowInc < int32_t(mWidth) || (y.rowInc & 1)
        || u.rowInc != y.rowInc / 2 || v.rowInc != u.rowInc) {
    ALOGE("Unsupported output plane layout"); return false;
}
displayStride = y.rowInc;
```
Sitios en nuestro árbol: AVC `:538` (`setDecodeArgs`) y `:798` + `ensureDecoderState`
(`fetchGraphicBlock(ALIGN32(mWidth), …)`, `cpuStride = ALIGN32(mWidth)`); HEVC
`frameworks/av/media/codec2/components/hevc/C2SoftHevcDec.cpp:372,522` ✓
Nuestro árbol ya usa `wView.layout().planes[0].rowInc` en `:931,1100` (rutas de copia CPU)
✓ — la corrección es coherente con eso.

### 5. `pending/frameworks-base-01-pms-skip-sockets.patch` — ruido/seguridad de datos
Su `frameworks_base/0015` salta entradas **AF_UNIX** al conciliar CE/DE. Nuestro
`frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java:26000-26004`
(no hay comprobación de socket ✓):
```java
final File[] files = FileUtils.listFilesOrEmpty(ceDir);
for (File file : files) {
    final String packageName = file.getName();
    try { assertPackageKnownAndInstalled(volumeUuid, packageName, userId); }
    catch (PackageManagerException e) { logCriticalInfo(Log.WARN, "Destroying " + file + …); }
```
Sin el guard, un socket legacy en el directorio de datos acaba en
`logCriticalInfo("Destroying … due to …")` + `InstallerException` ✗. Añadir:
```java
try {
    if (OsConstants.S_ISSOCK(Os.lstat(file.getPath()).st_mode)) continue;
} catch (ErrnoException e) {
    if (e.errno == OsConstants.ENOENT) continue;
    Slog.w(TAG, "Unable to inspect app data entry " + file, e);
}
```
en los dos bucles (CE ~26000 y DE ~26017) + `import android.system.OsConstants;` ✓

### 6. Pendientes de leer antes de decidir (los dejo sin portar)
* `frameworks_native/0015` (176 líneas): `libs/renderengine/gl/GLESRenderEngine.cpp`,
  `include/renderengine/RenderEngine.h`, `services/surfaceflinger/Layer.cpp` → zona render
  GL/efectos; hay que ver qué cambia exactamente.
* `frameworks_native/0002-force-client-composition-option` → ¿fuerza client composition?
* `hardware_interfaces/0015` (50 l.): `audio/common/all-versions/default/service/service.cpp`
  y `graphics/composer/2.1/default/service.cpp`.
* `hardware_interfaces/0003-legacy-factory-bluetooth-address` → MAC de fábrica BT.
* `frameworks_av/0002-nvidia-avc-output-flags` (flags de salida del decoder AVC).
* `system_core/0015` hunk de `rootdir/init.rc`.

## Orden de aplicación sugerido
1. ~~camera-01~~ descartado; cámara ya portada (kernel blobs + ueventd + file_contexts +
   imager): compilar, flashear y verificar con la batería de `plan_fixes_F8.md` §F9
   (`ls -Z /dev/imx179`, `ls /sys/bus/i2c/devices/ | grep 2-00`, `dumpsys media.camera`).
2. kernel defconfig (ítem 3, solo `DEBUG_VM`/`CGROUP_DEBUG` fuera) → build de kernel.
3. renderengine-01 (ítem 7) → handoff GPU→CPU, impacta capturas/lecturas de buffer.
4. codec2-01 (ítem 4) → probar vídeo (Chrome/VLC/Gallery) y retirar nuestro apaño
   `frameworks/av.disabled/0001-C2SoftAvcDec-YV12-to-RGBA8888-software-conversion.patch`.
5. frameworks-base-01 (ítem 5) → ruido de StorageManagerService/PMS.

### 7. `pending/renderengine-01-publish-writes-on-unbind.patch` — alta prioridad (gráficos)
Port de su `frameworks_native/0015` (ver `notes_abobo17_analysis.md` §9). Anclajes en
nuestro árbol:
* `frameworks/native/libs/renderengine/gl/GLESRenderEngine.cpp` → `unbindFrameBuffer(Framebuffer* /*framebuffer*/)`
  (stub actual; `grep -n glTexImage2D` = 0 ✓) → versionar: desbindar FBO **y** liberar el
  almacenamiento de la textura del EGLImage sibling (`glBindTexture(getTextureName())`,
  `glTexImage2D(…,0,0,0,…,nullptr)`, `glBindTexture(…,0)`), manteniendo
  `glBindFramebuffer(GL_FRAMEBUFFER, 0)`.
* `:128-131` `~BindNativeBufferAsFramebuffer()` → extraer `unbind()` reutilizable con guard
  `mFramebuffer == nullptr` y llamarlo también antes de tomar el fence (`:485`
  `unbindFrameBuffer(mDrawingBuffer.get())` ya existe como punto de corte ✓).
* fast-path de `drawLayers` (`:912-939`) → fallback a `drawMesh(mesh)` si la transform no es
  identidad (incluidos `[0][3]`, `[1][3]`, `[2][3]`, `[3][3]`) o si
  `bounds.bottom - bounds.top <= 2*radius` ✓
* opcional: llevar su test `drawLayers_r15ReusedOutputCpuReadback` a
  `frameworks/native/libs/renderengine/tests/RenderEngineTest.cpp` como cobertura ✓

## PENDIENTE (medido 2026-10-28): etiquetas SELinux de los datos del HAL NVIDIA en /data

Medido montando /data rw desde TWRP (tablet en health-charging):

    /data/nvcam/                drwxrwxrwx root:root
      factory_OTP_0.bin         -rw------- 1047:1005   u:object_r:system_data_root_file:s0
      nvcam_cache_0.bin         -rw------- 1047:1005   u:object_r:system_data_root_file:s0
      nvcam_cache_1.bin         -rw------- 1047:1005   u:object_r:system_data_root_file:s0

- DAC ya es correcto: 1047=AID_CAMERASERVER, 1005=AID_AUDIO coinciden con
  'user cameraserver' / 'group audio ...' del rc del provider -> no es un problema de permisos.
- El problema es la etiqueta: heredan de /data ('system_data_root_file'), y un dominio vendor
  (hal_camera_default) no puede leerlas. Hoy el device es permissive -> solo se registro
  'avc: denied { read/open } ... /data/nvcam/nvcam_cache_0.bin' y el HAL pudo seguir
  (de hecho escribio esos caches el 2026-10-27 23:53, o sea la capa NVMM camera funciona).
- Al pasar a enforcing habra que:
  1) anadir a sepolicy/mocha/file_contexts una entrada para estas rutas, p.ej.
       /data/nvcam(/.*)?          u:object_r:vendor_data_file:s0   (o un tipo propio)
       /data/calibration\.bin     u:object_r:vendor_data_file:s0
  2) y en sepolicy/mocha/hal_camera_default.te los permisos correspondientes
     (dir: search; file: {open read getattr}; create si el HAL reescribe los caches),
     comprobando que no viola los neverallow de data_file_type (el build de sepolicy los aplica).
  3) Verificar en vivo con: restorecon -Rv /data/nvcam && ls -Z /data/nvcam
     y getenforce / avc: denied en logcat.
- No se aplico hoy para no mezclar variables: primero hay que confirmar preview/frames.

## CORREGIDO 2026-10-29: /data/nvcam NO existia (causa real del SIGSEGV del provider)

Medido en el device tras el arranque del 2026-10-29:

    ls: /data/nvcam: No such file or directory

y el blob tiene las rutas hard-codedas (strings drivers de libnvmm_camera_v3.so):

    /data/nvcam/factory_OTP   /data/nvcam/nvcam_cache   /data/nvcam/factory_EEPROM
    /data/nvcam/factory_copy  /data/nvcam/factory_device /data/nvcam/CameraParameters.yml

El blob **no comprueba el NULL que devuelve NvOsFopen**, asi que la falta del directorio se
convierte en `NvCameraIspDumpFactoryToFile -> NvOsFwrite(NULL)` -> SIGSEGV fault addr 0x0 y el
provider entra en bucle de reinicio (`LegacyCameraProviderImpl_2_4::initialize`).

Fix aplicado (identico a `abobo7_device/rootdir/etc/init.tegra.rc:57`), en
`initfiles/init.tegra.rc` dentro de `on post-fs-data`:

    mkdir /data/nvcam 0700 cameraserver camera

Validado en vivo (mkdir manual + reboot): `Dump factory file into /data/nvcam/factory_OTP_0.bin
completed`, aparecen `nvcam_cache_0.bin` y `nvcam_cache_1.bin`, `dumpsys media.camera` reporta
`Number of camera devices: 2` y `com.android.camera2` abre la camara 0.

Lo que QUEDA para enforcing sigue siendo la etiqueta (recipe de Abobo7):

    sepolicy: type nv_camera_data_file, file_type, data_file_type, core_data_file_type;
    file_contexts: /data/nvcam(/.*)?  u:object_r:nv_camera_data_file:s0
    cameraserver.te / hal_camera_default.te:
        allow X nv_camera_data_file:dir  create_dir_perms;
        allow X nv_camera_data_file:file create_file_perms;
    (abobo7_device/sepolicy/file.te:81, file_contexts:208, cameraserver.te:20-21)

Anadir tambien las denegaciones medidas hoy (permissive=1):

    hal_camera_default -> /dev/nvmap            chr_file read/write/open   (hoy etiqueta 'device')
    hal_camera_default -> /dev/nvhost-ctrl      chr_file ioctl 0x4807
    hal_camera_default -> camera_config_prop    file read/open
