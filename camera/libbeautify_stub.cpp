/*
 * libbeautify.so  --  STUB de reemplazo (2026-10-28)
 * ============================================================================
 * El HAL de camara stock de Tegra124 (camera.vendor.tegra.so) tiene como DT_NEEDED duro
 * a libbeautify.so, que a su vez necesita libFaceProc.so y libopencv24_tegra.so.
 *
 * libbeautify.so (blob de 2015) declara 251 simbolos indefinidos de la ABI C++ de GNU de
 * 2011: std::string copy-on-write (incluido el OBJETO _ZNSs4_Rep20_S_empty_rep_storageE),
 * std::cout / std::cerr, basic_ostream::operator<<, basic_ifstream/ofstream,
 * std::num_get / num_put, basic_istream::sentry ... mas la capa OpenCV 2.4. Nada de eso
 * existe en Android 12: libc++ solo exporta std::__1::... y el libstdc++.so del tree es un
 * stub de 10 KB. El unico donante completo era el propio libFaceProc.so (libstdc++ ligado
 * estaicamente, 6923 simbolos exportados), pero:
 *   - tiene DT_TEXTREL y el enlazador rechaza precargarlo:
 *     'CANNOT LINK EXECUTABLE "/vendor/bin/hw/android.hardware.camera.provider@2.4-service":
 *      "/system/vendor/lib/libFaceProc.so" has text relocations'   (medido 2026-10-28)
 *   - dlopen-eado desde el wrapper con RTLD_GLOBAL si carga, pero queda en el grupo del
 *     namespace del wrapper; el blob se carga con android_load_sphal_library y no consulta
 *     ese grupo: sigue 'cannot locate symbol'   (medido en dispositivo el 2026-10-28)
 *
 * Consumo REAL del blob (readelf --dyn-syms, no suposicion): camera.vendor.tegra.so importa
 * de libbeautify.so exactamente 17 simbolos, todos de LLAMADA DIRECTA (no aparece _ZTV...:
 * no hay metodos virtuales, asi que no hay que replicar tablas de vtable):
 *
 *   _ZN14SkinBeautifierC1Eb                                        SkinBeautifier::SkinBeautifier(bool)
 *   _ZN14SkinBeautifier6enableEb                                   ::enable(bool)
 *   _ZN14SkinBeautifier8setParamEiiii                              ::setParam(int,int,int,int)
 *   _ZN14SkinBeautifier11setCamParamEiib                           ::setCamParam(int,int,bool)
 *   _ZN14SkinBeautifier12setRectArrayEPi                           ::setRectArray(int*)
 *   _ZN14SkinBeautifier13setParamArrayEPi                          ::setParamArray(int*)
 *   _ZN14SkinBeautifier7setCudaEP16NvCameraHal3CudaP13NvMMBufferRec           ::setCuda(NvCameraHal3Cuda*,NvMMBufferRec*)
 *   _ZN14SkinBeautifier14setCudaPreviewEP16NvCameraHal3CudaP13NvMMBufferRec   ::setCudaPreview(...)
 *      (OJO: esas dos ortografias van SIN la 'E' de cierre de la lista de parametros -- es
 *       la cadena literal que usan a la vez camera.vendor.tegra.so (UND) y el libbeautify.so
 *       original (definicion). Se exportan las dos variantes, con y sin 'E', verificadas
 *       extrayendo las ortografias del blob original de vendor/xiaomi/mocha/proprietary/lib/.)
 *   _ZN14SkinBeautifier16updatePostParamsEv                        ::updatePostParams()
 *   _ZN14SkinBeautifier18cudaProcessPriviewEv                      ::cudaProcessPriview()  [sic: errata de NVIDIA]
 *   _ZN14SkinBeautifier7processEPhiiiS0_S0_iibb                    ::process(uchar*,int,int,int,uchar*,uchar*,int,int,bool,bool)
 *   _Z12RecoFaceInitv / _Z13RecoFaceClearv / _Z16RecoFaceFinalizev / _Z15RecoFaceAnalyzePhS_iiiiiif
 *   _ZN7android9WatermarkC1Ev                                      android::Watermark::Watermark()
 *   _ZN7android9Watermark17watermark_executeEPhS1_S1_iiii          android::Watermark::watermark_execute(uchar*,uchar*,uchar*,int,int,int,int)
 *
 * Se escriben los nombres mangled LITERALMENTE como extern "C" (cero incertidumbre de
 * mangling, cero dependencia de cabeceras NVIDIA) con la convencion de llamadas Itanium:
 * los metodos reciben 'this' como primer argumento, y el constructor de objeto COMPLETO
 * (C1) debe DEVOLVER 'this' en r0  --  el blob hace 'new SkinBeautifier(bool)' y usa el
 * valor de retorno. Los retornos enteros son 0, ambiguo de forma segura: 0 == NULL para un
 * llamador que comprueba punteros y == OK para uno que comprueba codigo de estado.
 *
 * Tamano de objeto: el blob reserva el tamano segun su cabecera original (>1 byte) y aqui
 * no se escribe en ningun miembro, asi que no hay desbordamiento posible.
 *
 * Coste funcional: se desactiva el suavizado de piel, el analisis de rostro de embellecer y
 * la marca de agua. Captura, preview, 3A, flash, HDR y grabacion del HAL NVIDIA no pasan
 * por libbeautify y quedan intactas.
 *
 * REVERSIBILIDAD: el blob original no se borra del tree (sigue en
 * vendor/xiaomi/mocha/proprietary/lib/libbeautify.so); solo se comenta su linea de copia en
 * vendor/xiaomi/mocha/mocha-vendor.mk. Marcha atras: reponer esa linea y borrar el modulo
 * 'libbeautify' de device/xiaomi/mocha/camera/Android.mk junto con este archivo.
 */

