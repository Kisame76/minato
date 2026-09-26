/* Test 4 of ADR-0030 (bind-test): the constructors of <lib>_bind.c bind the front libs' tables through the
   runtime's syscall "bind host table" (4101), with the providers of graphics/host that graphics-run sets
   from MINATO_MOLTENVK and MINATO_ANGLE. The program prints what each binding gave, any slot of the tables
   that does not show it, and what a few calls through the stubs answer, each round twice, since
   graphics/host logs a missing function only once. Then it binds each table again with a wrong hash and
   Vulkan's with a slot less, which the provider refuses, and checks and calls again. CTest runs it three
   times, each against its own expected output (decision 3):
   - graphics.bind with ANGLE and MoltenVK: the calls reach them, and ANGLE's missing glGetPathLengthNV
     answers "not supported";
   - graphics.bind_missing with fake_driver.c, which has one function per lib: every other name answers
     "not supported";
   - graphics.bind_no_driver without providers: every call answers "no driver".
   As in test 2, the three libs' tables and stubs are linked into one program. */
#include <stdint.h>
#include <stdio.h>

#include "minato_bind.h"
#include "egl_table.h"
#include "gles_table.h"
#include "vulkan_table.h"

extern void *minato_vulkan_table[MINATO_VULKAN_TABLE_LENGTH] MINATO_HIDDEN;
extern void *minato_gles_table[MINATO_GLES_TABLE_LENGTH] MINATO_HIDDEN;
extern void *minato_egl_table[MINATO_EGL_TABLE_LENGTH] MINATO_HIDDEN;
extern const int64_t minato_vulkan_no_driver[MINATO_VULKAN_TABLE_LENGTH] MINATO_HIDDEN;
extern const int64_t minato_gles_no_driver[MINATO_GLES_TABLE_LENGTH] MINATO_HIDDEN;
extern const int64_t minato_egl_no_driver[MINATO_EGL_TABLE_LENGTH] MINATO_HIDDEN;
extern int minato_vulkan_binding MINATO_HIDDEN, minato_gles_binding MINATO_HIDDEN, minato_egl_binding MINATO_HIDDEN;

/* The stubs called here, with the NDK's C types written as their sizes. The functions that libvulkan and
   libGLESv2 write by hand reach their host functions through minato_host_<name>. */
int32_t minato_host_vkEnumerateInstanceVersion(uint32_t *version);
int32_t minato_host_vkEnumerateInstanceLayerProperties(uint32_t *count, void *properties);
const unsigned char *minato_host_glGetString(uint32_t name);
uint32_t eglQueryAPI(void);
int32_t eglGetError(void);
uint32_t glGetError(void);
void glFlush(void);
void glColorMask(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);
void glBlitFramebuffer(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t to_x0, int32_t to_y0, int32_t to_x1,
                       int32_t to_y1, uint32_t mask, uint32_t filter);
void glTextureBarrierNV(void);
float glGetPathLengthNV(uint32_t path, int32_t start, int32_t count);

/* glGetPathLengthNV(0, 0, 0) with 2.5 in v0 before the call, so an answer that leaves v0 alone shows. */
static float path_length(void) {
    float result;
    __asm__ volatile("fmov s0, #2.5\n\t"
                     "mov w0, wzr\n\t"
                     "mov w1, wzr\n\t"
                     "mov w2, wzr\n\t"
                     "bl glGetPathLengthNV\n\t"
                     "fmov %s0, s0"
                     : "=w"(result)
                     :
                     : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
                       "x15", "x16", "x17", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17",
                       "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30",
                       "v31", "cc", "memory");
    return result;
}

