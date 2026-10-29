# Parches de plataforma para mocha (LineageOS 19.1 / Android 12)

Aplicar con `sh device/xiaomi/mocha/patches/install.sh` **antes** de compilar
(instal.sh recorre `dirs` y aplica `*.patch` en orden alfabético dentro de cada
repo; si un parche ya está aplicado lo detecta con `git apply --check -R`).

Cada parche lleva cabecera `Subject:` y, en el cuerpo, **por qué** hace falta en
este hardware (kernel 3.10 Tegra + blobs NVIDIA stock de Android 4/5 + blobs
Broadcom). No borrar los comentarios: son el historial de por qué existe cada uno.

## Orden de aplicación dentro de cada repo (importante)

- `system/core`: `0001` (LD_PRELOAD en zygote32) → `0002` (guard cgroup2 uid) →
  `0003` (cgroups.json v1-only) → **`0004` (perfiles cgroup para kernel 3.10)** →
  **`0005` (writepid con controladores no configurados)** → `add_root`.
  `0004` se generó sobre el árbol ya modificado por `0003`: si se aplica `0004`
  sin `0003`, el contexto de `cgroups.json` no cuadra.
- `frameworks/base`: `0001..0005` (históricos) → `0006` (preload EGL en el
  RenderThread) → `0007` (`statx` no soportado).

## Índice rápido

| repo | parche | motivo corto |
|---|---|---|
| `system/core` | `0001-init-zygote32-ld-preload-shim` | A12 no consume `TARGET_LD_SHIM_LIBS`; hay que inyectar `LD_PRELOAD` de `libshadowcache_intercept.so` en zygote |
| `system/core` | `0002-cgroup2-uid-creation-guard` | no intentar crear grupos uid en jerarquía v2 inexistente |
| `system/core` | `0003-cgroups-json-v1-only` | quita el bloque `Cgroups2` (`/sys/fs/cgroup` es sysfs en 3.10) |
| `system/core` | **`0004-legacy-kernel-cgroup-profiles`** | `CONFIG_CPUSETS`/`CONFIG_BLK_CGROUP`/schedtune/freezer-v2 no existen → 574 fallos de `JoinCgroup` por arranque; también limpia los bloques equivalentes de `rootdir/init.rc` |
| `system/core` | **`0005-init-skip-unconfigured-cgroup-writepid`** | `WritePidToFiles()` abortaba en el primer destino inválido y perdía el grupo `/dev/cpuctl` real del servicio |
| `frameworks/native` | `0004-restore-120-byte-graphicbuffer-and-48-byte-parcel-abi` | ABI de `GraphicBuffer`/`Parcel` que usan las blobs stock (ver `notes_parcel_abi_48b.md`) |
| `hardware/interfaces` | `0001-camera-and-hwc2-changes`, `0002-preserve-tegra-gralloc-handle-references`, `0003-camera-provider-legacy-framework-binder`, `0004-allocator-passthrough-sanitize-usage` | HAL legacy Tegra (cámara, composer 2.1, allocator) |
| `hardware/interfaces` | **`0005-camera-provider-rc-preload-mocha-abi-shims`** | A12 no consume `TARGET_LD_SHIM_LIBS` y `RTLD_GLOBAL` desde `camera.tegra.so` no llega a los blobs cargados por `android_load_sphal_library` (medido 2026-10-27/28): hay que poner `setenv LD_PRELOAD` de `libmocha_cpp_shim.so:libmocha_sensor_shim.so:libshim_atomic.so` **solo** en el servicio camera provider (exportarlo globalmente rompe los binarios APEX). Mismo commit ya aplicado en el repo `hardware/interfaces` (`9a23d2779`); este parche es el registro para reconstruir el árbol desde cero |
| `frameworks/native` | **`0006-libui-120-byte-graphicbuffer-libbinder-48-byte-parcel-gui-surface-and-sf-shims`** | registro completo del commit local `2d0a6820be` (ABI de `GraphicBuffer`/`Parcel` que usan las blobs stock, shims de `Surface.cpp` y de `main_surfaceflinger.cpp`); genera ademas `libs/gui/Surface.cpp` y `services/surfaceflinger/main_surfaceflinger.cpp`, que no estaban cubiertos por `0002`/`0004`/`0005` |
| `frameworks/base` | `0004-legacy-egl-display-resilience`, `0005-task-snapshot-copy-null-guard` | resiliencia EGL/`eglReleaseThread` y guard de snapshots (idénticos a los del árbol lineage-17.1) |
| `frameworks/base` | **`0006-hwui-preload-egl-on-render-thread`** | quita el hilo *detached* de preload EGL (carrera con el estado global del driver Tegra) |
| `frameworks/base` | **`0007-hwui-memoize-unsupported-statx`** | `statx()` no existe en 3.10 (ENOSYS) → spam de log y syscalls inútiles |
| `kernel/xiaomi/mocha` | **`0001-board-ardbeg-sensors-restore-camera-pdata-blobs`** | `of_camera_create_pdata()` (`drivers/media/platform/tegra/cam_dev/of_camera.c:476`) sólo pone `bi->platform_data` si el auxdata instala la tabla `camera_data_blob` cuyos `.name` coinciden con las claves `platformdata` del DT. Con el bloque en `#if 0` y el stub `{ {} }` los drivers `imx179`/`ov5693`/`ad5823` no poweronean y `misc_register()` nunca crea `/dev/imx179` ni `/dev/ov5693` -> 0 cámaras. Además CSIA/B/E no entran en DPD en tn8/mocha (el CSI es el camino de datos). Ver `plan_fixes_F8.md` §F9 |
| `kernel/xiaomi/mocha` | **`0002-defconfig-disable-cgroup_debug-and-debug_vm`** | `CONFIG_CGROUP_DEBUG=y` y `CONFIG_DEBUG_VM=y` estaban activos en 2 GB de RAM (comprobaciones de VM y ficheros de depuración cgroup); Abobo7 17.1 los trae desactivados. `CONFIG_ANDROID_BINDER_IPC_32BIT` NO se toca: nuestro wire binder es de 64 bits y todo el HIDL/FD-passing funciona sobre él |
| `kernel/xiaomi/mocha` | **`0003-dts-include-mocha-camera-mc-and-disable-legacy-sensors`** | `tegra124-mocha.dts` desactiva el `vi` del SoC «para evitar conflicto con nuestro mocha-vi», pero el nodo `nvidia,mocha-vi` vive en `tegra124-mocha-camera-mc.dtsi`, que **no estaba incluido** -> no hay platform device para `drivers/media/platform/tegra/v4l2/tegra_vi.c` -> no hay `/dev/video*` -> 0 cámaras. El parche incluye el dtsi, añade `clocks=<&tegra_car 20/21>` + `clock-names="mclk"` (ids del propio `tvc.dtsi`) a `imx179@10`/`ov5693@36` y desactiva en defconfig los claim legacy (`VIDEO_IMX179/OV5693/AD5823/CAMERA`) para que los clientes i2c nacidos del DT se los quede el subdev V4L2. Ver `plan_fixes_F9.md` |

