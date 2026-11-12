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

/* Leaves EGL_BAD_ALLOC as ANGLE's error, Android's answer for a window surface whose window takes no producer (its
   native_window_api_connect fails: another API, the CPU or a decoder, is connected), once ANGLE has checked the
   display: ANGLE gives it for eglCreateStreamKHR, as its Vulkan and Metal displays have no streams. */
MINATO_HIDDEN void minato_bad_alloc(EGLDisplay display);

/* Deletes made with no context current after the thread released its context (deferred.c): eglMakeCurrent's part
   after a successful call (`before` was current on the thread, `context` is now), eglReleaseThread's, and
   eglDestroyContext's and eglTerminate's (EGL_NO_CONTEXT: every context). For libGLES, in version APKRT_PRIVATE:
   minato_egl_defer_delete keeps `count` names of a delete (kind 0 textures, 1 buffers, 2 framebuffers, 3
   renderbuffers) for the context the thread released and answers 1, or answers 0 when the caller deletes them. */
MINATO_HIDDEN void minato_deferred_current(EGLContext before, EGLContext context);
MINATO_HIDDEN void minato_deferred_release_thread(void);
MINATO_HIDDEN void minato_deferred_destroyed(EGLContext context);
/* The context this thread released with eglMakeCurrent and has made none current since, else EGL_NO_CONTEXT
   (deferred.c), for display.c's backup share. */
MINATO_HIDDEN EGLContext minato_deferred_released(void);
/* Whether libEGL made one of `a` and `b` to share the other's objects in a backup window (display.c). */
MINATO_HIDDEN int minato_contexts_share(EGLContext a, EGLContext b);
int minato_egl_defer_delete(int kind, int count, const unsigned *names);

/* eglTerminate's part (surface.c): the window surfaces of `display` let their windows go. */
MINATO_HIDDEN void minato_release_windows(EGLDisplay display);

/* Window surfaces on a queue window, a SurfaceTexture's (queue.c, ADR-0058 c)): whether `window` is one; its surface,
   a pbuffer; the swap, 1 when `surface` is one of them with the swap's result in `result`, else 0; eglDestroySurface's
   and eglTerminate's parts; and eglPresentationTimeANDROID's, 1 when `surface` is one of them. */
struct ANativeWindow;
MINATO_HIDDEN int minato_queue_window(struct ANativeWindow *window);
/* A window surface on a window, a host or a queue window, as its producer (minato_ANativeWindow_connectProducer,
   ADR-0060 e2c3): 0, or an error while another API is connected (the CPU since its first lock, or a decoder's claim);
   each 0 has one disconnect, when the surface goes. */
MINATO_HIDDEN int minato_window_connect(struct ANativeWindow *window);
MINATO_HIDDEN void minato_window_disconnect(struct ANativeWindow *window);
MINATO_HIDDEN EGLSurface minato_queue_surface(EGLDisplay display, EGLConfig config, struct ANativeWindow *window);
MINATO_HIDDEN int minato_queue_swap(EGLDisplay display, EGLSurface surface, EGLBoolean *result);
MINATO_HIDDEN void minato_queue_destroyed(EGLDisplay display, EGLSurface surface);
MINATO_HIDDEN void minato_queue_terminated(EGLDisplay display);
MINATO_HIDDEN int minato_queue_presentation_time(EGLDisplay display, EGLSurface surface, int64_t time);

/* Whether `surface` is a window surface of surface.c's on a host window (not a queue window's). */
MINATO_HIDDEN int minato_window_surface(EGLDisplay display, EGLSurface surface);

/* EGL_ANDROID_get_frame_timestamps (timestamps.c): eglPresentationTimeANDROID's time for the next frame; a swap's
   fence, made on the swapping thread before ANGLE's swap while the surface's history is on (EGL_NO_SYNC_KHR
   otherwise), and its record after the swap; a destroyed surface's history, or all of a display's for
   EGL_NO_SURFACE. */
MINATO_HIDDEN void minato_timestamps_presentation(EGLDisplay display, EGLSurface surface, int64_t time);
MINATO_HIDDEN EGLSyncKHR minato_timestamps_before_swap(EGLDisplay display, EGLSurface surface);
MINATO_HIDDEN void minato_timestamps_swapped(EGLDisplay display, EGLSurface surface, EGLSyncKHR sync,
                                             EGLBoolean swapped);
MINATO_HIDDEN void minato_timestamps_destroyed(EGLDisplay display, EGLSurface surface);

/* The thread's EGL error for a diagnostic line, left for the app's eglGetError, which still answers it
   (timestamps.c). */
MINATO_HIDDEN EGLint minato_peek_error(void);

/* The trace of a device run (trace.c, MINATO_EGL_TRACE=1): whether it is on, a line, and its bookkeeping of
   contexts, the context current on the calling thread, swaps and destroyed surfaces. An error passed in is one read
   once with minato_peek_error (a second read would lose it for the app). */
MINATO_HIDDEN int minato_trace_on(void);
MINATO_HIDDEN void minato_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
MINATO_HIDDEN void minato_trace_context_made(EGLContext context);
MINATO_HIDDEN void minato_trace_context_destroyed(EGLContext context);
MINATO_HIDDEN void minato_trace_current(EGLContext context);
MINATO_HIDDEN void minato_trace_swap(EGLSurface surface, uint64_t window, EGLBoolean result, EGLint error);
MINATO_HIDDEN void minato_trace_dropped(EGLSurface surface, uint64_t window);
MINATO_HIDDEN void minato_trace_surface_destroyed(EGLSurface surface);

/* The host window's ID of a window surface of libEGL's, 0 for any other surface (surface.c). */
MINATO_HIDDEN uint64_t minato_window_surface_id(EGLSurface surface);

/* Whether host window `window`'s gate is open now, without a wait: 0 once it is gone, removed or closed (the
   background), 1 while open or for an answer it cannot give (surface.c). */
MINATO_HIDDEN int minato_window_open(uint64_t window);

#endif
