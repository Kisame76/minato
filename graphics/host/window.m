/* graphics/host's buffers for drawing by the CPU (ADR-0043, decision 3), which minato_graphics_set_providers gives the
   runtime (apkrt_set_window_buffers, whose contract they keep). Their lock and post run as host code on the guest
   thread in libnativewindow's ANativeWindow_lock and ANativeWindow_unlockAndPost.
   - A window has three buffers of its geometry: IOSurfaces, which the guest writes and the GPU reads without a copy.
     A lock takes, of those that are neither the last posted one nor read by the GPU, the one posted longest ago, and
     waits up to a second for one. A new geometry makes three new buffers.
   - A lock copies the last posted pixels into its buffer outside the new dirty rectangle, as libgui's Surface::lock
     does. Each buffer keeps one rectangle around what changed since it was last posted; the copy takes that rectangle
     less the dirty one. The first lock of a geometry answers the whole buffer as dirty.
   - At TRIM_MEMORY_UI_HIDDEN (apkrt_window_buffers' trim) the buffers of every window that holds no lock go, as
     Android's hidden window gives its surface's back: the next lock makes new buffers and answers all of one dirty.
   - A post draws the buffer into the window layer's next drawable with a Metal pass, BGRA8Unorm (F15), RGBX_8888's
     alpha as 1, presents it and counts it (apkrt_window_presented). The layer's nextDrawable lets MinatoMetalLayer
     record it. Its command buffer commits only in the foreground and counts for "scheduled", as libvulkan's submits
     do (vulkan.m).
   - The hide rule (decision 4, worded in b3): a posted RGBA_8888 buffer whose every pixel has alpha 0 hides the
     window's layer, unless the layer is opaque, and its post presents nothing, so that Core Animation presents a
     full-screen SurfaceView below directly. The first buffer with a visible pixel shows the layer again in the
     Core Animation transaction that presents its drawable (presentsWithTransaction), so no older frame flashes.
   - A removed window's buffers go at once, but for a locked one, which its post gives back with -ENODEV; a lock for
     it answers -ENODEV.
   ARC. Their state lives under one mutex, which no Metal or Core Animation call holds. */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <TargetConditionals.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"
#include "present_pace.h"

/* Linux's errno values, which the guest gets. */
enum { linux_enomem = 12, linux_enodev = 19, linux_einval = 22, linux_etimedout = 110 };

/* AHardwareBuffer's formats of ANativeWindow's (WINDOW_FORMAT_*). */
enum { format_rgba_8888 = 1, format_rgbx_8888 = 2, format_rgb_565 = 4 };

enum { buffer_count = 3 };

/* How long a lock waits for a buffer that the GPU has read. */
static const uint64_t buffer_wait_ns = 1000000000;

@interface MinatoCPUBuffer : NSObject {
@public
    IOSurfaceRef surface;
    id<MTLTexture> texture;
    BOOL copies;        // the texture is no view of the surface: a post copies the pixels into it (the Simulator)
    uint8_t *wide;      // RGB_565's pixels as RGBA_8888 for its copy, where Metal has no B5G6R5Unorm (the Simulator)
    int32_t behind[4];  // around what changed since this buffer was last posted
    uint64_t posted;    // the window's post count at its last post, 0 before
    BOOL reading;       // a post's command buffer reads it
}
@end

@implementation MinatoCPUBuffer
- (void)dealloc {
    if (surface != NULL) {
        CFRelease(surface);
    }
    free(wide);
}
@end

@interface MinatoCPUWindow : NSObject {
@public
    int32_t width, height, format, bytes, stride;
    NSArray<MinatoCPUBuffer *> *buffers;
    MinatoCPUBuffer *locked, *front;  // the guest's, and the last posted
    int32_t dirty[4];                 // the locked buffer's
    uint64_t posts;
}
@end

@implementation MinatoCPUWindow
@end

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t freed = PTHREAD_COND_INITIALIZER;  // a buffer's read ended, with state_lock
static NSMutableDictionary<NSNumber *, MinatoCPUWindow *> *windows;
static NSMutableSet<NSNumber *> *removed_ids;  // those the runtime said removed

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipeline;

