/* For test 6 of ADR-0030 (graphics.vulkan_moltenvk and graphics.vulkan_queue), in graphics-run only: with
   MINATO_METAL_HOOK set, it hooks the classes of Metal's command queues and command buffers before the
   program starts and prints to stderr each command buffer made, each MTLEvent wait and signal encoded and
   each commit, with whether a guest thread did it. So the tests see where and when MoltenVK commits, which
   its settings and libvulkan decide.
   With MINATO_METAL_HOOK=scheduled it also checks graphics/host's "scheduled" (decision 13): once a guest
   thread commits a command buffer that waits for an event at SCHEDULED_VALUE, a thread of the hook's own
   asks graphics/host for "scheduled" with 100 ms to go, sets the event to that value, asks again with 1 s
   to go, prints both answers and whether Metal scheduled the guest's command buffer before graphics/host's
   first one, then sets the event one higher, which the program waits for. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"

/* The value graphics/tests/vulkan_queue_test.c waits at. */
#define SCHEDULED_VALUE 1000

static IMP commit, made, made_unretained, made_with_descriptor, signaled, waited;
static int scheduled_mode;
static id<MTLCommandBuffer> waiting;  /* the guest's command buffer that waits at SCHEDULED_VALUE */
static id<MTLSharedEvent> waiting_event;
static __thread int asking;           /* on the hook's thread that asks for "scheduled" */
static int marked;                    /* whether graphics/host's first command buffer is marked */
static atomic_int scheduled_count, waiting_order, host_order;  /* the order Metal scheduled them in, from 1 */

static void note(const char *what) {
    fprintf(stderr, "metal: %s on %s\n", what, apkrt_is_guest_thread() ? "a guest thread" : "another thread");
}

static void *ask_scheduled(void *unused) {
    (void)unused;
    asking = 1;
    const int before = minato_graphics_vulkan_scheduled(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 100000000u);
    waiting_event.signaledValue = SCHEDULED_VALUE;
    const int after = minato_graphics_vulkan_scheduled(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000u);
    const int guest = atomic_load(&waiting_order), host = atomic_load(&host_order);
    fprintf(stderr,
            "metal: \"scheduled\" while the guest's command buffer waited: %d, after its event: %d; the guest's "
            "command buffer scheduled before graphics/host's first: %s\n",
            before, after, guest != 0 && host != 0 && guest < host ? "yes" : "no");
    waiting_event.signaledValue = SCHEDULED_VALUE + 1;
    return NULL;
}

/* Records when Metal schedules the command buffer into *order. */
static void mark(id<MTLCommandBuffer> buffer, atomic_int *order) {
    [buffer addScheduledHandler:^(id<MTLCommandBuffer> scheduled) {
        (void)scheduled;
        atomic_store(order, atomic_fetch_add(&scheduled_count, 1) + 1);
    }];
}

static void hooked_commit(id self, SEL selector) {
    note("a commit");
    if (scheduled_mode && self == waiting) {
        mark(self, &waiting_order);
    } else if (scheduled_mode && asking && !marked) {
        marked = 1;
        mark(self, &host_order);
    }
    ((void (*)(id, SEL))commit)(self, selector);
    if (scheduled_mode && self == waiting) {
        pthread_t thread;
        pthread_create(&thread, NULL, ask_scheduled, NULL);
        pthread_detach(thread);
    }
}

static id hooked_made(id self, SEL selector) {
    note("a command buffer made");
    return ((id (*)(id, SEL))made)(self, selector);
}

static id hooked_made_unretained(id self, SEL selector) {
    note("a command buffer made");
    return ((id (*)(id, SEL))made_unretained)(self, selector);
}

static id hooked_made_with_descriptor(id self, SEL selector, id descriptor) {
    note("a command buffer made");
    return ((id (*)(id, SEL, id))made_with_descriptor)(self, selector, descriptor);
}

static void hooked_signaled(id self, SEL selector, id event, uint64_t value) {
    note("an event signal encoded");
    ((void (*)(id, SEL, id, uint64_t))signaled)(self, selector, event, value);
}

static void hooked_waited(id self, SEL selector, id event, uint64_t value) {
    note("an event wait encoded");
    if (scheduled_mode && value == SCHEDULED_VALUE && waiting == nil && apkrt_is_guest_thread()) {
        waiting = self;
        waiting_event = event;
    }
    ((void (*)(id, SEL, id, uint64_t))waited)(self, selector, event, value);
}

static IMP hook(Class class, SEL selector, IMP replacement) {
    Method method = class_getInstanceMethod(class, selector);
    if (method == NULL) {
        fprintf(stderr, "metal hook: %s has no %s\n", class_getName(class), sel_getName(selector));
        exit(2);
    }
    return method_setImplementation(method, replacement);
}

__attribute__((constructor)) static void hook_metal(void) {
    const char *mode = getenv("MINATO_METAL_HOOK");
    if (mode == NULL) {
        return;
    }
    scheduled_mode = strcmp(mode, "scheduled") == 0;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        Class queues = [queue class], buffers = [buffer class];
        commit = hook(buffers, @selector(commit), (IMP)hooked_commit);
        signaled = hook(buffers, @selector(encodeSignalEvent:value:), (IMP)hooked_signaled);
        waited = hook(buffers, @selector(encodeWaitForEvent:value:), (IMP)hooked_waited);
        made = hook(queues, @selector(commandBuffer), (IMP)hooked_made);
        made_unretained = hook(queues, @selector(commandBufferWithUnretainedReferences), (IMP)hooked_made_unretained);
        made_with_descriptor =
            hook(queues, @selector(commandBufferWithDescriptor:), (IMP)hooked_made_with_descriptor);
    }
}
