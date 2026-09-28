/* eglGetProcAddress's lists (libegl.h): tools/frontgen's names of libEGL's table, each with the function
   libEGL hands out for it, and libGLESv2's lookup for the GLES names. Without the EGL headers: every function
   is declared alike here, as the list names it. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "minato_bind.h"

MINATO_HIDDEN void *minato_egl_proc(const char *name);
MINATO_HIDDEN void *minato_gles_function(const char *name);

#define MINATO_PROC(name, function) extern void function(void);
#include "egl_procs.inc"
#undef MINATO_PROC

struct proc {
    const char *name;
    void (*function)(void);
};
#define MINATO_PROC(name, function) {#name, function},
static const struct proc procs[] = {
#include "egl_procs.inc"
};
#undef MINATO_PROC

static int by_name(const void *key, const void *element) {
    return strcmp(key, ((const struct proc *)element)->name);
}

void *minato_egl_proc(const char *name) {
    const struct proc *proc = bsearch(name, procs, sizeof procs / sizeof *procs, sizeof *procs, by_name);
    return proc != NULL ? (void *)proc->function : NULL;
}

static void *(*gles_proc)(const char *);
static pthread_once_t found = PTHREAD_ONCE_INIT;

static void find_gles(void) {
    void *library = dlopen("libGLESv2.so", RTLD_NOW);
    if (library != NULL) {
        gles_proc = (void *(*)(const char *))dlsym(library, "minato_gles_proc");
    }
}

void *minato_gles_function(const char *name) {
    pthread_once(&found, find_gles);
    return gles_proc != NULL ? gles_proc(name) : NULL;
}
