/* ADR-0043 b5, detail 4: one window drawn by the CPU, then by EGL, then by the CPU again, as Android lets an app
   connect another producer after a disconnect, on graphics-run's window, ID 1, of 64 x 48 pixels, with ANGLE's macOS
   slice. The window host prints each buffer shown by contents and each change of their sublayer's hidden
   (MINATO_WINDOW_WATCH=contents), and the Metal hook each drawable presented (present:4).
   1. A CPU post of opaque red: its buffer shows. The CPU stays connected after the post, as Android's Surface::lock
      connects it, so EGL's window surface fails (EGL_NO_SURFACE, EGL_BAD_ALLOC, as on Android 15) until the CPU's
      disconnect. Were it made, the test would draw and swap on it, as an app does: its drawable would hide the CPU's
      buffer (the black windows' candidate (c), ADR-0043), which the host's lines would show.
   2. EGL's window surface on the window, a clear to blue and a swap: the drawable hides the buffers' sublayer. While
      EGL is connected the CPU's lock fails (-EINVAL, as on Android 15).
   3. After eglDestroySurface and 200 ms, for ANGLE's present to reach Core Animation, a CPU post whose left half is
      transparent: a transparent drawable first, as the blue frame would show through, then the sublayer shows its
      buffer again. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { width = 64, height = 48 };

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);
typedef void (*disconnect_cpu)(ANativeWindow *window);

/* A CPU post of the whole buffer: the left half `left`, the right half `right`, RGBA. */
static void cpu_post(ANativeWindow *window, const char *step, const uint8_t left[4], const uint8_t right[4]) {
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer buffer;
    const int32_t locked = ANativeWindow_lock(window, &buffer, NULL);
    if (locked == 0) {
        for (int y = 0; y < buffer.height; y++) {
            for (int x = 0; x < buffer.width; x++) {
                memcpy((uint8_t *)buffer.bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * 4,
                       x < buffer.width / 2 ? left : right, 4);
            }
        }
    }
    printf("%s cpu: lock %d, post %d\n", step, locked, locked == 0 ? ANativeWindow_unlockAndPost(window) : -1);
}

int main(void) {
    void *library = dlopen("libnativewindow.so", RTLD_NOW);
    create_window create = (create_window)dlsym(library, "minato_ANativeWindow_create");
    disconnect_cpu disconnect = (disconnect_cpu)dlsym(library, "minato_ANativeWindow_disconnectCpu");
    ANativeWindow *window = create(1, width, height);
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255}, clear[4] = {0, 0, 0, 0};
    cpu_post(window, "1)", red, red);

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    eglChooseConfig(display, attributes, &config, 1, &count);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const EGLSurface refused = eglCreateWindowSurface(display, config, window, NULL);
    const EGLint error = eglGetError();
    printf("1) egl while the cpu is connected: %s, error 0x%x\n",
           refused != EGL_NO_SURFACE ? "a surface" : "EGL_NO_SURFACE", error);
    if (refused != EGL_NO_SURFACE) {  // an app's frame on it, green
        eglMakeCurrent(display, refused, refused, context);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display, refused);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, refused);
        usleep(200000);
    }
    if (disconnect != NULL) {
        disconnect(window);
    } else {
        printf("absent: minato_ANativeWindow_disconnectCpu\n");
    }

    const EGLSurface surface = eglCreateWindowSurface(display, config, window, NULL);
    eglMakeCurrent(display, surface, surface, context);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const EGLBoolean swapped = eglSwapBuffers(display, surface);
    ANativeWindow_Buffer buffer;
    printf("2) cpu while egl is connected: lock %d\n", ANativeWindow_lock(window, &buffer, NULL));
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    const EGLBoolean destroyed = eglDestroySurface(display, surface);
    printf("2) egl: surface %s, swap %s, destroyed %s\n", surface != EGL_NO_SURFACE ? "made" : "none",
           swapped ? "EGL_TRUE" : "EGL_FALSE", destroyed ? "EGL_TRUE" : "EGL_FALSE");
    eglDestroyContext(display, context);
    usleep(200000);

    cpu_post(window, "3)", clear, green);
    usleep(200000);  // the host shows a post's buffer on its main thread, after the post
    ANativeWindow_release(window);
    return 0;
}