/* Posts between their count and their commit (decision 13 of ADR-0030): each counts itself, then reads the background
   flag and backs out if it is set; the app sets the flag, then "scheduled" reads the count. */
static atomic_uint in_flight;

static NSString *const shader_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct Position { float4 position [[position]]; };\n"
     "vertex Position minato_cpu_vertex(uint index [[vertex_id]]) {\n"
     "    const float2 corner = float2((index << 1) & 2, index & 2);\n"
     "    return {float4(corner * 2 - 1, 0, 1)};\n"
     "}\n"
     "fragment float4 minato_cpu_fragment(Position in [[stage_in]],\n"
     "                                    texture2d<float, access::read> pixels [[texture(0)]],\n"
     "                                    constant uint &opaque [[buffer(0)]]) {\n"
     "    const float4 color = pixels.read(uint2(in.position.xy));\n"
     "    return opaque != 0 ? float4(color.rgb, 1) : color;\n"
     "}\n";

/* The device, the queue and the pass's pipeline, once. A triangle larger than the drawable covers it, and each of its
   pixels reads the buffer's pixel at the same place. */
static BOOL set_up(void) {
    static dispatch_once_t once;
    static BOOL ready;
    dispatch_once(&once, ^{
        device = MTLCreateSystemDefaultDevice();
        queue = [device newCommandQueue];
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:shader_source options:nil error:&error];
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"minato_cpu_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"minato_cpu_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeline = library != nil ? [device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
        ready = queue != nil && pipeline != nil;
        if (!ready) {
            fprintf(stderr, "minato graphics: no drawing by the CPU: %s\n",
                    error != nil ? error.localizedDescription.UTF8String : "no Metal device");
        }
    });
    return ready;
}

static BOOL texture_format(int32_t format, MTLPixelFormat *pixel_format, int32_t *bytes, uint32_t *surface_format) {
    switch (format) {
        case format_rgba_8888:
        case format_rgbx_8888:
            *pixel_format = MTLPixelFormatRGBA8Unorm;
            *bytes = 4;
            *surface_format = 'RGBA';
            return YES;
        case format_rgb_565:
            *pixel_format = MTLPixelFormatB5G6R5Unorm;
            *bytes = 2;
            *surface_format = 'L565';
            return YES;
        default:
            return NO;
    }
}

