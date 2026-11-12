/* What the GL calls that want a result of earlier GPU work answer while the app is in the background, against
   Android 15, whose GPU keeps running there: each returns within its time with its result. Minato's commits wait for
   the foreground there (iOS refuses them), kept in ANGLE's FIFO (patch 0025), and no call waits for them (patch
   0027): glFinish, glReadPixels, a sync's client wait without an end, glMapBufferRange for reading, a query's result
   and 40 frames' flushes (beyond the FIFO's 32 command buffers) each return at once with GL_NO_ERROR. Their values
   are Android's only once the GPU ran: in Minato's background a readback has the pixels from before the frames kept
   for the foreground, so each line says that it returned, its error and its answer's kind, never a value. Back in
   the foreground the kept work runs, and the pixel the background cleared is there. As a shell program on the
   emulator (no test control) the same calls run with the GPU at work: Android's lines. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "test_control.h"

#pragma weak minato_test

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

/* Into the background (1) or back (0) on Minato; nothing on Android. */
static void background(int on) {
    if (minato_test != NULL) {
        minato_test(MINATO_TEST_BACKGROUND, on, 0, 0);
    }
}

static const char *in_time(uint64_t start) {
    return now_ns() - start < 1000000000u ? "yes" : "no";
}

static void clear(float red) {
    glClearColor(red, 0.5f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* The calls, in the foreground or the background; each line: whether it returned in time, its error, its kind. */
static void calls(GLuint query) {
    clear(1.0f);
    uint64_t start = now_ns();
    glFinish();
    printf("  glFinish returned in time: %s, error 0x%x\n", in_time(start), glGetError());

    clear(0.5f);
    GLubyte pixel[4] = {0, 0, 0, 0};
    start = now_ns();
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("  glReadPixels returned in time: %s, error 0x%x\n", in_time(start), glGetError());

    clear(0.25f);
    const GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    start = now_ns();
    const GLenum waited = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    printf("  glClientWaitSync without an end returned in time: %s, %s, error 0x%x\n", in_time(start),
           waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED ? "signaled"
           : waited == GL_TIMEOUT_EXPIRED ? "GL_TIMEOUT_EXPIRED" : waited == GL_WAIT_FAILED ? "GL_WAIT_FAILED" : "0",
           glGetError());
    glDeleteSync(sync);

    static const GLubyte more[64] = {5, 6, 7, 8};
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof more, more);
    clear(0.75f);
    start = now_ns();
    const void *mapped = glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof more, GL_MAP_READ_BIT);
    printf("  glMapBufferRange for reading returned in time: %s, %s, error 0x%x\n", in_time(start),
           mapped != NULL ? "a pointer" : "null", glGetError());
    glUnmapBuffer(GL_ARRAY_BUFFER);

    glBeginQuery(GL_ANY_SAMPLES_PASSED, query);
    clear(0.125f);
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    GLuint result = 7;
    start = now_ns();
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &result);
    printf("  glGetQueryObjectuiv(GL_QUERY_RESULT) returned in time: %s, %s, error 0x%x\n", in_time(start),
           result <= 1 ? "0 or 1" : "another value", glGetError());

    start = now_ns();
    for (int frame = 0; frame < 40; frame++) {
        clear((float)frame / 40.0f);
        glFlush();
    }
    clear(1.0f);
    glFlush();
    printf("  40 frames' flushes returned in time: %s, error 0x%x\n", in_time(start), glGetError());

}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    const EGLBoolean initialized = eglInitialize(display, &major, &minor);
    const EGLint wanted[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                             EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    eglChooseConfig(display, wanted, &config, 1, &configs);
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const EGLSurface pbuffer = eglCreatePbufferSurface(display, config, size);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const EGLBoolean current = eglMakeCurrent(display, pbuffer, pbuffer, context);
    GLuint buffer = 0, query = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    static const GLubyte data[64] = {1, 2, 3, 4};
    glBufferData(GL_ARRAY_BUFFER, sizeof data, data, GL_STATIC_DRAW);
    glGenQueries(1, &query);
    glFinish();
    printf("ready: initialized: %s, a config: %s, current: %s, %s\n", initialized ? "yes" : "no",
           configs > 0 ? "yes" : "no", current ? "yes" : "no",
           glGetError() == GL_NO_ERROR ? "GL_NO_ERROR" : "an error");

    printf("foreground:\n");
    calls(query);
    background(1);
    printf("background:\n");
    calls(query);
    background(0);
    GLubyte pixel[4] = {0, 0, 0, 0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("back in the foreground: the background's last clear reads %u %u %u %u, error 0x%x\n", pixel[0],
           pixel[1], pixel[2], pixel[3], glGetError());

    glDeleteQueries(1, &query);
    glDeleteBuffers(1, &buffer);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, pbuffer);
    eglTerminate(display);
    printf("done\n");
    return 0;
}
