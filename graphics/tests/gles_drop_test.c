/* ADR-0041 decision 11 c) on the Mac (graphics.gles_drop): what graphics/host drops at a drawable's present, with EGL
   on ANGLE's macOS slice, graphics-run's window of 64 x 48 pixels (window_host.m, a MinatoMetalLayer), its test
   control and the Metal hook (count mode), which sees the drawables Core Animation gets. On a refresh grid of 10 ms
   from 1 s, each time below goes to Core Animation less half a period (decision 11).
   - Six swaps: at 2 s; at 2.005 s, which fails as the surface is not current (EGL_BAD_SURFACE), so graphics/host
     takes its record back; at 2.01, 2.02 and 2.03 s; then one without a time. The present asked for at 2.02 s is
     not shown (the test control's MINATO_TEST_DROP, in the place of the rule): Core Animation gets 1.995, 2.005 and
     2.025 s, then the frame without a time, so each drawable took its own present's record. */
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "test_control.h"

typedef struct ANativeWindow *(*create_window)(uint64_t, int32_t, int32_t);

static EGLDisplay display;
static EGLSurface surface;
static PFNEGLPRESENTATIONTIMEANDROIDPROC set_time;

/* A frame asked for at `time` (none for 0) and swapped. */
static void frame(EGLnsecsANDROID time, const char *what) {
    if (time != 0) {
        set_time(display, surface, time);
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    printf("%s: %s\n", what, eglSwapBuffers(display, surface) ? "EGL_TRUE" : "EGL_FALSE");
}

int main(void) {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES3_BIT, EGL_NONE};
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
    const EGLint size[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
    const EGLSurface other = eglCreatePbufferSurface(display, config, size);
    if (count != 1 || context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || other == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) {
        printf("no window surface\n");
        return 1;
    }
    set_time = (PFNEGLPRESENTATIONTIMEANDROIDPROC)eglGetProcAddress("eglPresentationTimeANDROID");
    minato_test(MINATO_TEST_VSYNC_GRID, 1000000000, 10000000, 0);
    printf("the present at 2.02 s to drop: %s\n",
           minato_test(MINATO_TEST_DROP, 2020000000, 0, 0) == 1 ? "noted" : "refused");
    minato_test(MINATO_TEST_PRESENTED_AT, 1, 0, 0);

    frame(2000000000, "at 2 s");
    set_time(display, surface, 2005000000);
    eglMakeCurrent(display, other, other, context);
    const EGLBoolean failed = eglSwapBuffers(display, surface);
    printf("at 2.005 s, the surface not current: %s, %s\n", failed ? "EGL_TRUE" : "EGL_FALSE",
           eglGetError() == EGL_BAD_SURFACE ? "EGL_BAD_SURFACE" : "another error");
    eglMakeCurrent(display, surface, surface, context);
    frame(2010000000, "at 2.01 s");
    frame(2020000000, "at 2.02 s");
    frame(2030000000, "at 2.03 s");
    frame(0, "without a time");
    glFinish();

    /* The four drawables shown, and time for a fifth that must not come. */
    for (int tries = 0; tries < 2000 && minato_test(MINATO_TEST_PRESENTED_NTH, 3, 0, 0) == 0; tries++) {
        usleep(1000);
    }
    usleep(200000);
    printf("Core Animation got:\n");
    for (uint64_t index = 0; index < 8; index++) {
        const uint64_t how = minato_test(MINATO_TEST_PRESENTED_NTH, index, 0, 0);
        if (how == 0) {
            break;
        }
        if (how == UINT64_MAX) {
            printf("  without a time\n");
        } else {
            printf("  at %llu ns\n", (unsigned long long)how);
        }
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, other);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    ANativeWindow_release(window);
    printf("done\n");
    return 0;
}
