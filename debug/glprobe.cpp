// mocha_glprobe — sondeo de la pila grafica NVIDIA/Tegra (T124 / R32, blobs stock).
//
// Replica, paso a paso, lo que hace el RenderEngine GLES de SurfaceFlinger:
//   dlopen de los blobs -> eglGetDisplay -> eglInitialize -> eglBindAPI ->
//   eglChooseConfig -> eglCreatePbufferSurface -> eglCreateContext ->
//   eglMakeCurrent -> glGetString(...)
// y ademas informa de dlerror() y del mapa real de librerias (/proc/self/maps),
// para distinguir dos causas distintas del "pc 0x0" en glGetString+20:
//   (a) libglcore.so no llega a cargarse (namespace/enlazado) -> la mesa dispatch queda NULL
//   (b) libglcore si se carga, pero el registro de "client functions" no la rellena
//
// Si reproduce el fallo, el proceso muere con SIGSEGV igual que SurfaceFlinger (se ve el
// tombstone). Por eso imprime marcas antes de cada paso y vuelca maps antes del glGetString.

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static const char* const kBlobs[] = {
        "libnvos.so",
        "libnvrm.so",
        "libnvrm_graphics.so",
        "libnvcpl.so",
        "libnvglsi.so",
        "libnvwsi.so",
        "libnvrmapi_tegra.so",
        "libglcore.so",
        "/system/lib/egl/libEGL_tegra.so",
        "/system/vendor/lib/egl/libEGL_tegra_impl.so",
        "/system/lib/egl/libGLESv2_tegra.so",
        "/system/vendor/lib/egl/libGLESv2_tegra_impl.so",
};

static void dumpMaps(const char* tag) {
        printf("--- maps (%s): librerias graficas mapeadas ---\n", tag);
        FILE* f = fopen("/proc/self/maps", "r");
        if (!f) {
                printf("   no puedo abrir /proc/self/maps\n");
                return;
        }
        char line[512];
        int total = 0, hits = 0;
        while (fgets(line, sizeof(line), f)) {
                total++;
                if (strstr(line, ".so") &&
                    (strstr(line, "nv") || strstr(line, "glcore") || strstr(line, "tegra") ||
                     strstr(line, "egl") || strstr(line, "GLES"))) {
                        /* solo la ruta, sin direcciones */
                        const char* path = strchr(line, '/');
                        if (path) {
                                printf("   %s", path);
                                hits++;
                        }
                }
        }
        fclose(f);
        printf("   (%d lineas, %d entradas graficas)\n", total, hits);
        fflush(stdout);
}

static void showErr(const char* what) {
        EGLint e = eglGetError();
        printf("   %-26s EGL error = 0x%04x\n", what, e);
        fflush(stdout);
}

/* Fase NvCPL (sin GPU): que responde el blob REAL cuando no hay servicio 'nvcpl'.
   Prototipos leidos del disassembly de libnvcpl.so (0x14dc / 0x1524 / 0x1544 / 0x15ba):
     GetAppProfileSettingInt(a, key, out)      -> helper(key, out, 4)  (tamano 4 interno)
     GetOpenGlAppProfileSettingInt(a, key, out)-> helper(key); NULL => retorna 2 SIN tocar out
   Claves = las 14 que registr6 la traza F3 del stub del 25-10. */
/* fase 4 (`--load`): COMO RESUELVE EL WRAPPER GLESv2 LA LIBRERIA QUE LE POBLA LA MESA.
   Desassembly real de libGLESv2_tegra.so (ARM, no Thumb) en glGetString:
       mov r4,r0 ; bl 0x1a90 ; ldr r3,[r0,#0x11c] ; mov r0,r4 ; blx r3     <- r3 == 0 -> pc=0
   y el helper 0x1a90 hace, literalmente:
       if (*(u8*)(A+8) != 0) return A+12;                      // mesa ya inicializada
       h = NvOsLibraryLoad("/gl/libEGL_tegra.so");             // cadena exacta @ .rodata 0x3908
       if (h) return A+12;                                      // !!! devuelve la mesa SIN poblar
       fn = NvOsLibraryGetSymbol(h, "NvEglInitClientFunctions");// cadena exacta @ 0x391c
       if (!fn) return A+12;                                    // !!! mesa queda a CEROS
       err = fn(h, A, A+12);  cmp err,#0 / cmp err,#13 ...      // codigos 0 y 13 manejados
   (aparecen ademas "%s: Aborted initialization", "%s: Error in initialization" y
    "NvGetEs2Functions" @ 0x3974). O sea: si el load de "/gl/libEGL_tegra.so" falla, la mesa
   del wrapper se queda llena de NULLs y CUALQUIER gl* revienta en pc=0, sin avisar.
   Esta fase comprueba esas rutas exactas con el propio cargador de NVIDIA (solo dlopen). */
