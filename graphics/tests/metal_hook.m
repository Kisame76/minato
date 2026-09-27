/* For test 6 of ADR-0030 (graphics.vulkan_moltenvk), in graphics-run only: with MINATO_METAL_HOOK set, it
   hooks the classes of Metal's command queues and command buffers before the program starts and prints to
   stderr each command buffer made, each MTLEvent signal encoded and each commit, with whether a guest
   thread did it. So the test sees where and when MoltenVK commits, which its settings decide. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

#include "APKRuntime/apkrt.h"

static IMP commit, made, made_unretained, made_with_descriptor, signaled;

static void note(const char *what) {
    fprintf(stderr, "metal: %s on %s\n", what, apkrt_is_guest_thread() ? "a guest thread" : "another thread");
}

static void hooked_commit(id self, SEL selector) {
    note("a commit");
    ((void (*)(id, SEL))commit)(self, selector);
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

static IMP hook(Class class, SEL selector, IMP replacement) {
    Method method = class_getInstanceMethod(class, selector);
    if (method == NULL) {
        fprintf(stderr, "metal hook: %s has no %s\n", class_getName(class), sel_getName(selector));
        exit(2);
    }
    return method_setImplementation(method, replacement);
}

__attribute__((constructor)) static void hook_metal(void) {
    if (getenv("MINATO_METAL_HOOK") == NULL) {
        return;
    }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        Class queues = [queue class], buffers = [buffer class];
        commit = hook(buffers, @selector(commit), (IMP)hooked_commit);
        signaled = hook(buffers, @selector(encodeSignalEvent:value:), (IMP)hooked_signaled);
        made = hook(queues, @selector(commandBuffer), (IMP)hooked_made);
        made_unretained = hook(queues, @selector(commandBufferWithUnretainedReferences), (IMP)hooked_made_unretained);
        made_with_descriptor =
            hook(queues, @selector(commandBufferWithDescriptor:), (IMP)hooked_made_with_descriptor);
    }
}
