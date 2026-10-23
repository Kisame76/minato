/* libEGL's own code (ADR-0029, decisions 14 and 15; ADR-0030, decision 4): the functions written by hand,
   which reach ANGLE through the stubs minato_host_<name> of tools/frontgen's table. Guest code, built by
   tools/build-front-libs.sh against the NDK's headers. */
#ifndef MINATO_LIBEGL_H
#define MINATO_LIBEGL_H

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "metal_display.h"
#include "minato_bind.h"

/* The host function of a name written by hand: its stub in egl_stubs.S, which takes what the name takes. */
#define MINATO_HOST(name) extern __typeof__(name) minato_host_##name MINATO_HIDDEN

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

/* Window surfaces on a queue window, a SurfaceTexture's (queue.c, ADR-0058 c)): whether `window` is one; its surface,
   a pbuffer; the swap, 1 when `surface` is one of them with the swap's result in `result`, else 0; eglDestroySurface's
   and eglTerminate's parts; and eglPresentationTimeANDROID's, 1 when `surface` is one of them. */
struct ANativeWindow;
MINATO_HIDDEN int minato_queue_window(struct ANativeWindow *window);
/* A window surface on a host window as its producer (minato_ANativeWindow_connectProducer, ADR-0060 e2c3): 0, or
   -EBUSY while a decoder claims it; each 0 has one disconnect, when the surface goes. */
MINATO_HIDDEN int minato_window_connect(struct ANativeWindow *window);
MINATO_HIDDEN void minato_window_disconnect(struct ANativeWindow *window);
MINATO_HIDDEN EGLSurface minato_queue_surface(EGLDisplay display, EGLConfig config, struct ANativeWindow *window);
MINATO_HIDDEN int minato_queue_swap(EGLDisplay display, EGLSurface surface, EGLBoolean *result);
MINATO_HIDDEN void minato_queue_destroyed(EGLDisplay display, EGLSurface surface);
MINATO_HIDDEN void minato_queue_terminated(EGLDisplay display);
MINATO_HIDDEN int minato_queue_presentation_time(EGLDisplay display, EGLSurface surface, int64_t time);

#endif
