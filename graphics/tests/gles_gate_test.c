/* Test 7 of ADR-0029, its first part (graphics.gles_gate): the gates of decision 15, on ANGLE's macOS slice,
   with graphics-run's window of 64 x 48 pixels (its gate open), its test control (test_control.h) playing
   the app, and the Metal hook counting the command buffers guest threads commit.
   - open: every swap presents and is counted;
   - closed: the swap flushes the frame in flight, one commit, then waits at the gate; a signal's handler
     runs meanwhile and the wait goes on; once the gate opens the swap presents;
   - gone: the swap fails at once with EGL_BAD_NATIVE_WINDOW and commits nothing;
   - background: on two other threads, a context with no surface uploads a texture, clears a framebuffer on it,
     calls glFlush and lets the context go, and returns at once (ANGLE keeps the command buffers for the foreground,
     patch 0025), and so does glFinish (patch 0027: no GL call parks its thread there); nothing is committed,
     "scheduled" says yes meanwhile, and once the process is in the foreground again the reading thread reads its
     pixel, the background's work done;
   - "scheduled" through the runtime waits for ANGLE's command buffers: not while one waits for an event of
     the host's (EGL_ANGLE_metal_shared_event_sync), once the host set the event.
   On ANGLE's Vulkan display, the ES 3.1 path (graphics.gles_gate_vulkan, ADR-0044 c)), three lines differ for its
   backend's reasons, and the last says the display has no such events: "gone" counts the commit of MoltenVK's
   acquire, which ANGLE makes at the frame's first draw (nothing is presented); a glFlush with nothing but a clear
   submits nothing there, and its upload's submit waits at the Vulkan gate, so in the background neither thread
   returns; after the background both commit. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_control.h"

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static void frame(float red) {
    glClearColor(red, 0.5f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static EGLDisplay display;
static EGLConfig config;

/* EGL_ANGLE_metal_shared_event_sync's names, which the NDK's headers lack. */
#define EGL_SYNC_METAL_SHARED_EVENT_ANGLE 0x34D8
#define EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE 0x34D9
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE 0x34DA
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE 0x34DC

/* A context of its own on a pbuffer of its own, for another thread. */
struct other {
    EGLSurface pbuffer;
    EGLContext context;
    atomic_int done;
    atomic_int foreground;  /* set once the process is in the foreground again */
    GLubyte pixel[4];
};

static void other_make(struct other *other) {
    const EGLint size[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    other->pbuffer = eglCreatePbufferSurface(display, config, size);
    other->context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
}

/* A context with no surface (SDL 2's after its window surface went, ADR-0029 decision 15's addendum): an upload into
   a texture, a clear of a framebuffer on it, glFlush, and the context let go. */
static void *flush_in_background(void *argument) {
    struct other *other = argument;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, other->context);
    static GLubyte texels[16 * 16 * 4];
    GLuint texture = 0, framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    frame(1.0f);
    glFlush();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    atomic_store(&other->done, 1);
    return NULL;
}

static void *read_in_background(void *argument) {
    struct other *other = argument;
    eglMakeCurrent(display, other->pbuffer, other->pbuffer, other->context);
    frame(1.0f);
    glFinish();
    atomic_store(&other->done, 1);
    while (!atomic_load(&other->foreground)) {
        usleep(1000);
    }
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, other->pixel);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return NULL;
}

static atomic_int handled, swapped;
static void handle(int signal) {
    (void)signal;
    atomic_store(&handled, 1);
}

/* Closed: waits for the flush's commit, signals the swapping thread, then opens the gate. */
struct closed {
    pthread_t swapping;
    uint64_t commits;
    int still_waiting;
};