/* --- clase SkinBeautifier (metodos directos, this como primer argumento) --- */

extern "C" void *_ZN14SkinBeautifierC1Eb(void *self, bool) { return self; }
extern "C" void *_ZN14SkinBeautifierC2Eb(void *self, bool) { return self; }
extern "C" void _ZN14SkinBeautifierD1Ev(void *) {}
extern "C" void _ZN14SkinBeautifierD2Ev(void *) {}

extern "C" void _ZN14SkinBeautifier6enableEb(void *, bool) {}
extern "C" void _ZN14SkinBeautifier8setParamEiiii(void *, int, int, int, int) {}
extern "C" void _ZN14SkinBeautifier11setCamParamEiib(void *, int, int, bool) {}
extern "C" void _ZN14SkinBeautifier12setRectArrayEPi(void *, int *) {}
extern "C" void _ZN14SkinBeautifier13setParamArrayEPi(void *, int *) {}
extern "C" void _ZN14SkinBeautifier7setCudaEP16NvCameraHal3CudaP13NvMMBufferRec(void *, void *, void *) {}
extern "C" void _ZN14SkinBeautifier14setCudaPreviewEP16NvCameraHal3CudaP13NvMMBufferRec(void *, void *, void *) {}
extern "C" void _ZN14SkinBeautifier7setCudaEP16NvCameraHal3CudaP13NvMMBufferRecE(void *, void *, void *) {}
extern "C" void _ZN14SkinBeautifier14setCudaPreviewEP16NvCameraHal3CudaP13NvMMBufferRecE(void *, void *, void *) {}
extern "C" void _ZN14SkinBeautifier16updatePostParamsEv(void *) {}
extern "C" int  _ZN14SkinBeautifier18cudaProcessPriviewEv(void *) { return 0; }
extern "C" int  _ZN14SkinBeautifier7processEPhiiiS0_S0_iibb(void *, unsigned char *,
                                                            int, int, int,
                                                            unsigned char *, unsigned char *,
                                                            int, int, bool, bool) { return 0; }

/* --- funciones libres de deteccion/analisis de rostro --- */
extern "C" int  _Z12RecoFaceInitv(void) { return 0; }
extern "C" void _Z13RecoFaceClearv(void) {}
extern "C" void _Z16RecoFaceFinalizev(void) {}
extern "C" int  _Z15RecoFaceAnalyzePhS_iiiiiif(unsigned char *, unsigned char *,
                                              int, int, int, int, int, float) { return 0; }

/* --- android::Watermark --- */
extern "C" void *_ZN7android9WatermarkC1Ev(void *self) { return self; }
extern "C" void _ZN7android9WatermarkD1Ev(void *) {}
extern "C" int  _ZN7android9Watermark17watermark_executeEPhS1_S1_iiii(void *,
                                                                      unsigned char *,
                                                                      unsigned char *,
                                                                      unsigned char *,
                                                                      int, int, int, int) { return 0; }

/*
 * Anclaje: --gc-sections esta activo en el build (-ffunction-sections -fdata-sections).
 * Esta tabla global referencia todos los simbolos para garantizar que ni el enlazador ni el
 * linker GC los descarten. No la usa nada: su unica funcion es ser raiz de dinamica.
 */
extern "C" const void *const mocha_beautify_stub_anchor[] = {
    (const void *)_ZN14SkinBeautifierC1Eb,
    (const void *)_ZN14SkinBeautifierC2Eb,
    (const void *)_ZN14SkinBeautifierD1Ev,
    (const void *)_ZN14SkinBeautifierD2Ev,
    (const void *)_ZN14SkinBeautifier6enableEb,
    (const void *)_ZN14SkinBeautifier8setParamEiiii,
    (const void *)_ZN14SkinBeautifier11setCamParamEiib,
    (const void *)_ZN14SkinBeautifier12setRectArrayEPi,
    (const void *)_ZN14SkinBeautifier13setParamArrayEPi,
    (const void *)_ZN14SkinBeautifier7setCudaEP16NvCameraHal3CudaP13NvMMBufferRec,
    (const void *)_ZN14SkinBeautifier14setCudaPreviewEP16NvCameraHal3CudaP13NvMMBufferRec,
    (const void *)_ZN14SkinBeautifier7setCudaEP16NvCameraHal3CudaP13NvMMBufferRecE,
    (const void *)_ZN14SkinBeautifier14setCudaPreviewEP16NvCameraHal3CudaP13NvMMBufferRecE,
    (const void *)_ZN14SkinBeautifier16updatePostParamsEv,
    (const void *)_ZN14SkinBeautifier18cudaProcessPriviewEv,
    (const void *)_ZN14SkinBeautifier7processEPhiiiS0_S0_iibb,
    (const void *)_Z12RecoFaceInitv,
    (const void *)_Z13RecoFaceClearv,
    (const void *)_Z16RecoFaceFinalizev,
    (const void *)_Z15RecoFaceAnalyzePhS_iiiiiif,
    (const void *)_ZN7android9WatermarkC1Ev,
    (const void *)_ZN7android9WatermarkD1Ev,
    (const void *)_ZN7android9Watermark17watermark_executeEPhS1_S1_iiii,
};
