/*
 * Copyright (C) 2026 Dargons10 <jdamr2p@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Stub vendor-visible de libcameraservice (plan A, HAL stock NVIDIA).
 *
 * Motivo: vendor/xiaomi/mocha/proprietary/lib/hw/camera.vendor.tegra.so declara
 * en su DT_NEEDED:
 *      libcameraservice.so, libgui.so, libmedia.so, libbinder.so
 * que en Android 12 son librerias SOLO de system (namespace "default"), y el
 * HAL de camera corre en el namespace "vendor" -> dlopen() falla y
 * hw_get_module_by_class() no prueba otra variante ("if this fails, we're
 * doomed", hardware/libhardware/hardware.c) -> "Number of camera devices: 0".
 *
 * Comprobado con `readelf --dyn-syms` sobre el blob: de sus 345 simbolos
 * indefinidos, NINGUNO proviene de libcameraservice (los constructores/destructors
 * de CameraMetadata/CameraParameters2 estan resueltos inline desde cabeceras).
 * Por tanto basta con exponer en vendor un shared object con ese nombre de
 * fichero para satisfacer el DT_NEEDED, sin arrastrar codigo de system a vendor.
 *
 * OJO: si en algun momento el blob SI necesitara simbolos reales de
 * libcameraservice, este stub provocaria un "undefined symbol" al dlopen y
 * habria que sustituirlo por una copia real (o por libmocha_camera, que ya
 * reexporta esa capa desde device/xiaomi/mocha/libmocha).
 */

/* Simbolo dummy: evita una unidad de traduccion vacia (algunos enlazadores
 * eliminan el .so si no aporta nada) y permite comprobar en dispositivo que el
 * stub esta efectivamente cargado:  nm -D /vendor/lib/libcameraservice.so */
int __mocha_libcameraservice_stub_present(void) { return 1; }
