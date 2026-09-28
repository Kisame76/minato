/* The providers of the front libs' host tables (ADR-0030, decision 3); see minato_graphics.h.

   A provider runs when a front lib's first constructor binds its table: on that guest thread, as host
   code. It takes only the table of the lists it was built with (the same length and hash), opens the host
   library, which stays open for the process's life, and fills slot n with the library's function for the
   n-th name of tools/frontgen's list. A name the library lacks gets its function from here: it logs once
   and answers the API's "not supported", the answer tools/frontgen put next to the name. graphics/host's
   own functions then stand in front of some of MoltenVK's (vulkan.m) and of ANGLE's (angle.m). */
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "APKRuntime/apkrt.h"
#include "angle_host.h"
#include "minato_graphics.h"
#include "egl_table.h"
#include "gles_table.h"
#include "vulkan_host.h"
#include "vulkan_table.h"

/* MVK_CONFIG_LOG_LEVEL_WARNING (MoltenVK/mvk_private_api.h). */
#define MOLTENVK_LOG_LEVEL "2"

static void report(const char *driver, const char *name, const char *answer, atomic_flag *logged) {
    if (!atomic_flag_test_and_set_explicit(logged, memory_order_relaxed)) {
        fprintf(stderr, "minato graphics: %s lacks %s, which %s\n", driver, name, answer);
    }
}

/* The function of a missing name, by the kind of its result. A floating-point result answers 0.0 as a
   double, which clears the low 64 bits of v0: 0.0 for a float too. */
#define MISSING_VOID(driver, name, answer)                                    \
    static void missing_##name(void) {                                        \
        static atomic_flag logged = ATOMIC_FLAG_INIT;                         \
        report(driver, #name, "does nothing", &logged);                       \
    }
#define MISSING_INT(driver, name, answer)                                     \
    static uint64_t missing_##name(void) {                                    \
        static atomic_flag logged = ATOMIC_FLAG_INIT;                         \
        report(driver, #name, "answers " #answer, &logged);                   \
        return (uint64_t)(int64_t)(answer);                                   \
    }
#define MISSING_FLOAT(driver, name, answer)                                   \
    static double missing_##name(void) {                                      \
        static atomic_flag logged = ATOMIC_FLAG_INIT;                         \
        report(driver, #name, "answers 0.0", &logged);                        \
        return 0;                                                             \
    }

#define MINATO_NAME(name, host, result, answer) MISSING_##result("MoltenVK", name, answer)
#include "vulkan_names.inc"
#undef MINATO_NAME
#define MINATO_NAME(name, host, result, answer) MISSING_##result("ANGLE", name, answer)
#include "gles_names.inc"
#include "egl_names.inc"
#undef MINATO_NAME

/* Per lib and slot from 1 on: the host function's name, and the function for it when the library lacks it. */
#define MINATO_NAME(name, host, result, answer) host,
static const char *const vulkan_hosts[] = {
#include "vulkan_names.inc"
};
static const char *const gles_hosts[] = {
#include "gles_names.inc"
};
static const char *const egl_hosts[] = {
#include "egl_names.inc"
};
#undef MINATO_NAME
#define MINATO_NAME(name, host, result, answer) (void *)missing_##name,
static void *const vulkan_missing[] = {
#include "vulkan_names.inc"
};
static void *const gles_missing[] = {
#include "gles_names.inc"
};
static void *const egl_missing[] = {
#include "egl_names.inc"
};
#undef MINATO_NAME
_Static_assert(sizeof vulkan_hosts / sizeof *vulkan_hosts == MINATO_VULKAN_TABLE_LENGTH - 1 &&
               sizeof gles_hosts / sizeof *gles_hosts == MINATO_GLES_TABLE_LENGTH - 1 &&
               sizeof egl_hosts / sizeof *egl_hosts == MINATO_EGL_TABLE_LENGTH - 1,
               "a name per slot but the entry's");

struct lib {
    const char *name, *driver;
    size_t length;
    uint64_t hash;
    const char *const *hosts;
    void *const *missing;
    void (*wrap)(void **table, void *library);  /* graphics/host's functions in front of the library's */
    char path[1024];
};

static void vulkan_wrap(void **table, void *library) {
    (void)library;
    minato_graphics_vulkan_wrap(table);
}

static struct lib libs[] = {
    {"vulkan", "MoltenVK", MINATO_VULKAN_TABLE_LENGTH, MINATO_VULKAN_TABLE_HASH, vulkan_hosts, vulkan_missing,
     vulkan_wrap, ""},
    {"gles", "ANGLE", MINATO_GLES_TABLE_LENGTH, MINATO_GLES_TABLE_HASH, gles_hosts, gles_missing,
     minato_graphics_gles_wrap, ""},
    {"egl", "ANGLE", MINATO_EGL_TABLE_LENGTH, MINATO_EGL_TABLE_HASH, egl_hosts, egl_missing, minato_graphics_egl_wrap,
     ""},
};

static int provide(void *context, void **table, size_t length, uint64_t hash) {
    const struct lib *lib = context;
    if (length != lib->length || hash != lib->hash) {
        fprintf(stderr, "minato graphics: %s's table differs from graphics/host's, so it gets no driver\n", lib->name);
        return 1;
    }
    /* RTLD_FIRST: dlsym searches the library alone, not what it loads (macOS's desktop GL below ANGLE has
       glTextureBarrierNV, which ANGLE lacks). */
    void *library = dlopen(lib->path, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    if (library == NULL) {
        fprintf(stderr, "minato graphics: %s, so %s gets no driver\n", dlerror(), lib->name);
        return 1;
    }
    for (size_t slot = 1; slot < length; slot++) {
        void *function = dlsym(library, lib->hosts[slot - 1]);
        table[slot] = function != NULL ? function : lib->missing[slot - 1];
    }
    lib->wrap(table, library);
    return 0;
}

int minato_graphics_set_providers(const char *moltenvk, const char *angle) {
    const char *paths[] = {moltenvk, angle, angle};
    for (size_t index = 0; index < sizeof libs / sizeof *libs; index++) {
        if (paths[index] != NULL && strlen(paths[index]) >= sizeof libs[index].path) {
            return -1;
        }
    }
    /* MoltenVK logs at its info level unless told otherwise, 173 lines per instance: errors and warnings
       only, unless the environment names a level. It reads the variable when first used, after this. */
    if (moltenvk != NULL && moltenvk[0] != '\0' && setenv("MVK_CONFIG_LOG_LEVEL", MOLTENVK_LOG_LEVEL, 0) != 0) {
        return -1;
    }
    int result = 0;
    for (size_t index = 0; index < sizeof libs / sizeof *libs; index++) {
        struct lib *lib = &libs[index];
        const int some = paths[index] != NULL && paths[index][0] != '\0';
        strlcpy(lib->path, some ? paths[index] : "", sizeof lib->path);
        if (apkrt_set_host_provider(lib->name, some ? provide : NULL, lib) != 0) {
            result = -1;
        }
    }
    /* The app asks for "scheduled" at background (ADR-0029, decision 15; ADR-0030, decision 13). */
    if (moltenvk != NULL && moltenvk[0] != '\0' && apkrt_add_scheduled_provider(minato_graphics_vulkan_provider, NULL) != 0) {
        result = -1;
    }
    if (angle != NULL && angle[0] != '\0' && apkrt_add_scheduled_provider(minato_graphics_angle_provider, NULL) != 0) {
        result = -1;
    }
    return result;
}