typedef void* (*nv_load_fn)(const char*);
typedef void* (*nv_getsym_fn)(void*, const char*);

static void runLoad(void) {
        void* nvos = dlopen("libnvos.so", RTLD_NOW | RTLD_GLOBAL);
        if (!nvos) {
                printf("   dlopen libnvos.so FALLO: %s\n", dlerror());
                return;
        }
        nv_load_fn load = (nv_load_fn)dlsym(nvos, "NvOsLibraryLoad");
        nv_getsym_fn getsym = (nv_getsym_fn)dlsym(nvos, "NvOsLibraryGetSymbol");
        printf("--- fase 4: cargador del wrapper GLESv2 (NvOsLibraryLoad/GetSymbol) ---\n");
        if (!load || !getsym) {
                printf("   simbolos no resueltos: load=%p getsym=%p\n", (void*)load, (void*)getsym);
                return;
        }
        static const char* cands[] = {
                "/gl/libEGL_tegra.so",
                "libEGL_tegra.so",
                "egl/libEGL_tegra.so",
                "/system/lib/egl/libEGL_tegra.so",
                "/system/vendor/lib/libEGL_tegra_impl.so",
        };
        for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
                dlerror();
                void* h = load(cands[i]);
                const char* err = dlerror();
                printf("   NvOsLibraryLoad(\"%s\") -> %p   dlerror=%s\n", cands[i], h, err ? err : "-");
                if (h) {
                        void* f1 = getsym(h, "NvEglInitClientFunctions");
                        void* f2 = getsym(h, "NvGetEs2Functions");
                        printf("      NvEglInitClientFunctions=%p   NvGetEs2Functions=%p\n", f1, f2);
                }
                fflush(stdout);
        }
        printf("   RTLD_DEFAULT: NvEglInitClientFunctions=%p  NvGetEs2Functions=%p\n",
               dlsym(RTLD_DEFAULT, "NvEglInitClientFunctions"),
               dlsym(RTLD_DEFAULT, "NvGetEs2Functions"));
        fflush(stdout);
}

static void runNvcpl(void) {
        static const char* const kKeys[] = {
                "3cf72823", "32153249", "AC0274", "83498426", "74095218", "48576893", "48576894",
                "B40D9E03d", "FF54EC97", "FF54EC98", "50299698", "50299699", "24189123", "73304097",
        };
        void* h = dlopen("libnvcpl.so", RTLD_NOW | RTLD_GLOBAL);
        printf("--- fase NvCPL (blob real, sin GPU) dlopen libnvcpl.so -> %p\n", h);
        if (!h) printf("   dlerror: %s\n", dlerror());
        fflush(stdout);

        int (*getGlInt)(const char*, const char*, int*) =
                (int (*)(const char*, const char*, int*))dlsym(RTLD_DEFAULT,
                                                               "NvCplGetOpenGlAppProfileSettingInt");
        int (*getAppInt)(const char*, const char*, int*) =
                (int (*)(const char*, const char*, int*))dlsym(RTLD_DEFAULT,
                                                               "NvCplGetAppProfileSettingInt");
        printf("   getGlInt=%p  getAppInt=%p\n", (void*)getGlInt, (void*)getAppInt);
        fflush(stdout);

        for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
                if (getGlInt) {
                        int v = 0x5A5A5A5A;
                        int r = getGlInt("surfaceflinger", kKeys[i], &v);
                        printf("   GLInt  %-12s ret=%d out=0x%08x%s\n", kKeys[i], r, v,
                               v == 0x5A5A5A5A ? " (out SIN escribir)" : "");
                        fflush(stdout);
                }
                if (getAppInt) {
                        int v = 0x5A5A5A5A;
                        int r = getAppInt("surfaceflinger", kKeys[i], &v);
                        printf("   AppInt %-12s ret=%d out=0x%08x%s\n", kKeys[i], r, v,
                               v == 0x5A5A5A5A ? " (out SIN escribir)" : "");
                        fflush(stdout);
                }
        }

        /* Las variantes de cadena, por si el GL core tambien pide strings (buffer pre-rellenado
           con 0x5A para ver si escribe). */
        int (*getGlStr)(const char*, const char*, char*, int) =
                (int (*)(const char*, const char*, char*, int))dlsym(
                        RTLD_DEFAULT, "NvCplGetOpenGlAppProfileSettingString");
        printf("   getGlStr=%p\n", (void*)getGlStr);
        fflush(stdout);
        if (getGlStr) {
                for (size_t i = 0; i < 4; i++) {
                        char buf[64];
                        memset(buf, 0x5A, sizeof(buf));
                        int r = getGlStr("surfaceflinger", kKeys[i], buf, (int)sizeof(buf));
                        printf("   GLStr  %-12s ret=%d buf[0]=0x%02x\n", kKeys[i], r,
                               (unsigned char)buf[0]);
                        fflush(stdout);
                }
        }
}