static MinatoCPUBuffer *new_buffer(int32_t width, int32_t height, int32_t bytes, uint32_t surface_format,
                                   MTLPixelFormat pixel_format) {
#if TARGET_OS_SIMULATOR
    // The Simulator's Metal has no B5G6R5Unorm, and a question about it ends the process: there an RGB_565 buffer's
    // texture is RGBA8Unorm, of its own, which each post fills with the buffer's pixels widened.
    const BOOL widens = pixel_format == MTLPixelFormatB5G6R5Unorm;
    if (widens) {
        pixel_format = MTLPixelFormatRGBA8Unorm;
    }
#else
    const BOOL widens = NO;
#endif
    const size_t minimum = (size_t)[device minimumLinearTextureAlignmentForPixelFormat:pixel_format];
    const size_t alignment = minimum > 64 ? minimum : 64;
    const size_t row = (IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)width * (size_t)bytes) + alignment - 1) /
                       alignment * alignment;
    NSDictionary *properties = @{
        (id)kIOSurfaceWidth: @(width),
        (id)kIOSurfaceHeight: @(height),
        (id)kIOSurfaceBytesPerElement: @(bytes),
        (id)kIOSurfaceBytesPerRow: @(row),
        (id)kIOSurfacePixelFormat: @(surface_format),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (surface == NULL) {
        return nil;
    }
    MinatoCPUBuffer *buffer = [MinatoCPUBuffer new];
    buffer->surface = surface;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                                                          width:(NSUInteger)width
                                                                                         height:(NSUInteger)height
                                                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    buffer->texture = widens ? nil : [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    if (widens) {
        buffer->wide = malloc((size_t)width * (size_t)height * 4);
        if (buffer->wide == NULL) {
            return nil;
        }
    }
    if (buffer->texture == nil) {
        // Where Metal takes no IOSurface [unverified: the Simulator], a texture of its own, filled at each post.
        buffer->texture = [device newTextureWithDescriptor:descriptor];
        buffer->copies = YES;
    }
    return buffer->texture != nil ? buffer : nil;
}

static int32_t smaller(int32_t a, int32_t b) {
    return a < b ? a : b;
}

static int32_t larger(int32_t a, int32_t b) {
    return a > b ? a : b;
}

static BOOL empty(const int32_t rect[4]) {
    return rect[0] >= rect[2] || rect[1] >= rect[3];
}

/* `rect` as the smallest rectangle around it and `other`. */
static void unite(int32_t rect[4], const int32_t other[4]) {
    if (empty(other)) {
        return;
    }
    if (empty(rect)) {
        memcpy(rect, other, 4 * sizeof rect[0]);
        return;
    }
    rect[0] = smaller(rect[0], other[0]);
    rect[1] = smaller(rect[1], other[1]);
    rect[2] = larger(rect[2], other[2]);
    rect[3] = larger(rect[3], other[3]);
}

static void copy_rows(const uint8_t *from, uint8_t *to, size_t row, int32_t bytes, int32_t left, int32_t top,
                      int32_t right, int32_t bottom) {
    for (int32_t y = top; left < right && y < bottom; y++) {
        const size_t start = (size_t)y * row + (size_t)left * (size_t)bytes;
        memcpy(to + start, from + start, (size_t)(right - left) * (size_t)bytes);
    }
}

/* Copies `area` less `hole` from one buffer to the other: the rows above the hole and below it, and beside it in its
   rows. */
static void copy_outside(const uint8_t *from, uint8_t *to, size_t row, int32_t bytes, const int32_t area[4],
                         const int32_t hole[4]) {
    if (empty(area)) {
        return;
    }
    if (empty(hole)) {
        copy_rows(from, to, row, bytes, area[0], area[1], area[2], area[3]);
        return;
    }
    const int32_t top = larger(area[1], smaller(hole[1], area[3])), bottom = smaller(area[3], larger(hole[3], area[1]));
    copy_rows(from, to, row, bytes, area[0], area[1], area[2], top);
    copy_rows(from, to, row, bytes, area[0], bottom, area[2], area[3]);
    copy_rows(from, to, row, bytes, area[0], top, smaller(area[2], larger(hole[0], area[0])), bottom);
    copy_rows(from, to, row, bytes, larger(area[0], smaller(hole[2], area[2])), top, area[2], bottom);
}

/* Of the window's buffers that are neither the last posted one nor read by the GPU, the one posted longest ago. */
static MinatoCPUBuffer *free_buffer(MinatoCPUWindow *window) {
    MinatoCPUBuffer *found = nil;
    for (MinatoCPUBuffer *buffer in window->buffers) {
        if (buffer != window->front && !buffer->reading && (found == nil || buffer->posted < found->posted)) {
            found = buffer;
        }
    }
    return found;
}

/* Waits on `freed` until `deadline` (CLOCK_UPTIME_RAW); NO once it passed. */
static BOOL wait_freed(uint64_t deadline) {
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now >= deadline) {
        return NO;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    const uint64_t left = deadline - now;
    until.tv_sec += (time_t)(left / 1000000000);
    until.tv_nsec += (long)(left % 1000000000);
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&freed, &state_lock, &until);
    return YES;
}

