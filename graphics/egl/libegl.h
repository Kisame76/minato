/* libEGL's own code (ADR-0029, decisions 14 and 15; ADR-0030, decision 4): the functions written by hand,
   which reach ANGLE through the stubs minato_host_<name> of tools/frontgen's table. Guest code, built by
   tools/build-front-libs.sh against the NDK's headers. */
#ifndef MINATO_LIBEGL_H
#define MINATO_LIBEGL_H

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "minato_bind.h"

/* The host function of a name written by hand: its stub in egl_stubs.S, which takes what the name takes. */
#define MINATO_HOST(name) extern __typeof__(name) minato_host_##name MINATO_HIDDEN

/* ANGLE's names, which the NDK's headers lack (ANGLE's include/EGL/eglext_angle.h and eglext.h). */
#define MINATO_EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define MINATO_EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define MINATO_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define MINATO_EGL_FIXED_SIZE_ANGLE 0x3201

/* eglGetProcAddress's EGL names (procs.c): the function libEGL hands out for a name of its table, the one
   written by hand or else the stub; NULL for any other name. */
MINATO_HIDDEN void *minato_egl_proc(const char *name);

/* The function libGLESv2 hands out for a GLES name (its minato_gles_proc, found once with dlsym, as Android's
   libEGL opens libGLESv2.so); NULL for any other name or without libGLESv2. */
MINATO_HIDDEN void *minato_gles_function(const char *name);

/* Leaves EGL_BAD_NATIVE_WINDOW as ANGLE's error, which ANGLE gives for a window it cannot use once it has
   checked the display and the config: graphics/host hands it none for the window ID 0. */
MINATO_HIDDEN void minato_bad_native_window(EGLDisplay display, EGLConfig config);

/* eglTerminate's part (surface.c): the window surfaces of `display` let their windows go. */
MINATO_HIDDEN void minato_release_windows(EGLDisplay display);

#endif
