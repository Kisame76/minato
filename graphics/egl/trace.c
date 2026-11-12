/* libEGL's trace for a device run (MINATO_EGL_TRACE=1 in the guest's environment; off otherwise, and then nothing of
   it runs but one check of a flag): a line in the log (I/libEGL) for each window surface made or destroyed, each
   context made or destroyed, each eglMakeCurrent, eglReleaseThread and eglTerminate, with the thread, the handles,
   the host window where there is one, the result and the error; the first three swaps of each surface; and once a
   second, from a thread of its own, the live contexts with the thread each is current on, and the swaps since the
   last such line. It shows what an app does with EGL where nothing fails (a return after which an app makes no swap
   and no context is current on its thread). */
#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libegl.h"

enum { kContexts = 64, kSurfaces = 64, kFirstSwaps = 3 };

struct context_entry {
    EGLContext context;
    pid_t current_on;  // the thread it is current on, 0 for none
};

struct surface_entry {
    EGLSurface surface;
    unsigned swaps;
    unsigned dropped;
};

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int on;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct context_entry contexts[kContexts];
static struct surface_entry surfaces[kSurfaces];
static unsigned swaps_since, failed_since, dropped_since;
static __thread EGLContext thread_context;

static void *report(void *unused);

static void start(void) {
    const char *trace = getenv("MINATO_EGL_TRACE");
    on = trace != NULL && strcmp(trace, "1") == 0;
    if (on) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, report, NULL) == 0) {
            pthread_detach(thread);
        }
        __android_log_write(ANDROID_LOG_INFO, "libEGL", "trace: on (MINATO_EGL_TRACE=1)");
    }
}

int minato_trace_on(void) {
    pthread_once(&once, start);
    return on;
}

void minato_trace(const char *format, ...) {
    char line[320];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    __android_log_print(ANDROID_LOG_INFO, "libEGL", "trace: thread %d: %s", (int)gettid(), line);
}

static struct context_entry *entry_of(EGLContext context, int add) {
    struct context_entry *free_entry = NULL;
    for (int index = 0; index < kContexts; index++) {
        if (contexts[index].context == context) {
            return &contexts[index];
        }
        if (free_entry == NULL && contexts[index].context == EGL_NO_CONTEXT) {
            free_entry = &contexts[index];
        }
    }
    if (add && free_entry != NULL) {
        free_entry->context = context;
        free_entry->current_on = 0;
        return free_entry;
    }
    return NULL;
}

void minato_trace_context_made(EGLContext context) {
    pthread_mutex_lock(&lock);
    entry_of(context, 1);
    pthread_mutex_unlock(&lock);
}

void minato_trace_context_destroyed(EGLContext context) {
    pthread_mutex_lock(&lock);
    struct context_entry *entry = entry_of(context, 0);
    if (entry != NULL && entry->current_on == 0) {
        entry->context = EGL_NO_CONTEXT;  // one current somewhere lives on until released, as EGL keeps it
    }
    pthread_mutex_unlock(&lock);
}

void minato_trace_current(EGLContext context) {
    pthread_mutex_lock(&lock);
    struct context_entry *before = thread_context != EGL_NO_CONTEXT ? entry_of(thread_context, 0) : NULL;
    if (before != NULL && before->current_on == gettid()) {
        before->current_on = 0;
    }
    struct context_entry *now = context != EGL_NO_CONTEXT ? entry_of(context, 1) : NULL;
    if (now != NULL) {
        now->current_on = gettid();
    }
    thread_context = context;
    pthread_mutex_unlock(&lock);
}

void minato_trace_swap(EGLSurface surface, uint64_t window, EGLBoolean result, EGLint error) {
    unsigned swaps = 0;
    pthread_mutex_lock(&lock);
    swaps_since++;
    failed_since += result ? 0 : 1;
    struct surface_entry *entry = NULL;
    for (int index = 0; index < kSurfaces && entry == NULL; index++) {
        if (surfaces[index].surface == surface) {
            entry = &surfaces[index];
        }
    }
    for (int index = 0; index < kSurfaces && entry == NULL; index++) {
        if (surfaces[index].surface == EGL_NO_SURFACE) {
            entry = &surfaces[index];
            entry->surface = surface;
            entry->swaps = 0;
            entry->dropped = 0;
        }
    }
    if (entry != NULL) {
        swaps = ++entry->swaps;
    }
    pthread_mutex_unlock(&lock);
    if (swaps != 0 && swaps <= kFirstSwaps) {
        minato_trace("eglSwapBuffers(surface %p, window %llu) #%u: %s, error 0x%x", surface,
                     (unsigned long long)window, swaps, result ? "EGL_TRUE" : "EGL_FALSE", error);
    }
}

/* A swap on a gone window dropped by the experiment's switch (surface.c): the first per surface, then counts. */
void minato_trace_dropped(EGLSurface surface, uint64_t window) {
    unsigned swaps = 0;
    pthread_mutex_lock(&lock);
    dropped_since++;
    struct surface_entry *entry = NULL;
    for (int index = 0; index < kSurfaces && entry == NULL; index++) {
        if (surfaces[index].surface == surface) {
            entry = &surfaces[index];
        }
    }
    if (entry != NULL) {
        swaps = ++entry->dropped;
    }
    pthread_mutex_unlock(&lock);
    if (swaps == 1) {
        minato_trace("eglSwapBuffers(surface %p, window %llu): window gone, swap dropped", surface,
                     (unsigned long long)window);
    }
}

void minato_trace_surface_destroyed(EGLSurface surface) {
    pthread_mutex_lock(&lock);
    for (int index = 0; index < kSurfaces; index++) {
        if (surfaces[index].surface == surface) {
            surfaces[index].surface = EGL_NO_SURFACE;
        }
    }
    pthread_mutex_unlock(&lock);
}

static void *report(void *unused) {
    (void)unused;
    for (;;) {
        sleep(1);
        char line[640];
        size_t length = 0;
        int live = 0;
        pthread_mutex_lock(&lock);
        for (int index = 0; index < kContexts; index++) {
            if (contexts[index].context == EGL_NO_CONTEXT) {
                continue;
            }
            live++;
            if (length < sizeof line - 64) {
                const int written =
                    contexts[index].current_on != 0
                        ? snprintf(line + length, sizeof line - length, "; %p current on thread %d",
                                   contexts[index].context, (int)contexts[index].current_on)
                        : snprintf(line + length, sizeof line - length, "; %p not current", contexts[index].context);
                length += written > 0 ? (size_t)written : 0;
            }
        }
        const unsigned swaps = swaps_since, failed = failed_since, dropped = dropped_since;
        swaps_since = 0;
        failed_since = 0;
        dropped_since = 0;
        pthread_mutex_unlock(&lock);
        line[length < sizeof line ? length : sizeof line - 1] = '\0';
        __android_log_print(ANDROID_LOG_INFO, "libEGL",
                            "trace: %d contexts%s; swaps in the last second %u, %u failed, %u dropped", live, line,
                            swaps, failed, dropped);
    }
    return NULL;
}