static int locked(void *context, uint64_t window_id, struct apkrt_window_buffer *request) {
    (void)context;
    MTLPixelFormat pixel_format;
    int32_t bytes;
    uint32_t surface_format;
    // Past 16384 a side, a Metal texture's largest, Metal would end the process at the buffer's texture (ADR-0060 e2c2
    // hardening); a device's gralloc refuses such a buffer too.
    if (request->width <= 0 || request->height <= 0 || request->width > 16384 || request->height > 16384 ||
        !texture_format(request->format, &pixel_format, &bytes, &surface_format)) {
        return -linux_einval;
    }
    if (!set_up()) {
        return -linux_enomem;
    }
    const int32_t width = request->width, height = request->height;
    const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + buffer_wait_ns;
    pthread_mutex_lock(&state_lock);
    if ([removed_ids containsObject:@(window_id)]) {
        pthread_mutex_unlock(&state_lock);
        return -linux_enodev;
    }
    MinatoCPUWindow *window = windows[@(window_id)];
    if (window == nil) {
        window = [MinatoCPUWindow new];
        windows[@(window_id)] = window;
    }
    if (window->locked != nil) {
        pthread_mutex_unlock(&state_lock);
        return -linux_einval;
    }
    if (window->buffers == nil || window->width != width || window->height != height ||
        window->format != request->format) {
        // A new geometry, or the buffers trimmed: three new buffers, once the GPU reads none of the old ones.
        BOOL in_time = YES;
        for (BOOL reading = YES; reading && in_time;) {
            reading = NO;
            for (MinatoCPUBuffer *buffer in window->buffers) {
                reading = reading || buffer->reading;
            }
            in_time = !reading || wait_freed(deadline);
        }
        NSMutableArray<MinatoCPUBuffer *> *buffers = [NSMutableArray arrayWithCapacity:buffer_count];
        for (int index = 0; in_time && index < buffer_count; index++) {
            MinatoCPUBuffer *buffer = new_buffer(width, height, bytes, surface_format, pixel_format);
            if (buffer == nil) {
                break;
            }
            const int32_t whole[4] = {0, 0, width, height};
            memcpy(buffer->behind, whole, sizeof whole);
            [buffers addObject:buffer];
        }
        if (buffers.count != buffer_count) {
            pthread_mutex_unlock(&state_lock);
            return in_time ? -linux_enomem : -linux_etimedout;
        }
        window->buffers = buffers;
        window->front = nil;
        window->width = width;
        window->height = height;
        window->format = request->format;
        window->bytes = bytes;
        window->stride = (int32_t)(IOSurfaceGetBytesPerRow(buffers[0]->surface) / (size_t)bytes);
    }
    MinatoCPUBuffer *buffer;
    while ((buffer = free_buffer(window)) == nil) {
        if (!wait_freed(deadline)) {
            pthread_mutex_unlock(&state_lock);
            return -linux_etimedout;
        }
    }
    // The dirty rectangle within the buffer; all of it before the geometry's first post, as nothing can be copied.
    MinatoCPUBuffer *const front = window->front;
    int32_t dirty[4] = {larger(request->dirty[0], 0), larger(request->dirty[1], 0), smaller(request->dirty[2], width),
                        smaller(request->dirty[3], height)};
    if (front == nil) {
        const int32_t whole[4] = {0, 0, width, height};
        memcpy(dirty, whole, sizeof whole);
    } else if (empty(dirty)) {
        memset(dirty, 0, sizeof dirty);
    }
    int32_t behind[4];
    memcpy(behind, buffer->behind, sizeof behind);
    memcpy(window->dirty, dirty, sizeof dirty);
    window->locked = buffer;
    const int32_t stride = window->stride;
    pthread_mutex_unlock(&state_lock);

    // The copy-back, outside the lock: no other post or lock writes these two buffers now, and both stay alive.
    IOSurfaceLock(buffer->surface, 0, NULL);
    uint8_t *const bits = IOSurfaceGetBaseAddress(buffer->surface);
    if (front != nil) {
        IOSurfaceLock(front->surface, kIOSurfaceLockReadOnly, NULL);
        copy_outside(IOSurfaceGetBaseAddress(front->surface), bits, IOSurfaceGetBytesPerRow(buffer->surface), bytes,
                     behind, dirty);
        IOSurfaceUnlock(front->surface, kIOSurfaceLockReadOnly, NULL);
    }
    request->stride = stride;
    request->bits = bits;
    memcpy(request->dirty, dirty, sizeof dirty);
    return 0;
}

/* Whether a pixel of the RGBA_8888 buffer has alpha above 0, visible as alpha is premultiplied: it reads each row
   whole, which vectorizes, and stops after the first row with one. */
static BOOL any_visible(const uint8_t *bits, size_t row, int32_t width, int32_t height) {
    for (int32_t y = 0; y < height; y++) {
        const uint32_t *pixels = (const uint32_t *)(const void *)(bits + (size_t)y * row);
        uint32_t seen = 0;
        for (int32_t x = 0; x < width; x++) {
            seen |= pixels[x];
        }
        if ((seen & 0xff000000u) != 0) {
            return YES;
        }
    }
    return NO;
}

