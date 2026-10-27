/* The scene with GLES 3.0 (gles.c), on an EGL display and a window that the platform gives it. */
#ifndef BENCH_GLES_H
#define BENCH_GLES_H

#ifdef BENCH_ANGLE_DIRECT
#include "native/angle_egl.h"
#endif
#include <EGL/egl.h>

struct bench_gles;

/* Initializes `display`, makes a window surface of `window` with `surface_attributes` and a context of ES 3.0
   current, and the scene's objects with `meshes` meshes; `check` is the frame whose CRC it logs, at its place and
   drawn once before the first frame, -1 for none; `scale` the offscreen target's size per side in percent of the
   window's (bench.conf's scale, 100 for none). NULL when a step fails, which it logs. */
struct bench_gles *bench_gles_start(EGLDisplay display, EGLNativeWindowType window, const EGLint *surface_attributes,
                                    int meshes, int check, int scale);

/* Draws frame `frame` and swaps: EGL_SUCCESS, or eglSwapBuffers's error. */
EGLint bench_gles_frame(struct bench_gles *scene, int frame);

/* Deletes the scene's objects, its surface and context, and terminates the display. */
void bench_gles_stop(struct bench_gles *scene);

#endif
