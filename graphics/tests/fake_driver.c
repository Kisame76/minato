/* A host library for test 4 of ADR-0030 (graphics.bind_missing), which graphics/host opens as MoltenVK and
   as ANGLE: it has one function of each table, with answers neither of them gives, so every other name of
   the tables is missing and answers "not supported". */
#include <stdint.h>

#define EXPORTED __attribute__((visibility("default")))

/* Vulkan 1.3.0 where MoltenVK says 1.4. */
EXPORTED int32_t vkEnumerateInstanceVersion(uint32_t *version) {
    *version = 1u << 22 | 3u << 12;
    return 0;
}

/* EGL_OPENVG_API where ANGLE says EGL_OPENGL_ES_API. */
EXPORTED uint32_t EGL_QueryAPI(void) {
    return 0x30a1;
}

/* GL_OUT_OF_MEMORY where ANGLE, without a context, says GL_NO_ERROR. */
EXPORTED uint32_t glGetError(void) {
    return 0x505;
}