static void set_hidden(CALayer *layer, BOOL hidden) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.hidden = hidden;
    [CATransaction commit];
}

/* RGB_565's pixels as RGBA_8888, each channel widened as Metal's unorm conversion does. */
static void widen_565(const uint8_t *bits, size_t row, int32_t width, int32_t height, uint8_t *wide) {
    for (int32_t y = 0; y < height; y++) {
        const uint16_t *pixels = (const uint16_t *)(const void *)(bits + (size_t)y * row);
        uint8_t *out = wide + (size_t)y * (size_t)width * 4;
        for (int32_t x = 0; x < width; x++) {
            const uint32_t value = pixels[x];
            out[4 * x] = (uint8_t)(((value >> 11) * 255 + 15) / 31);
            out[4 * x + 1] = (uint8_t)((((value >> 5) & 63) * 255 + 31) / 63);
            out[4 * x + 2] = (uint8_t)(((value & 31) * 255 + 15) / 31);
            out[4 * x + 3] = 255;
        }
    }
}

/* The buffer's read ended: it may be locked again. */
static void read_done(MinatoCPUBuffer *buffer) {
    pthread_mutex_lock(&state_lock);
    buffer->reading = NO;
    pthread_cond_broadcast(&freed);
    pthread_mutex_unlock(&state_lock);
}

/* Draws the buffer into the next drawable of the window's layer and presents it, in the foreground. YES once the
   command buffer is committed, whose end ends the buffer's read; NO without a layer or drawable, in the background,
   or for a buffer without a visible pixel on a layer that is not opaque, which it hides. */
static BOOL presented(uint64_t window_id, MinatoCPUBuffer *buffer, int32_t width, int32_t height, int32_t format,
                      BOOL visible) {
    CAMetalLayer *layer = (__bridge_transfer CAMetalLayer *)apkrt_window_layer(window_id);
    if (layer == nil) {
        return NO;
    }
    if (!visible && !layer.opaque) {
        // Hidden over nothing looks as transparent over nothing does; the post counts but presents nothing.
        if (!layer.hidden) {
            set_hidden(layer, YES);
        }
        apkrt_window_presented(window_id);
        return NO;
    }
    BOOL changed = NO;
    if (layer.device != device) {
        layer.device = device;
        changed = YES;
    }
    if (layer.pixelFormat != MTLPixelFormatBGRA8Unorm) {
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        changed = YES;
    }
    if (!CGSizeEqualToSize(layer.drawableSize, CGSizeMake(width, height))) {
        layer.drawableSize = CGSizeMake(width, height);
        changed = YES;
    }
    if (changed) {
        [CATransaction flush];
    }
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable == nil) {
        return NO;
    }
    // The window's own drawable takes no record of the front libs' presents (ADR-0041 decision 11 c)).
    minato_present_unpaced((__bridge void *)drawable);
    atomic_fetch_add(&in_flight, 1);
    if (apkrt_is_background()) {
        atomic_fetch_sub(&in_flight, 1);
        return NO;
    }
    if (buffer->copies) {
        IOSurfaceLock(buffer->surface, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *bits = IOSurfaceGetBaseAddress(buffer->surface);
        size_t row = IOSurfaceGetBytesPerRow(buffer->surface);
        if (buffer->wide != NULL) {
            widen_565(bits, row, width, height, buffer->wide);
            bits = buffer->wide;
            row = (size_t)width * 4;
        }
        [buffer->texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
                           mipmapLevel:0
                             withBytes:bits
                           bytesPerRow:row];
        IOSurfaceUnlock(buffer->surface, kIOSurfaceLockReadOnly, NULL);
    }
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    const uint32_t opaque = format == format_rgba_8888 ? 0 : 1;
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:buffer->texture atIndex:0];
    [encoder setFragmentBytes:&opaque length:sizeof opaque atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        read_done(buffer);
    }];
    if (layer.hidden) {
        // Shown again with this drawable, in the transaction that presents it.
        layer.presentsWithTransaction = YES;
        [commands commit];
        [commands waitUntilScheduled];
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.hidden = NO;
        [drawable present];
        [CATransaction commit];
        layer.presentsWithTransaction = NO;
    } else {
        [commands presentDrawable:drawable];
        [commands commit];
    }
    atomic_fetch_sub(&in_flight, 1);
    apkrt_window_presented(window_id);
    return YES;
}