| `kernel/xiaomi/mocha` | **`0004-plan-A-stock-tvc-camera-host`** | Plan A (HAL stock NVIDIA): el kernel pasa al modelo TVC/pcl -> `tegra124-mocha.dts` incluye el dtsi de cámara, se elimina el dtsi `tvc` duplicado y el defconfig activa el host TVC. Registro del commit `4d03d64aa6d` del repo del kernel |
| `kernel/xiaomi/mocha` | **`0005-dts-point-camera-include-at-an-existing-dtsi`** | el include del dtsi de cámara apuntaba a un fichero que no existe en el árbol; el include debe apuntar al dtsi real. Registro del commit `43cebb30695` |
| `external/dng_sdk` | **`0001-remove-sdk_version-from-libdng_sdk`** | `sdk_version` en `libdng_sdk` obliga a una SDK de plataforma que no encaja con este tree; sin él la lib se construye con los headers del tree. Registro del commit `04a2068` |
| `lineage-sdk` | **`0001-health-null-guard-when-HAL-absent`** | `isChargingModeSupported()` lanzaba NPE cuando el HAL de health no está (mocha no tiene HAL health propio). Registro del commit `b4f8a06d` |
| `packages/apps/Settings` | **`0002-guard-null-Activity-in-SettingsPreferenceFragment`** | `hasNextButton()`/`getNextButton()` con Activity nula (prefs abiertos desde fuera del flujo de Setup) -> NPE. Registro del commit `ceaee0eefc` |
| `packages/apps/Trebuchet` | **`0001-remove-setupImageDecoder-from-QuickstepProcessInitializer`** | el initializer llamaba a un decodificador de imágenes no disponible en la pila gráfica de T124. Registro del commit `2dd9154f56` |
| `system/libhidl` | **`0001-VINTF-race-log-spam`** | spam por la carrera de VINTF al arranque (manifest aún no publicado); solo ruido, pero inundaba logcat y ocultaba fallos reales. Registro del commit `5787785` |
| `system/bt` | **`0002-bt-rt-scheduling-FATAL-to-WARNING`** | el kernel 3.10 no permite la planificación RT que Bluetooth expects: registraba FATAL y mataba la stack; se degrada a WARNING. Registro del commit `57ce4c963` (complementa a `0001-ldac-codec-unavailable-fix`) |

