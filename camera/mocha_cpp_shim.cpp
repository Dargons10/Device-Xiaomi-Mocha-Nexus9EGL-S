/*
 * Shim ABI (C++ runtime) para los blobs NVIDIA de camara heredados de Android 5/6.
 *
 * camera.vendor.tegra.so y sus librerias (libbeautify.so, libFaceProc.so, ...) se
 * compilaron en 2015 contra **libstdc++** de Android L/M. En Android 12 solo existe
 * libc++: `libbeautify.so` referencia std::ios_base::Init::Init()/~Init()  (si <iostream>
 * aparece en cualquier TU) y ese simbolo VIVE en libc++.so, pero libbeautify no enlaza
 * libc++ (enlaza el libstdc++.so legacy de /system/lib, que solo tiene stubs) ->
 *   dlopen failed: cannot locate symbol "_ZNSt8ios_base4InitD1Ev"
 *
 * El arreglo es un shim vendor que SI enlaza libc++: al precargarlo con LD_PRELOAD pasa al
 * *shared group* del enlazador (bionic/linker/linker_namespaces.cpp: "This group consists of
 * the main executable, LD_PRELOAD and DF_1_GLOBAL libraries") y arrastra libc++ con el, de
 * modo que los blobs lo encuentran aunque no lo enlacen. Funciona en este dispositivo:
 * medido en vivo el 2026-10-27 (con LD_PRELOAD de libmocha_sensor_shim.so el error de
 * Singleton<SensorManager>::sLock desaparecio y subio el siguiente simbolo).
 *
 * Se referencia de verdad un simbolo de libc++ (no vale -Wl,--as-needed: el enlazador
 * descartaria la dependencia) y ademas se fuerza --no-as-needed.
 */

#include <ios>
#include <new>
#include <stdexcept>

/*
 * Definiciones de la ABI GNU libstdc++ (namespace `std::` a secas, SIN `__1`) que libc++
 * NO proporciona. Medido en out/.../system/lib/libc++.so: libc++ solo exporta
 * _ZNSt3__18ios_base4Init* (std::__1::...). libbeautify.so (DT_NEEDED de
 * camera.vendor.tegra.so) declara UND _ZNSt8ios_base4InitC1Ev y _ZNSt8ios_base4InitD1Ev,
 * y solo enlaza el libstdc++.so legacy de /system/lib, que es un stub de 10 KB sin ellos.
 *
 * El unico donante completo del tree era libFaceProc.so (6923 simbolos, libstdc++ ligado
 * estaicamente). No sirve de nada: tiene DT_TEXTREL y Android 12 rechaza precargarlo
 * ('CANNOT LINK EXECUTABLE ... has text relocations'), y si se dlopen-ea desde el wrapper
 * con RTLD_GLOBAL queda en el grupo del namespace del wrapper, que el blob (cargado por
 * android_load_sphal_library) no consulta -- comprobado en vivo el 2026-10-28: libFaceProc
 * se cargo OK y libbeautify siguio sin localizar el simbolo.
 *
 * Los Init()/~Init() solo inicializan los objetos iostream globales; el blob no depende de
 * su comportamiento mas alla de enlazarlos. Los __throw_* y new/delete si se llaman, asi
 * que se implementan de verdad (rebotando a libc++).
 */
/*
 * OJO (medido 2026-10-28): definir aqui el family antiguo de operator new/delete (_Znwj,
 * _Znaj, _ZdlPv, _ZdaPv) o los lanzadores _ZSt*__throw_* PROVOCO SIGSEGV en el provider:
 * al estar en el shared group (LD_PRELOAD) el enlazador resuelve esos simbolos para TODO el
 * proceso (libutils/libbinder incluidos) y 'android::add_sysprop_change_callback' fallo en
 * 'android::VectorImpl::VectorImpl' durante call_constructors (signal 11, null deref 0x1c).
 * libc++ ya exporta _Znwj/_ZdlPv con el ABI correcto: no hay que tocarlos.
 * Solo se definen los Init de iostreams, que es lo que el loader pidio textualmente.
 */
extern "C" void _ZNSt8ios_base4InitC1Ev(void) {}
extern "C" void _ZNSt8ios_base4InitC2Ev(void) {}
extern "C" void _ZNSt8ios_base4InitD1Ev(void) {}
extern "C" void _ZNSt8ios_base4InitD2Ev(void) {}

extern "C" void mocha_cpp_shim_keepalive(void)
{
    /* Referencia real a libc++.so: std::__1::ios_base::Init::Init() y su dtor. */
    static std::ios_base::Init io_init;
    (void)io_init;
}