static int posted(void *context, uint64_t window_id) {
    (void)context;
    pthread_mutex_lock(&state_lock);
    MinatoCPUWindow *window = windows[@(window_id)];
    MinatoCPUBuffer *const buffer = window != nil ? window->locked : nil;
    if (buffer == nil) {
        pthread_mutex_unlock(&state_lock);
        return -linux_einval;
    }
    window->locked = nil;
    const BOOL gone = [removed_ids containsObject:@(window_id)];
    if (gone) {
        [windows removeObjectForKey:@(window_id)];
    } else {
        // The others now differ from it in what it drew; it is the one the next lock copies from.
        for (MinatoCPUBuffer *other in window->buffers) {
            if (other != buffer) {
                unite(other->behind, window->dirty);
            }
        }
        memset(buffer->behind, 0, sizeof buffer->behind);
        buffer->posted = ++window->posts;
        buffer->reading = YES;
        window->front = buffer;
    }
    const int32_t width = window->width, height = window->height, format = window->format;
    pthread_mutex_unlock(&state_lock);
    // The hide rule's look, while the guest's lock still holds the buffer's memory: only RGBA_8888 has alpha.
    const BOOL visible = format != format_rgba_8888 || any_visible(IOSurfaceGetBaseAddress(buffer->surface),
                                                                   IOSurfaceGetBytesPerRow(buffer->surface), width,
                                                                   height);
    IOSurfaceUnlock(buffer->surface, 0, NULL);
    if (gone) {
        return -linux_enodev;
    }
    @autoreleasepool {
        if (!presented(window_id, buffer, width, height, format, visible)) {
            read_done(buffer);
        }
    }
    return 0;
}

/* The window is gone: its buffers go, but for a locked one, which waits for its post. */
static void removed(void *context, uint64_t window_id) {
    (void)context;
    pthread_mutex_lock(&state_lock);
    [removed_ids addObject:@(window_id)];
    MinatoCPUWindow *window = windows[@(window_id)];
    if (window != nil && window->locked == nil) {
        [windows removeObjectForKey:@(window_id)];
    } else if (window != nil) {
        window->buffers = @[window->locked];
        window->front = nil;
    }
    pthread_mutex_unlock(&state_lock);
}

/* TRIM_MEMORY_UI_HIDDEN: the buffers of each window without a lock go; a GPU read still running keeps its buffer
   until it ends. */
static void trimmed(void *context) {
    (void)context;
    pthread_mutex_lock(&state_lock);
    for (MinatoCPUWindow *window in windows.allValues) {
        if (window->locked == nil) {
            window->buffers = nil;
            window->front = nil;
        }
    }
    pthread_mutex_unlock(&state_lock);
}

int minato_graphics_window_scheduled(uint64_t deadline) {
    while (atomic_load(&in_flight) != 0) {
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
            return 0;
        }
        usleep(100);
    }
    if (queue == nil) {
        return 1;
    }
    @autoreleasepool {
        dispatch_semaphore_t scheduled = dispatch_semaphore_create(0);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        [commands addScheduledHandler:^(id<MTLCommandBuffer> unused) {
            (void)unused;
            dispatch_semaphore_signal(scheduled);
        }];
        [commands commit];
        // Metal schedules the queue's command buffers in their order, so each earlier one is scheduled too.
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        return now < deadline &&
               dispatch_semaphore_wait(scheduled, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(deadline - now))) == 0;
    }
}

static int scheduled_provider(void *context, uint64_t deadline) {
    (void)context;
    return minato_graphics_window_scheduled(deadline);
}

int minato_graphics_set_window_buffers(void) {
    static const struct apkrt_window_buffers buffers = {locked, posted, removed, NULL, trimmed};
    pthread_mutex_lock(&state_lock);
    if (windows == nil) {
        windows = [NSMutableDictionary dictionary];
        removed_ids = [NSMutableSet set];
    }
    pthread_mutex_unlock(&state_lock);
    return apkrt_set_window_buffers(&buffers) == 0 && apkrt_add_scheduled_provider(scheduled_provider, NULL) == 0
               ? 0
               : -1;
}
