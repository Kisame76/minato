/* ADR-0078 decision 4 on the Mac (graphics.present_limit): graphics/host holds an app's presents to the frame rate
   limit, with EGL on ANGLE's macOS slice and graphics-run's window of 64 x 48 pixels (its test control sets the
   limit as App does at a session's start). An app that swaps in a loop with a swap interval of 0, unpaced, gets more
   than 100 swaps a second without a limit, at most the limit's 30 or 60 a second with one (and at least 20 or 45, with
   room for a loaded Mac); an app that paces itself at 30 a second keeps its 30 under a limit of 30; without a limit
   again it runs unpaced once more. Each case counts the swaps of one second by the monotonic clock. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "test_control.h"

typedef struct ANativeWindow *(*create_window)(uint64_t, int32_t, int32_t);

static EGLDisplay display;
static EGLSurface surface;

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

/* The swaps of one second, each frame cleared and swapped; with `paced` above 0 the app waits for its own grid of
   `paced` frames a second before each frame. */
static int swaps_in_a_second(int paced) {
    const uint64_t start = now_ns(), end = start + 1000000000u;
    int swaps = 0;
    for (uint64_t next = start; now_ns() < end;) {
        if (paced > 0) {
            next += 1000000000u / (uint64_t)paced;
            const uint64_t at = now_ns();
            if (next > at) {
                const struct timespec wait = {(time_t)((next - at) / 1000000000u), (long)((next - at) % 1000000000u)};
                nanosleep(&wait, NULL);
            }
        }
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        swaps += eglSwapBuffers(display, surface) == EGL_TRUE;
    }
    return swaps;
}

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

static void limit(int rate) {
    printf("the limit %d: %s\n", rate, minato_test(MINATO_TEST_FRAME_RATE_LIMIT, (uint64_t)rate, 0, 0) == 1
                                           ? "set" : "refused");
}

int main(void) {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
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
    surface = eglCreateWindowSurface(display, config, window, NULL);
    if (count != 1 || context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) {
        printf("no window surface\n");
        return 1;
    }
    eglSwapInterval(display, 0);
    swaps_in_a_second(0);  // warm up: the first frames compile and allocate
    int swaps = swaps_in_a_second(0);
    printf("unpaced without a limit: more than 100 a second: %s\n", yes(swaps > 100));
    limit(30);
    swaps = swaps_in_a_second(0);
    printf("unpaced at the limit 30: 20 to 30 a second: %s\n", yes(swaps >= 20 && swaps <= 31));
    swaps = swaps_in_a_second(30);
    printf("paced at 30 a second, at the limit 30: 20 to 30 a second: %s\n", yes(swaps >= 20 && swaps <= 31));
    limit(60);
    swaps = swaps_in_a_second(0);
    printf("unpaced at the limit 60: 45 to 60 a second: %s\n", yes(swaps >= 45 && swaps <= 61));
    limit(0);
    swaps = swaps_in_a_second(0);
    printf("unpaced without a limit again: more than 100 a second: %s\n", yes(swaps > 100));
    printf("a limit of -1: %s\n", minato_test(MINATO_TEST_FRAME_RATE_LIMIT, (uint64_t)-1, 0, 0) == 1
                                       ? "set" : "refused");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    ANativeWindow_release(window);
    printf("done\n");
    return 0;
}
