/* The drop until the app is told (graphics.gles_untold), with EGL on ANGLE's macOS slice and graphics-run's window:
   a window the host removes before the app's destroy callback has returned takes swaps as Android's BufferQueue
   does until then (apkrt_window_remove_untold): each succeeds without a frame and without GPU work, at most one
   every 1/60 s; once told (apkrt_window_told) a swap fails with EGL_BAD_NATIVE_WINDOW, as a gone window's does. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "test_control.h"

typedef struct ANativeWindow *(*create_window)(uint64_t, int32_t, int32_t);

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

int main(void) {
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    eglChooseConfig(display, attributes, &config, 1, &count);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const create_window create =
        (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    const uint64_t id = minato_test(MINATO_TEST_WINDOW, 0, 0, 0);
    struct ANativeWindow *window = create != NULL && id != 0 ? create(id, 64, 48) : NULL;
    const EGLSurface surface = eglCreateWindowSurface(display, config, window, NULL);
    if (count != 1 || context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) {
        printf("no window surface\n");
        return 1;
    }
    minato_test(MINATO_TEST_GATE, 1, 0, 0);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    printf("open: %s\n", eglSwapBuffers(display, surface) ? "EGL_TRUE" : "EGL_FALSE");
    glFinish();

    printf("removed until told: %lld\n", (long long)minato_test(MINATO_TEST_REMOVE, 1, 0, 0));
    minato_test(MINATO_TEST_COUNT_COMMITS, 0, 0, 0);
    int all = 1;
    const uint64_t start = now_ns();
    for (int frame = 0; frame < 4; frame++) {
        glClear(GL_COLOR_BUFFER_BIT);
        all = all && eglSwapBuffers(display, surface) == EGL_TRUE && eglGetError() == EGL_SUCCESS;
    }
    const uint64_t took = now_ns() - start;
    printf("four swaps before the app is told: EGL_TRUE %s, at its pace (at least 3/60 s) %s, commits %llu\n",
           all ? "yes" : "no", took >= 3 * 16000000ull ? "yes" : "no",
           (unsigned long long)minato_test(MINATO_TEST_COMMITS, 0, 0, 0));

    minato_test(MINATO_TEST_TOLD, id, 0, 0);
    const EGLBoolean told = eglSwapBuffers(display, surface);
    printf("after it is told: %s, %s\n", told ? "EGL_TRUE" : "EGL_FALSE",
           eglGetError() == EGL_BAD_NATIVE_WINDOW ? "EGL_BAD_NATIVE_WINDOW" : "another error");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    printf("done\n");
    return 0;
}