int main(int argc, char** argv) {
        bool soloBlobs = (argc > 1 && !strcmp(argv[1], "--blobs-only"));
        bool testNvcpl = false;
        bool callGl = false;
        bool glPhase = false;
        bool loadPhase = false;
        const char* logpath = nullptr;
        for (int i = 1; i < argc; i++) {
                if (!strcmp(argv[i], "--nvcpl")) testNvcpl = true;
                if (!strcmp(argv[i], "--call-gl")) callGl = true;
                if (!strcmp(argv[i], "--gl")) glPhase = true;
                if (!strcmp(argv[i], "--load")) loadPhase = true;
                if (!strcmp(argv[i], "--log") && i + 1 < argc) logpath = argv[++i];
        }

        /* --log: vuelca stdout a un fichero y LO ESCRIBE SIN BUFFER, para que el resultado
           sobreviva a un cuelgue/kill del proceso (con buffering completo se pierde todo). */
        if (logpath) {
                mkdir("/data/local", 0771);
                mkdir("/data/local/tmp", 0771);
                if (!freopen(logpath, "w", stdout))
                        fprintf(stderr, "freopen(%s): %s\n", logpath, strerror(errno));
                setvbuf(stdout, nullptr, _IONBF, 0);
                printf("mocha_glprobe: log -> %s\n", logpath);
                /* cortafuegos: si algo se queda colgado (eglMakeCurrent esperando conexion),
                   la senal SIGALRM mata el proceso sin colgar el arranque entero. */
                alarm(25);
                printf("mocha_glprobe: watchdog 25 s armado\n");
        }
        fflush(stdout);

        printf("== mocha_glprobe (uid %d, pid %d) ==\n", getuid(), getpid());
        fflush(stdout);

        /* 1. dlopen individual de cada blob, con dlerror() explicito */
        printf("--- fase 1: dlopen de blobs ---\n");
        for (size_t i = 0; i < sizeof(kBlobs) / sizeof(kBlobs[0]); i++) {
                void* h = dlopen(kBlobs[i], RTLD_NOW | RTLD_GLOBAL);
                if (h) {
                        printf("   dlopen %-46s OK\n", kBlobs[i]);
                } else {
                        printf("   dlopen %-46s FALLO: %s\n", kBlobs[i], dlerror());
                }
                fflush(stdout);
        }
        dumpMaps("tras dlopen");

        /* 2. simbolos que el wrapper GLESv2 busca para instalar su mesa de funciones */
        printf("--- fase 2: simbolos de registro de client functions ---\n");
        static const char* const kSyms[] = {
                "NvEglInitClientFunctions",
                "NvEglRegClientApi",
                "NvEglRedirInit",
                "NvGlEs2Init",
                "NvClientRedirInit",
                "NvGlEglGetFunctions",
                "NvCplGetAppProfileSettingString",
                "NvCplGetOpenGlAppProfileSettingString",
                "glGetString",
                /* registros de 'client functions' del stack NVIDIA (los que deberian poblar la
                   mesa del wrapper GLESv2; si ninguno es callable, la mesa queda NULL) */
                "NvEglRedirInit",
                "NvEglInitClientFunctions",
                "NvClientRedirInit",
                "NvGlEs2Init",
                "eglGetProcAddress",
        };
        for (size_t i = 0; i < sizeof(kSyms) / sizeof(kSyms[0]); i++) {
                void* s = dlsym(RTLD_DEFAULT, kSyms[i]);
                printf("   dlsym %-42s -> %p\n", kSyms[i], s);
        }
        fflush(stdout);

        if (soloBlobs) {
                printf("== fin (--blobs-only) ==\n");
                return 0;
        }

        /* 2b. --nvcpl: SOLO la parte NvCPL, sin tocar la GPU.
           Motivo: en cuanto se llama a eglInitialize/eglMakeCurrent con SurfaceFlinger en
           crash-loop, el device se cuelga entero (adbd deja de responder). La fase 5 no
           necesita GPU: solo llama a libnvcpl.so. */
        if (testNvcpl) {
                runNvcpl();
                if (!glPhase && !loadPhase) {
                        printf("== mocha_glprobe fin (--nvcpl, sin GPU) ==\n");
                        fflush(stdout);
                        return 0;
                }
                printf("   continua con las fases pedidas\n");
                fflush(stdout);
        }
        if (loadPhase) {
                runLoad();
                if (!glPhase) {
                        printf("== mocha_glprobe fin (--load, sin GPU) ==\n");
                        fflush(stdout);
                        return 0;
                }
        }

        /* 3. mismo flujo EGL que GLESRenderEngine::create */
        printf("--- fase 3: flujo EGL identico al RenderEngine ---\n");
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        printf("   eglGetDisplay(EGL_DEFAULT_DISPLAY) -> %p\n", dpy);
        if (dpy == EGL_NO_DISPLAY) showErr("eglGetDisplay");

        EGLint major = 0, minor = 0;
        EGLint ok = eglInitialize(dpy, &major, &minor);
        printf("   eglInitialize -> %d  (version %d.%d)\n", ok, major, minor);
        if (!ok) showErr("eglInitialize");

        printf("   eglBindAPI(ES) -> %d\n", eglBindAPI(EGL_OPENGL_ES_API));
        showErr("eglBindAPI");

        const EGLint cfgAttribs[] = {
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_NONE
        };
        EGLConfig cfg = nullptr;
        EGLint numCfg = 0;
        EGLint chosen = eglChooseConfig(dpy, cfgAttribs, &cfg, 1, &numCfg);
        printf("   eglChooseConfig -> %d (configs=%d, cfg=%p)\n", chosen, numCfg, cfg);
        if (numCfg == 0) printf("   AVISO: ninguna EGLConfig (igual que 'no suitable EGLConfig found')\n");
        showErr("eglChooseConfig");

        const EGLint pbAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        EGLSurface sur = eglCreatePbufferSurface(dpy, cfg, pbAttribs);
        printf("   eglCreatePbufferSurface -> %p\n", sur);
        showErr("eglCreatePbufferSurface");

        const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
        printf("   eglCreateContext -> %p\n", ctx);
        showErr("eglCreateContext");

        /* 3.a antes de makeCurrent: como mira el wrapper GLESv2 su mesa de funciones.
           Si eglGetProcAddress devuelve NULL aqui Y despues, la mesa queda sin poblar y
           glGetString+20 salta a 0x0 (firma exacta del tombstone de SF). */
        printf("   eglGetProcAddress(\"glGetString\") ANTES  -> %p\n", (void*)eglGetProcAddress("glGetString"));
        printf("   eglGetProcAddress(\"glGetIntegerv\") ANTES -> %p\n", (void*)eglGetProcAddress("glGetIntegerv"));

        printf("   ANTES de eglMakeCurrent\n");
        fflush(stdout);
        EGLint mc = eglMakeCurrent(dpy, sur, sur, ctx);
        printf("   eglMakeCurrent -> %d\n", mc);
        showErr("eglMakeCurrent");

        printf("   eglGetProcAddress(\"glGetString\") DESPUES -> %p\n", (void*)eglGetProcAddress("glGetString"));
        printf("   eglGetProcAddress(\"glGetIntegerv\") DESPUES -> %p\n", (void*)eglGetProcAddress("glGetIntegerv"));
        fflush(stdout);

        /* 5. NvCPL contra el blob REAL. Prototipos leidos del disassembly de libnvcpl.so
           (libnvcpl.so!NvCplGetAppProfileSettingInt en 0x14dc, ...OpenGl... en 0x1524):
             GetAppProfileSettingInt(a, key, out)      -> bl helper(key, out, 4); el tamano 4
                                                        es interno, NO lo pasa el llamante
             GetOpenGlAppProfileSettingInt(a, key, out)-> helper(key); si devuelve NULL
                                                        retorna 2 SIN tocar 'out'; si no,
                                                        tail-call a GetAppProfileSettingInt
           => sin servicio 'nvcpl' se espera ret=2 y 'out' sin escribir (lo que veria libglcore).
           arity real = 3 (el argc=4 que registró el stub v2 de F3 era su propio scan variadico). */
        /* 5. (ver runNvcpl() arriba; aqui solo se ejecuta si --call-gl) */
        if (false) {
                static const char* const kKeys[] = {
                        "3cf72823", "32153249", "AC0274", "83498426", "74095218", "48576893",
                        "48576894", "B40D9E03d", "FF54EC97", "FF54EC98", "50299698", "50299699",
                        "24189123", "73304097",
                };
                int (*getInt)(const char*, const char*, int*) =
                        (int (*)(const char*, const char*, int*))dlsym(
                                RTLD_DEFAULT, "NvCplGetOpenGlAppProfileSettingInt");
                int (*getIntApp)(const char*, const char*, int*) =
                        (int (*)(const char*, const char*, int*))dlsym(
                                RTLD_DEFAULT, "NvCplGetAppProfileSettingInt");
                printf("--- fase 5: NvCPL perfiles GL (blob real, arity 3) ---\n");
                printf("   getInt=%p  getIntApp=%p\n", (void*)getInt, (void*)getIntApp);
                for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
                        if (getInt) {
                                int v = 0x5A5A5A5A;
                                int r = getInt("surfaceflinger", kKeys[i], &v);
                                printf("   GLInt  %-12s ret=%d out=0x%08x%s\n", kKeys[i], r, v,
                                       v == 0x5A5A5A5A ? " (out SIN escribir)" : "");
                                fflush(stdout);
                        }
                        if (getIntApp) {
                                int v = 0x5A5A5A5A;
                                int r = getIntApp("surfaceflinger", kKeys[i], &v);
                                printf("   AppInt %-12s ret=%d out=0x%08x%s\n", kKeys[i], r, v,
                                       v == 0x5A5A5A5A ? " (out SIN escribir)" : "");
                                fflush(stdout);
                        }
                }
        }

        /* 6. aqui es donde SurfaceFlinger salta a 0x0 (slot NULL de la mesa dispatch).
           Solo se hace si se pide --call-gl: ejecutarlo puede colgar el system entero
           (el GL core espera una conexión que nunca llega) => ultimo paso y con timeout. */
        dumpMaps("antes de glGetString");
        if (!callGl) {
                printf("   (no se llama a glGetString: anadir --call-gl para reproducir el crash)\n");
                printf("== mocha_glprobe fin (sin crash) ==\n");
                fflush(stdout);
                return 0;
        }
        printf("   ANTES de glGetString(GL_VERSION)  (si muere aqui, es el bug de SF)\n");
        fflush(stdout);
        fflush(stderr);

        const GLubyte* version = glGetString(GL_VERSION);
        printf("   glGetString(GL_VERSION) -> %s\n", version ? (const char*)version : "NULL");
        const GLubyte* vendor = glGetString(GL_VENDOR);
        printf("   glGetString(GL_VENDOR)  -> %s\n", vendor ? (const char*)vendor : "NULL");
        const GLubyte* rend = glGetString(GL_RENDERER);
        printf("   glGetString(GL_RENDERER) -> %s\n", rend ? (const char*)rend : "NULL");
        printf("   glGetError -> 0x%04x\n", glGetError());
        fflush(stdout);
        fflush(stderr);

        printf("== mocha_glprobe fin ==\n");
        fflush(stdout);
        return 0;
}