static void calls(void) {
    for (int round = 0; round < 2; round++) {
        uint32_t version = 0, layers = 7;
        const int32_t enumerated = minato_host_vkEnumerateInstanceVersion(&version);
        const int32_t listed = minato_host_vkEnumerateInstanceLayerProperties(&layers, NULL);
        printf("  vkEnumerateInstanceVersion %d (%u.%u), vkEnumerateInstanceLayerProperties %d (count %u)\n",
               enumerated, version >> 22, version >> 12 & 0x3ffu, listed, layers);
        const uint32_t api = eglQueryAPI();
        printf("  eglQueryAPI 0x%x, eglGetError 0x%x\n", api, eglGetError());
        glFlush();
        glColorMask(1, 1, 1, 1);                                     /* an extend stub */
        glBlitFramebuffer(0, 0, 1, 1, 0, 0, 1, 1, 0x4000, 0x2600);  /* repack: GL_COLOR_BUFFER_BIT, GL_NEAREST */
        glTextureBarrierNV();  /* not in ANGLE, only in macOS's desktop GL below it */
        const uint32_t error = glGetError();
        const unsigned char *version_string = minato_host_glGetString(0x1F02);  /* GL_VERSION */
        printf("  glGetError 0x%x, glGetString %s, glGetPathLengthNV %g\n", error,
               version_string ? "a string" : "NULL", path_length());
    }
}

static void binding(const char *lib, int result) {
    if (result == 0) {
        printf("%s: bound\n", lib);
    } else {
        printf("%s: no driver (%d)\n", lib, result);
    }
}

/* Whether a table shows what its binding gave: once bound, every slot filled and slot 0 not
   minato_no_driver; else slot 0 minato_no_driver and every other slot its answer without a driver. Prints
   only what is wrong. */
static void slots(const char *lib, int result, void *const *table, size_t length, const int64_t *no_driver) {
    size_t wrong = 0;
    for (size_t slot = 0; slot < length; slot++) {
        void *const expected = slot == 0 ? (void *)minato_no_driver : (void *)(intptr_t)no_driver[slot];
        wrong += result == 0 ? table[slot] == NULL || (slot == 0 && table[slot] == expected) : table[slot] != expected;
    }
    if (wrong != 0) {
        printf("%s: %zu of %zu slots wrong\n", lib, wrong, length);
    }
}

static void tables(int vulkan, int gles, int egl) {
    slots("vulkan", vulkan, minato_vulkan_table, MINATO_VULKAN_TABLE_LENGTH, minato_vulkan_no_driver);
    slots("gles", gles, minato_gles_table, MINATO_GLES_TABLE_LENGTH, minato_gles_no_driver);
    slots("egl", egl, minato_egl_table, MINATO_EGL_TABLE_LENGTH, minato_egl_no_driver);
}

int main(void) {
    binding("vulkan", minato_vulkan_binding);
    binding("gles", minato_gles_binding);
    binding("egl", minato_egl_binding);
    tables(minato_vulkan_binding, minato_gles_binding, minato_egl_binding);
    calls();
    const int vulkan = minato_bind_host_table("vulkan", minato_vulkan_table, MINATO_VULKAN_TABLE_LENGTH,
                                              MINATO_VULKAN_TABLE_HASH ^ 1, minato_vulkan_no_driver);
    const int gles = minato_bind_host_table("gles", minato_gles_table, MINATO_GLES_TABLE_LENGTH,
                                            MINATO_GLES_TABLE_HASH ^ 1, minato_gles_no_driver);
    const int egl = minato_bind_host_table("egl", minato_egl_table, MINATO_EGL_TABLE_LENGTH,
                                           MINATO_EGL_TABLE_HASH ^ 1, minato_egl_no_driver);
    const int shorter = minato_bind_host_table("vulkan", minato_vulkan_table, MINATO_VULKAN_TABLE_LENGTH - 1,
                                               MINATO_VULKAN_TABLE_HASH, minato_vulkan_no_driver);
    printf("bound again with a wrong hash: vulkan %d, gles %d, egl %d; with a slot less: vulkan %d\n", vulkan, gles,
           egl, shorter);
    tables(shorter, gles, egl);
    calls();
    return 0;
}