static void *open_later(void *argument) {
    struct closed *closed = argument;
    const uint64_t start = now_ns();
    while (minato_test(MINATO_TEST_COMMITS, 0, 0, 0) == 0 && now_ns() - start < 2000000000u) {
        usleep(1000);
    }
    usleep(50000);
    pthread_kill(closed->swapping, SIGUSR1);
    usleep(50000);
    closed->commits = minato_test(MINATO_TEST_COMMITS, 0, 0, 0);
    closed->still_waiting = !atomic_load(&swapped);
    minato_test(MINATO_TEST_GATE, 1, 0, 0);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint count = 0;
    eglChooseConfig(display, attributes, &config, 1, &count);
    struct ANativeWindow *(*create)(uint64_t, int32_t, int32_t) =
        (struct ANativeWindow * (*)(uint64_t, int32_t, int32_t)) dlsym(dlopen("libnativewindow.so", RTLD_NOW),
                                                                       "minato_ANativeWindow_create");
    struct ANativeWindow *window = create(minato_test(MINATO_TEST_WINDOW, 0, 0, 0), 64, 48);
    const EGLSurface surface = eglCreateWindowSurface(display, config, window, NULL);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    eglMakeCurrent(display, surface, surface, context);

    int all = 1;
    for (int index = 0; index < 3; index++) {
        frame(0.25f);
        all &= eglSwapBuffers(display, surface) == EGL_TRUE;
    }
    printf("open: 3 swaps, each EGL_TRUE: %s; presents counted: %llu\n", yes(all),
           (unsigned long long)minato_test(MINATO_TEST_PRESENTS, 0, 0, 0));

    struct sigaction action = {.sa_handler = handle};
    sigaction(SIGUSR1, &action, NULL);
    struct closed closed = {pthread_self(), 0, 0};
    pthread_t opener;
    minato_test(MINATO_TEST_COUNT_COMMITS, 0, 0, 0);
    minato_test(MINATO_TEST_GATE, 0, 0, 0);
    frame(0.5f);
    pthread_create(&opener, NULL, open_later, &closed);
    const EGLBoolean waited = eglSwapBuffers(display, surface);
    atomic_store(&swapped, 1);
    pthread_join(opener, NULL);
    printf("closed: the frame flushed before the wait: %llu commit; a signal's handler ran during the wait: %s; the "
           "swap still waited 50 ms after it: %s; once the gate opened: %s; presents counted: %llu\n",
           (unsigned long long)closed.commits, yes(atomic_load(&handled)), yes(closed.still_waiting),
           waited ? "EGL_TRUE" : "EGL_FALSE", (unsigned long long)minato_test(MINATO_TEST_PRESENTS, 0, 0, 0));

    minato_test(MINATO_TEST_REMOVE, 0, 0, 0);
    minato_test(MINATO_TEST_COUNT_COMMITS, 0, 0, 0);
    frame(0.75f);
    const uint64_t start = now_ns();
    const EGLBoolean gone = eglSwapBuffers(display, surface);
    const uint64_t took = now_ns() - start;
    const EGLint error = eglGetError();
    printf("gone: %s, %s, at once: %s, commits: %llu\n", gone ? "EGL_TRUE" : "EGL_FALSE",
           error == EGL_BAD_NATIVE_WINDOW ? "EGL_BAD_NATIVE_WINDOW" : "another error", yes(took < 50000000u),
           (unsigned long long)minato_test(MINATO_TEST_COMMITS, 0, 0, 0));

    struct other flushing = {0}, reading = {0};
    other_make(&flushing);
    other_make(&reading);
    pthread_t flusher, reader;
    minato_test(MINATO_TEST_BACKGROUND, 1, 0, 0);
    minato_test(MINATO_TEST_COUNT_COMMITS, 0, 0, 0);
    pthread_create(&flusher, NULL, flush_in_background, &flushing);
    pthread_create(&reader, NULL, read_in_background, &reading);
    usleep(100000);
    const int flushed = atomic_load(&flushing.done), read = atomic_load(&reading.done);
    const uint64_t in_background = minato_test(MINATO_TEST_COMMITS, 0, 0, 0);
    const uint64_t scheduled = minato_test(MINATO_TEST_SCHEDULED, 1000, 0, 0);
    printf("background: after 100 ms the upload, glFlush and the unbind on a context with no surface returned: %s, "
           "glFinish returned: %s, commits: %llu; \"scheduled\": %llu\n",
           yes(flushed), yes(read), (unsigned long long)in_background, (unsigned long long)scheduled);
    minato_test(MINATO_TEST_BACKGROUND, 0, 0, 0);
    atomic_store(&reading.foreground, 1);
    pthread_join(flusher, NULL);
    pthread_join(reader, NULL);
    /* The pixel shows the background's work done; who committed it varies (the reading thread or ANGLE's own, patch
       0027), so the guest threads' commit count is not compared. */
    printf("foreground: both returned: %s; glReadPixels read %u %u %u %u\n",
           yes(atomic_load(&flushing.done) && atomic_load(&reading.done)), reading.pixel[0], reading.pixel[1],
           reading.pixel[2], reading.pixel[3]);

    eglMakeCurrent(display, flushing.pbuffer, flushing.pbuffer, flushing.context);
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (extensions == NULL || strstr(extensions, "EGL_ANGLE_metal_shared_event_sync") == NULL) {
        printf("\"scheduled\" with an event of the host's: the display has no EGL_ANGLE_metal_shared_event_sync\n");
        glFinish();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        ANativeWindow_release(window);
        eglTerminate(display);
        return 0;
    }
    const uint64_t event = minato_test(MINATO_TEST_EVENT, 0, 0, 0);
    const EGLAttrib gpu_wait[] = {EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE, (EGLAttrib)event,
                                  EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, 1,
                                  EGL_SYNC_CONDITION, EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE, EGL_NONE};
    const EGLSync waiting = eglCreateSync(display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, gpu_wait);
    eglWaitSync(display, waiting, 0);
    frame(0.5f);
    glFlush();
    const uint64_t before_event = minato_test(MINATO_TEST_SCHEDULED, 100, 0, 0);
    minato_test(MINATO_TEST_EVENT_SIGNAL, event, 1, 0);
    const uint64_t after_event = minato_test(MINATO_TEST_SCHEDULED, 1000, 0, 0);
    printf("\"scheduled\" while ANGLE's command buffer waited for an event of the host's: %llu, once the host set "
           "it: %llu\n",
           (unsigned long long)before_event, (unsigned long long)after_event);
    glFinish();
    eglDestroySync(display, waiting);

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    ANativeWindow_release(window);
    eglTerminate(display);
    return 0;
}
