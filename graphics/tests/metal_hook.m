/* For test 6 of ADR-0030 (graphics.vulkan_moltenvk and graphics.vulkan_queue), in graphics-run only: with
   MINATO_METAL_HOOK set, it hooks the classes of Metal's command queues and command buffers before the
   program starts and prints to stderr each command buffer made, each MTLEvent wait and signal encoded and
   each commit, with whether a guest thread did it. So the tests see where and when MoltenVK commits, which
   its settings and libvulkan decide.
   With MINATO_METAL_HOOK=scheduled it also checks graphics/host's "scheduled" (decision 13): once a guest
   thread commits a command buffer that waits for an event at SCHEDULED_VALUE, a thread of the hook's own
   asks graphics/host for "scheduled" with 100 ms to go, sets the event to that value, asks again with 1 s
   to go, prints both answers and whether Metal scheduled the guest's command buffer before graphics/host's
   first one, then sets the event one higher, which the program waits for.
   With MINATO_METAL_HOOK=present it prints none of that. It reads back the textures of the first four
   drawables presented, before they go to the screen, and prints their size, pixel format and CRC-32; and
   while window_host.m asks for it (test_host.h), it counts the command buffers guest threads commit. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"
#include "test_host.h"

/* The value graphics/tests/vulkan_queue_test.c waits at. */
#define SCHEDULED_VALUE 1000

static IMP commit, made, made_unretained, made_with_descriptor, signaled, waited, presented;
static int noting, scheduled_mode;
static atomic_int drawables;          /* presented so far */
static id<MTLCommandQueue> readback;  /* the hook's own, for reading drawables back */
static id<MTLCommandBuffer> waiting;  /* the guest's command buffer that waits at SCHEDULED_VALUE */
static id<MTLSharedEvent> waiting_event;
static __thread int asking;           /* on the hook's thread that asks for "scheduled" */
static int marked;                    /* whether graphics/host's first command buffer is marked */
static atomic_int scheduled_count, waiting_order, host_order;  /* the order Metal scheduled them in, from 1 */

static void note(const char *what) {
    if (noting) {
        fprintf(stderr, "metal: %s on %s\n", what, apkrt_is_guest_thread() ? "a guest thread" : "another thread");
    }
}

static uint32_t crc32_of(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < length; index++) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* Copies a drawable's texture into memory the CPU reads, on the hook's own queue, and prints what it holds. */
static void read_back(id<CAMetalDrawable> drawable, int number) {
    id<MTLTexture> texture = drawable.texture;
    const NSUInteger width = texture.width, height = texture.height, row = width * 4;
    if (readback == nil) {
        readback = [texture.device newCommandQueue];
    }
    id<MTLBuffer> buffer = [texture.device newBufferWithLength:row * height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [readback commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1) toBuffer:buffer destinationOffset:0
      destinationBytesPerRow:row destinationBytesPerImage:row * height];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    const char *format = texture.pixelFormat == MTLPixelFormatBGRA8Unorm        ? "BGRA8Unorm"
                         : texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ? "BGRA8Unorm_sRGB"
                                                                                : "another format";
    fprintf(stderr, "metal: drawable %d: %lu x %lu, %s, CRC-32 %08x\n", number, (unsigned long)width,
            (unsigned long)height, format, crc32_of(buffer.contents, row * height));
}

/* MoltenVK presents a drawable from the scheduled handler of the command buffer that waits for the image's
   semaphores, so what drew it has run. */
static void hooked_present(id self, SEL selector) {
    const int number = atomic_fetch_add(&drawables, 1) + 1;
    if (number <= 4) {
        read_back(self, number);
    }
    ((void (*)(id, SEL))presented)(self, selector);
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
    if (atomic_load(&minato_test_counting) && apkrt_is_guest_thread()) {
        atomic_fetch_add(&minato_test_guest_commits, 1);
    }
    if (atomic_load(&minato_test_asking) && !apkrt_is_guest_thread()) {
        atomic_fetch_add(&minato_test_asker_commits, 1);
    }
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
    noting = strcmp(mode, "present") != 0;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        Class queues = [queue class], buffers = [buffer class];
        if (!noting) {
            CAMetalLayer *layer = [CAMetalLayer layer];
            layer.device = device;
            layer.drawableSize = CGSizeMake(1, 1);
            presented = hook([[layer nextDrawable] class], @selector(present), (IMP)hooked_present);
        }
        commit = hook(buffers, @selector(commit), (IMP)hooked_commit);
        signaled = hook(buffers, @selector(encodeSignalEvent:value:), (IMP)hooked_signaled);
        waited = hook(buffers, @selector(encodeWaitForEvent:value:), (IMP)hooked_waited);
        made = hook(queues, @selector(commandBuffer), (IMP)hooked_made);
        made_unretained = hook(queues, @selector(commandBufferWithUnretainedReferences), (IMP)hooked_made_unretained);
        made_with_descriptor =
            hook(queues, @selector(commandBufferWithDescriptor:), (IMP)hooked_made_with_descriptor);
    }
}
