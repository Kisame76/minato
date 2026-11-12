/* graphics.gles_thread_exit: a thread that ends with a context current lets it go, as Android's libEGL does.
   Android's libEGL keeps its thread state under a pthread key whose destructor calls eglReleaseThread
   (frameworks/native opengl/libs/EGL/egl_tls.cpp, destructTLSData), so a context still current on a thread that ends
   stops being current, and one the app destroyed meanwhile is freed then. ANGLE does the same only on Android
   (src/libGLESv2/global_state.cpp, ThreadCleanupCallback); on Apple it keeps the context current to the dead thread.
   Here a thread makes a pbuffer context current, makes textures, releases it and makes it current again, destroys the
   context while it is still current (EGL defers that until it is no longer current) and ends without
   eglMakeCurrent(NONE). Once the thread is gone and Metal is done, ANGLE's color textures must be the count before the
   thread: the context and its textures are freed. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "test_control.h"

enum { count = 3 };

static EGLDisplay display;
static EGLConfig config;

/* ANGLE's color textures once Metal is done with what went: the lowest of up to 2 s of looks, early at `most`. */
static uint64_t color_textures(uint64_t most) {
    uint64_t lowest = minato_test(MINATO_TEST_COLOR_TEXTURES, 0, 0, 0);
    for (int look = 0; look < 40 && lowest > most; look++) {
        usleep(50000);
        const uint64_t now = minato_test(MINATO_TEST_COLOR_TEXTURES, 0, 0, 0);
        lowest = now < lowest ? now : lowest;
    }
    return lowest;
}

static int destroyed_while_current;

static void *worker(void *unused) {
    (void)unused;
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    eglMakeCurrent(display, surface, surface, context);
    static const unsigned char pixel[4] = {1, 2, 3, 4};
    GLuint textures[count];
    glGenTextures(count, textures);
    for (int i = 0; i < count; i++) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    }
    glFinish();
    // A release and the same context current again first, so the thread also ends with libEGL's deferred-delete
    // queue: both thread-end destructors run, in whichever order.
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglMakeCurrent(display, surface, surface, context);
    // Destroyed while current: EGL frees it once it is current to no thread. The thread then ends as it is.
    destroyed_while_current = eglDestroyContext(display, context) == EGL_TRUE &&
                              eglDestroySurface(display, surface) == EGL_TRUE;
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint configs = 0;
    eglChooseConfig(display, attributes, &config, 1, &configs);
    if (configs < 1) {
        printf("no pbuffer config\n");
        return 1;
    }
    const uint64_t before = color_textures(0);
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_join(thread, NULL);
    const uint64_t after = color_textures(before);
    printf("a thread destroyed its current context and ended: %s; color textures %s\n",
           destroyed_while_current ? "destroy answered EGL_TRUE" : "destroy FAILED",
           after <= before ? "back to the count before it" : "STILL HELD");
    printf("gles thread exit: %s\n", destroyed_while_current && after <= before
                                         ? "the context went with its thread, as on Android"
                                         : "BROKEN: a context destroyed while current outlived its thread");
    eglTerminate(display);
    return destroyed_while_current && after <= before ? 0 : 1;
}
