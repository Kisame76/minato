/* libGLESv2's part of the device trace (MINATO_EGL_TRACE=1, graphics/egl/trace.c): GL calls made while no context is
   current on the calling thread, which ANGLE answers by doing nothing (glGenTextures then writes no name). Six
   functions an engine calls every frame or every load look first: glClear, glViewport, glGenTextures, glTexImage2D,
   glBindFramebuffer and glBindTexture. A line (I/libEGL) at the first such call and then at most once a second, with
   the count and the first function's name. Without the switch they only check a flag. */
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libgles.h"

MINATO_HOST(glClear);
MINATO_HOST(glViewport);
MINATO_HOST(glGenTextures);
MINATO_HOST(glTexImage2D);
MINATO_HOST(glBindFramebuffer);
MINATO_HOST(glBindTexture);

static atomic_int state;  // 0 not read yet, 1 off, 2 on
static void *(*current_context)(void);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t last_line_ns;
static unsigned count;
static const char *first_name;
static int first_thread;

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static int trace_on(void) {
    int now = atomic_load_explicit(&state, memory_order_relaxed);
    if (now == 0) {
        const char *trace = getenv("MINATO_EGL_TRACE");
        now = trace != NULL && strcmp(trace, "1") == 0 ? 2 : 1;
        if (now == 2) {
            void *egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
            current_context = egl != NULL ? (void *(*)(void))dlsym(egl, "eglGetCurrentContext") : NULL;
            now = current_context != NULL ? 2 : 1;
        }
        atomic_store_explicit(&state, now, memory_order_relaxed);
    }
    return now == 2;
}

static void no_context(const char *name) {
    if (current_context() != NULL) {
        return;
    }
    const uint64_t now = now_ns();
    pthread_mutex_lock(&lock);
    if (count++ == 0) {
        first_name = name;
        first_thread = (int)gettid();
    }
    const int write = last_line_ns == 0 || now - last_line_ns >= 1000000000u;
    const unsigned calls = count;
    const char *first = first_name;
    const int thread = first_thread;
    if (write) {
        last_line_ns = now;
        count = 0;
    }
    pthread_mutex_unlock(&lock);
    if (write) {
        __android_log_print(ANDROID_LOG_INFO, "libEGL",
                            "trace: GL calls with no context current: %u since the last line, the first %s on "
                            "thread %d", calls, first, thread);
    }
}

#define CHECK(name)                                    \
    do {                                               \
        if (__builtin_expect(trace_on(), 0)) {         \
            no_context(name);                          \
        }                                              \
    } while (0)

void glClear(GLbitfield mask) {
    CHECK("glClear");
    minato_host_glClear(mask);
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    CHECK("glViewport");
    minato_host_glViewport(x, y, width, height);
}

void glGenTextures(GLsizei n, GLuint *textures) {
    CHECK("glGenTextures");
    minato_host_glGenTextures(n, textures);
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const void *pixels) {
    CHECK("glTexImage2D");
    minato_host_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    CHECK("glBindFramebuffer");
    minato_host_glBindFramebuffer(target, framebuffer);
}

void glBindTexture(GLenum target, GLuint texture) {
    CHECK("glBindTexture");
    minato_host_glBindTexture(target, texture);
}
