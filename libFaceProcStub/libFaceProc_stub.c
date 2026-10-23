// Minimal libFaceProc stub to satisfy libbeautify.so DT_NEEDED
// Exports the 4 C-style functions libbeautify imports:
//   FaceInit, FaceFinalize, FaceClear, FaceAnalyzeOneFace
// All return non-zero (failure) to gracefully disable beautify/FD.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Face detection/beautification API (from libFaceProc on Tegra 17.1)
// Returns: 0 = success, non-zero = failure
#define STUB_API __attribute__((visibility("default")))

STUB_API int FaceInit(void) {
    // Stub: fail to initialize -> beauty/FD disabled
    return -1;
}

STUB_API int FaceFinalize(void) {
    return -1;
}

STUB_API int FaceClear(void) {
    return -1;
}

// Analyze a single face in the image
// Returns: 0 = success, non-zero = failure/no face
STUB_API int FaceAnalyzeOneFace(void* img __attribute__((unused)), int width __attribute__((unused)), int height __attribute__((unused)), int format __attribute__((unused)),
                       void* faces __attribute__((unused)), int max_faces __attribute__((unused)), int* num_faces) {
    if (num_faces) *num_faces = 0;
    return -1;
}

#ifdef __cplusplus
}
#endif