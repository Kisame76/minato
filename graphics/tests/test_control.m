/* For the tests of ADR-0029 B1 in graphics-run: the host side of the front lib "test" (test_control.h), which
   plays the app for a guest program. Its provider, set before the program starts, puts control into slot 1;
   control runs as host code on the guest thread that asks, and hands what needs a thread of the host's own
   ("scheduled", a debug message in ANGLE) to one, which it waits for. */
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"
#include "test_control.h"
#include "test_host.h"

/* ANGLE's own names (EGL_MakeCurrent, glDebugMessageInsertKHR) with the types they have there. */
typedef unsigned (*make_current_function)(void *display, void *draw, void *read, void *context);
typedef void (*insert_function)(unsigned source, unsigned type, unsigned id, unsigned severity, int length,
                                const char *text);

struct host_work {
    uint64_t command, a, b, c;
    uint64_t result;
};

static void *on_host_thread(void *argument) {
    struct host_work *work = argument;
    if (work->command == MINATO_TEST_SCHEDULED) {
        work->result = (uint64_t)apkrt_wait_scheduled((double)work->a / 1000.0);
        return NULL;
    }
    const char *path = getenv("MINATO_ANGLE");
    void *angle = path != NULL ? dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST) : NULL;
    const make_current_function make_current = angle != NULL ? (make_current_function)dlsym(angle, "EGL_MakeCurrent")
                                                             : NULL;
    const insert_function insert = angle != NULL ? (insert_function)dlsym(angle, "glDebugMessageInsertKHR") : NULL;
    void *display = (void *)(uintptr_t)work->a, *surface = (void *)(uintptr_t)work->b;
    if (make_current != NULL && insert != NULL && make_current(display, surface, surface, (void *)(uintptr_t)work->c)) {
        /* GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, GL_DEBUG_SEVERITY_NOTIFICATION */
        insert(0x824A, 0x8268, 7, 0x826B, -1, "on a host thread");
        make_current(display, NULL, NULL, NULL);
        work->result = 1;
    }
    return NULL;
}

static uint64_t on_own_thread(uint64_t command, uint64_t a, uint64_t b, uint64_t c) {
    struct host_work work = {command, a, b, c, 0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, on_host_thread, &work) != 0) {
        return 0;
    }
    pthread_join(thread, NULL);
    return work.result;
}

static uint64_t control(uint64_t command, uint64_t a, uint64_t b, uint64_t c) {
    switch (command) {
        case MINATO_TEST_WINDOW:
            return minato_test_window;
        case MINATO_TEST_GATE:
            return (uint64_t)(int64_t)apkrt_window_set_gate(minato_test_window, a != 0);
        case MINATO_TEST_REMOVE:
            return (uint64_t)(int64_t)apkrt_window_remove(minato_test_window);
        case MINATO_TEST_PRESENTS:
            return apkrt_window_presents(minato_test_window);
        case MINATO_TEST_BACKGROUND:
            apkrt_set_background(a != 0);
            return 0;
        case MINATO_TEST_COUNT_COMMITS:
            atomic_store(&minato_test_guest_commits, 0);
            atomic_store(&minato_test_counting, 1);
            return 0;
        case MINATO_TEST_COMMITS:
            return (uint64_t)atomic_load(&minato_test_guest_commits);
        case MINATO_TEST_DRAWABLE_SIZE: {
            CAMetalLayer *layer = (__bridge_transfer CAMetalLayer *)apkrt_window_layer(minato_test_window);
            return layer != nil ? (uint64_t)layer.drawableSize.width << 32 | (uint64_t)layer.drawableSize.height : 0;
        }
        case MINATO_TEST_SCHEDULED:
        case MINATO_TEST_HOST_DEBUG:
            return on_own_thread(command, a, b, c);
        case MINATO_TEST_EVENT: {
            id<MTLSharedEvent> event = [MTLCreateSystemDefaultDevice() newSharedEvent];
            return (uint64_t)(uintptr_t)(__bridge_retained void *)event;
        }
        case MINATO_TEST_EVENT_SIGNAL:
            ((__bridge id<MTLSharedEvent>)(void *)(uintptr_t)a).signaledValue = b;
            return 0;
        case MINATO_TEST_VSYNC_GRID:
            apkrt_vsync_post((int64_t)a, (int64_t)(a + b));
            return 0;
        case MINATO_TEST_PRESENTED_AT:
            if (a != 0) {
                minato_test_watch_presents();
                return 0;
            }
            return (uint64_t)minato_test_presented_at();
        case MINATO_TEST_DROP:
            return minato_graphics_drop_present(minato_test_window, (int64_t)a) == 0;
        case MINATO_TEST_PRESENTED_NTH:
            return (uint64_t)minato_test_presented_nth(a);
        default:
            fprintf(stderr, "test control: no command %llu\n", (unsigned long long)command);
            return 0;
    }
}

static int provide(void *context, void **table, size_t length, uint64_t hash) {
    (void)context;
    if (length != MINATO_TEST_TABLE_LENGTH || hash != MINATO_TEST_TABLE_HASH) {
        return 1;
    }
    table[1] = (void *)control;
    return 0;
}

__attribute__((constructor)) static void set_test_provider(void) {
    if (apkrt_set_host_provider("test", provide, NULL) != 0) {
        fprintf(stderr, "test control: the runtime refused the provider\n");
        exit(2);
    }
}