> Nota de registro: `frameworks/av/0004-camera-and-media-changes` esta **obsoleto**: hoy falla
> tanto hacia delante como hacia atras (`install.sh` lo marcaria FAILED) porque su contenido se
> reparte entre `0005`/`0006` (que si estan en el tree) y hunks que ya no existen. Hay que
> regenerarlo desde el estado real de `frameworks/av` antes de fiarse de el.

## Referencia cruzada

- `notes_abobo17_analysis.md` (raíz del árbol): lista completa de los parches del
  árbol **Abobo7 `lineage-17.1`** (base r15 validada en hardware) y su estado aquí:
  portado / candidato / no aplicable.
- `plan lineage-19.1.md`: registro de fases F0..F7 con la evidencia de cada hallazgo.

## Comprobación de un parche sin aplicar nada

```bash
cd <repo> && git apply --check device/xiaomi/mocha/patches/<repo>/XXXX.patch   # reversible
cd <repo> && git apply --check -R <mismo.patch>                                # ya aplicado
```

## Índice de estado global (reversibilidad)

El inventario completo de **todos** los cambios de la sesión (árbol + dispositivo), con
comandos de reversión exactos por repo y por parche, está en:

```
<root del árbol>/notes_estado_cambios_20261027.md
```

Resumen de estado de los parches de kernel (2026-10-27, tras commitear Plan A):
- `0001-board-ardbeg-sensors-restore-camera-pdata-blobs.patch` → **commiteado** (`4258d469855`)
- `0002-defconfig-disable-cgroup_debug-and-debug_vm.patch` → **commiteado dentro de `4d03d64aa6d`**
  (sus hunks `CGROUP_DEBUG`/`DEBUG_VM` viajaron con el commit del Plan A) ⇒ **no reaplicar**,
  ya está en el árbol ⚠️
- `0003-dts-include-mocha-camera-mc-and-disable-legacy-sensors.patch` → **REVERTIDO hoy**
  (ruta stock/TVC activa); reaplicarlo devuelve la ruta V4L2 `mocha-vi`
- `0004-plan-A-stock-tvc-camera-host.patch` → **commiteado** (`4d03d64aa6d`)

### 0004-plan-A-stock-tvc-camera-host.patch  (kernel)

Plan A (HAL stock NVIDIA): host de captura TVC en lugar de mocha-vi.
- `tegra124-mocha.dts`: pasa a incluir **`tegra124-mocha-camera.dtsi`** (el modelo TVC original
  de MiPad e1780 que ya está en el árbol del kernel) y re-habilita
  `vi`, `isp@54600000`, `isp@54680000` (estaban disabled para la ruta B).
- `tegra12_android_defconfig`: elimina la línea duplicada
  `# CONFIG_VIDEO_CAMERA is not set` (ganaba sobre `CONFIG_VIDEO_CAMERA=y` y TVC nunca se
  compilaba) y apaga `VIDEO_MOCHA_V4L2`, `TEGRA_VI_V4L2`, `TEGRA_CSI`, `*_V4L2` de sensores.
- Se elimina el `tegra124-mocha-camera-tvc.dtsi` escrito a mano: no tenía nodo `modules`.

**Motivo del cambio de DT (medido en vivo):** `of_camera_create_pdata()`
(`cam_dev/of_camera.c:487`) hace `of_find_node_by_name(of_node, "modules")`; sin ese nodo el
driver imprime `pcl-generic pcl-generic: module node not found` y devuelve `-ENODEV` ⇒ no se
crea `/dev/camera.pcl` ⇒ el HAL stock enumera **0 dispositivos**. El `tegra124-mocha-camera.dtsi`
de NVIDIA sí trae `camera-pcl` + `configuration = <0xAA55AA55>` + `modules` (module1 trasero
imx179+ad5823, module2 frontal ov5693, `badge_info="e1780_*_camera"`) + `profiles`
`pcl_IMX179`/`pcl_AD5823`/`pcl_OV5693` (busnum 2, addr 0x10/0xc/0x36, `detect`/`devid`,
secuencias `poweron`/`poweroff` con GPIO 219/221/222/223/225 y reguladores) y
`platformdata="ardbeg_*_pdata"`, que son justo los blobs que restaura el parche **0001**.

**Ya está commiteado en la rama del kernel** (`4d03d64aa6d`); el fichero es el registro
reversible. Para volver a la ruta B (V4L2):

```bash
git -C kernel/xiaomi/mocha revert 4d03d64aa6d        # deja el kernel como estaba
# OJO: ese revert también desactiva de nuevo CGROUP_DEBUG/DEBUG_VM (hunks del 0002):
git -C kernel/xiaomi/mocha apply device/xiaomi/mocha/patches/kernel/xiaomi/mocha/0002-*.patch
# y, equivalente para la cmara, reaplicar el 0003:
git -C kernel/xiaomi/mocha apply device/xiaomi/mocha/patches/kernel/xiaomi/mocha/0003-*.patch
```
