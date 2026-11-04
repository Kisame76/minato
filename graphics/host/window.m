/* graphics/host's buffers for drawing by the CPU (ADR-0043, decision 3), which minato_graphics_set_providers gives the
   runtime (apkrt_set_window_buffers, whose contract they keep). Their lock and post run as host code on the guest
   thread in libnativewindow's ANativeWindow_lock and ANativeWindow_unlockAndPost.
   - A window has three buffers of its geometry: IOSurfaces, which the guest writes and Core Animation shows without a
     copy, as Android's window has its BufferQueue's three and nothing more (spike S5). A lock takes, of those that
     are neither the last posted one nor in use (IOSurfaceIsInUse: Core Animation still shows it; in the Simulator,
     the GPU reads it), the one posted longest ago, and waits up to a second for one, as Android's dequeueBuffer
     blocks while no buffer is free; Core Animation tells no end of a use, so the wait looks every 0.5 ms. A new
     geometry makes three new buffers.
   - Back-pressure (ADR-0043 b5, detail 3): Core Animation lets go at once a buffer replaced before it was composited,
     so nothing would hold a post that runs ahead of the display. As Android's dequeueBuffer blocks while no buffer
     is free, which SurfaceFlinger frees once a refresh, a lock waits until the display link has ticked since the
     window's last post (apkrt_vsync_grid): at most one post a refresh. Not in the background, before the first
     tick, nor past two periods after the post, for a display link that stopped.
   - A lock copies the last posted pixels into its buffer outside the new dirty rectangle, as libgui's Surface::lock
     does. Each buffer keeps one rectangle around what changed since it was last posted; the copy takes that rectangle
     less the dirty one. The first lock of a geometry answers the whole buffer as dirty.
   - At TRIM_MEMORY_UI_HIDDEN (apkrt_window_buffers' trim) the buffers of every window that holds no lock go, as
     Android's hidden window gives its surface's back: the next lock makes new buffers and answers all of one dirty.
   - A post shows the buffer as the contents of the window layer's lowest sublayer, "minato.window.buffers", in one
     Core Animation transaction without actions, and counts it (apkrt_window_presented); the window's layer takes no
     drawable; a recording of the layer takes each post as a tick-timed record (detail 5, present_pace.h). The
     first drawable another producer presents on the window's layer hides the sublayer (layer.m), and a
     post after one presents a transparent drawable first, so that its frame shows no more under transparent pixels
     (detail 4: a CAMetalLayer's own contents show its drawables, which our contents there would displace for good
     [measured, S5]). The sublayer is opaque for RGBX_8888 and RGB_565 and black below them and below any buffer of an
     opaque layer, as Core Animation takes RGBX's fourth byte as alpha even on an opaque layer [measured, S5]: on
     black that alpha changes nothing. The transaction commits only in the foreground and counts for "scheduled", as
     libvulkan's submits do (vulkan.m).
   - At TRIM_MEMORY_UI_HIDDEN the sublayer lets its buffer go too.
   - Step f's check (minato_graphics_set_window_crc, MINATO_WINDOW_CRC): a line per post with the CRC-32 of the
     pixels shown, a table's CRC over a full screen's buffer in some 30 ms, so for checks only.
   - In the Simulator, whose Metal has no B5G6R5Unorm and whose Core Animation shows no 'L565' IOSurface [measured,
     S5], a post draws the buffer into the window layer's next drawable with a Metal pass instead, BGRA8Unorm (F15),
     RGBX_8888's alpha as 1, and presents it; the layer's nextDrawable lets MinatoMetalLayer record it.
   - The hide rule (decision 4, worded in b3): a posted RGBA_8888 buffer whose every pixel has alpha 0 hides the
     window's layer, unless the layer is opaque, and its post presents nothing, so that Core Animation presents a
     full-screen SurfaceView below directly. The first buffer with a visible pixel shows the layer again in the
     Core Animation transaction that shows it (in the Simulator, that presents its drawable: presentsWithTransaction),
     so no older frame flashes.
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

/* Whether posts show the buffers as a sublayer's contents; the Simulator draws them into a drawable, and so does a
   Debug build after minato_graphics_set_window_by_drawables(1) or with MINATO_WINDOW_BY_DRAWABLES=1, for step f's
   comparison of the footprint. Decided at the first lock. */
static BOOL by_contents = !TARGET_OS_SIMULATOR;
#ifndef NDEBUG
static BOOL by_drawables_asked;
#endif

/* How often a lock that waits looks whether Core Animation still uses a buffer. */
static const uint64_t in_use_poll_ns = 500000;

/* How long a lock waits for a buffer that the GPU has read. */
static const uint64_t buffer_wait_ns = 1000000000;

/* How long a window the hide rule hid posts nothing before it keeps only its front buffer (drop_spares). */
static const uint64_t spare_ns = 1000000000;

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
    uint64_t posted_ns;               // the last post's time, CLOCK_UPTIME_RAW; 0 before
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
#ifndef NDEBUG
        const char *drawables = getenv("MINATO_WINDOW_BY_DRAWABLES");
        if (by_drawables_asked || (drawables != NULL && strcmp(drawables, "1") == 0)) {
            by_contents = NO;
        }
#endif
        device = MTLCreateSystemDefaultDevice();
        queue = [device newCommandQueue];
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:shader_source options:nil error:&error];
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"minato_cpu_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"minato_cpu_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (!by_contents) {
            pipeline = library != nil ? [device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
        }
        ready = queue != nil && (by_contents || pipeline != nil);
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
    if (by_contents) {
        return buffer;
    }
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

/* Of the window's buffers that are neither the last posted one nor in use, the one posted longest ago. */
static MinatoCPUBuffer *free_buffer(MinatoCPUWindow *window) {
    MinatoCPUBuffer *found = nil;
    for (MinatoCPUBuffer *buffer in window->buffers) {
        const BOOL busy = buffer->reading || (by_contents && IOSurfaceIsInUse(buffer->surface));
        if (buffer != window->front && !busy && (found == nil || buffer->posted < found->posted)) {
            found = buffer;
        }
    }
    return found;
}

/* Waits on `freed` until `deadline` (CLOCK_UPTIME_RAW), by contents at most in_use_poll_ns; NO once it passed. */
static BOOL wait_freed(uint64_t deadline) {
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now >= deadline) {
        return NO;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    const uint64_t left = by_contents && deadline - now > in_use_poll_ns ? in_use_poll_ns : deadline - now;
    until.tv_sec += (time_t)(left / 1000000000);
    until.tv_nsec += (long)(left % 1000000000);
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&freed, &state_lock, &until);
    return YES;
}

/* Whether the window's next lock may take a buffer: the display link has ticked since its last post (the header). */
static BOOL ticked_since_post(const MinatoCPUWindow *window) {
    int64_t tick = 0, period = 0;
    if (window->posted_ns == 0 || apkrt_is_background() || apkrt_vsync_grid(&tick, &period) != 0 || period <= 0) {
        return YES;
    }
    return tick > (int64_t)window->posted_ns ||
           clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= window->posted_ns + 2 * (uint64_t)period;
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
    for (MinatoCPUWindow *last = windows[@(window_id)]; by_contents && last != nil && !ticked_since_post(last);
         last = windows[@(window_id)]) {
        if (!wait_freed(deadline)) {
            pthread_mutex_unlock(&state_lock);
            return -linux_etimedout;
        }
    }
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
    if (window->buffers.count < buffer_count && free_buffer(window) == nil) {
        // One of the spares a hidden window let go (drop_spares) again, which the copy-back fills.
        MinatoCPUBuffer *spare = new_buffer(width, height, bytes, surface_format, pixel_format);
        if (spare == nil) {
            pthread_mutex_unlock(&state_lock);
            return -linux_enomem;
        }
        const int32_t whole[4] = {0, 0, width, height};
        memcpy(spare->behind, whole, sizeof whole);
        window->buffers = [window->buffers arrayByAddingObject:spare];
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

/* Step f's lines (minato_graphics.h's minato_graphics_set_window_crc), under crc_lock; NULL while off. */
static pthread_mutex_t crc_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *crc_file;
static BOOL crc_asked;  // the setter or the environment decided
static uint32_t crc_table[256];

int minato_graphics_set_window_by_drawables(int drawables) {
#ifndef NDEBUG
    by_drawables_asked = drawables != 0;
    return 0;
#else
    (void)drawables;
    return -1;
#endif
}

int minato_graphics_set_window_crc(const char *path) {
    FILE *file = path == NULL ? NULL : strcmp(path, "-") == 0 ? stderr : fopen(path, "a");
    if (path != NULL && file == NULL) {
        return -1;
    }
    pthread_mutex_lock(&crc_lock);
    if (crc_file != NULL && crc_file != stderr) {
        fclose(crc_file);
    }
    crc_file = file;
    crc_asked = YES;
    pthread_mutex_unlock(&crc_lock);
    return 0;
}

static uint32_t crc_bytes(uint32_t crc, const uint8_t *bytes, size_t length) {
    for (size_t index = 0; index < length; index++) {
        crc = crc_table[(crc ^ bytes[index]) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

/* The post's line, while asked for: the CRC-32 of the buffer's pixels as BGRA, RGBX's alpha as 255 and RGB_565
   widened as Metal's B5G6R5Unorm reads it (widen_565's), or "hidden". */
static void write_crc(uint64_t window_id, uint64_t post, MinatoCPUBuffer *buffer, int32_t width, int32_t height,
                      int32_t format, BOOL black, BOOL hidden) {
    pthread_mutex_lock(&crc_lock);
    if (!crc_asked) {
        const char *path = getenv("MINATO_WINDOW_CRC");
        crc_file = path == NULL ? NULL : strcmp(path, "-") == 0 ? stderr : fopen(path, "a");
        crc_asked = YES;
    }
    if (crc_file == NULL) {
        pthread_mutex_unlock(&crc_lock);
        return;
    }
    if (hidden) {
        fprintf(crc_file, "minato window %llu: post %llu hidden\n", (unsigned long long)window_id,
                (unsigned long long)post);
        fflush(crc_file);
        pthread_mutex_unlock(&crc_lock);
        return;
    }
    if (crc_table[1] == 0) {
        for (uint32_t index = 0; index < 256; index++) {
            uint32_t value = index;
            for (int bit = 0; bit < 8; bit++) {
                value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
            }
            crc_table[index] = value;
        }
    }
    IOSurfaceLock(buffer->surface, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *bits = IOSurfaceGetBaseAddress(buffer->surface);
    const size_t row = IOSurfaceGetBytesPerRow(buffer->surface);
    uint8_t *line = malloc((size_t)width * 4);
    uint32_t crc = 0xffffffffu;
    for (int32_t y = 0; line != NULL && y < height; y++) {
        const uint8_t *source = bits + (size_t)y * row;
        if (format == format_rgb_565) {
            widen_565(source, row, width, 1, line);
        } else {
            for (int32_t x = 0; x < width; x++) {
                line[4 * x] = source[4 * x + 2];
                line[4 * x + 1] = source[4 * x + 1];
                line[4 * x + 2] = source[4 * x];
                line[4 * x + 3] = format == format_rgbx_8888 ? 255 : source[4 * x + 3];
            }
        }
        if (format == format_rgb_565) {
            // widen_565's RGBA as the BGRA the pass would draw.
            for (int32_t x = 0; x < width; x++) {
                const uint8_t red = line[4 * x];
                line[4 * x] = line[4 * x + 2];
                line[4 * x + 2] = red;
            }
        }
        crc = crc_bytes(crc, line, (size_t)width * 4);
    }
    free(line);
    IOSurfaceUnlock(buffer->surface, kIOSurfaceLockReadOnly, NULL);
    static const char *const names[] = {"", "RGBA_8888", "RGBX_8888", "", "RGB_565"};
    fprintf(crc_file, "minato window %llu: post %llu %dx%d %s%s CRC-32 %08x\n", (unsigned long long)window_id,
            (unsigned long long)post, width, height, names[format], black ? " on black" : "", ~crc);
    fflush(crc_file);
    pthread_mutex_unlock(&crc_lock);
}

/* The window layer's sublayer that shows its buffers, made at the first post, lowest among its sublayers. */
static NSString *const buffers_layer_name = @"minato.window.buffers";

/* The window layer's buffers' sublayer, nil before its first post. */
static CALayer *found_buffers_layer(CALayer *layer) {
    for (CALayer *sublayer in layer.sublayers) {
        if ([sublayer.name isEqualToString:buffers_layer_name]) {
            return sublayer;
        }
    }
    return nil;
}

static CALayer *buffers_layer(CALayer *layer) {
    CALayer *found = found_buffers_layer(layer);
    if (found != nil) {
        return found;
    }
    CALayer *made = [CALayer layer];
    made.name = buffers_layer_name;
    [layer insertSublayer:made atIndex:0];
    return made;
}

/* A transparent drawable on the window's layer, presented at once and without a record, so that the frame of another
   producer's drawables shows no more under the buffers' transparent pixels. */
static void clear_drawables(CAMetalLayer *layer) {
    if (layer.device == nil) {
        return;
    }
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable == nil) {
        return;
    }
    minato_present_unpaced((__bridge void *)drawable);
    id<MTLCommandQueue> clearing = layer.device == device ? queue : [layer.device newCommandQueue];
    id<MTLCommandBuffer> commands = [clearing commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    [commands commit];
    [commands waitUntilScheduled];
    [drawable present];
}

/* Shows the buffer as the window layer's sublayer's contents, in the foreground, and counts the post; hides the layer
   for a buffer without a visible pixel on a layer that is not opaque. NO always: no GPU read follows, and Core
   Animation's use is IOSurfaceIsInUse's. */
static BOOL shown(uint64_t window_id, MinatoCPUBuffer *buffer, int32_t format, BOOL visible) {
    CALayer *layer = (__bridge_transfer CALayer *)apkrt_window_layer(window_id);
    if (layer == nil) {
        return NO;
    }
    if (!visible && !layer.opaque) {
        // Hidden over nothing looks as transparent over nothing does; the post counts but shows nothing, and its
        // record keeps a recording's record k the window's post k.
        if (!layer.hidden) {
            set_hidden(layer, YES);
        }
        minato_present_record_post((__bridge void *)layer, (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
        write_crc(window_id, buffer->posted, buffer, 0, 0, format, NO, YES);
        apkrt_window_presented(window_id);
        return NO;
    }
    atomic_fetch_add(&in_flight, 1);
    if (apkrt_is_background()) {
        atomic_fetch_sub(&in_flight, 1);
        return NO;
    }
    static CGColorRef black;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        black = CGColorCreateSRGB(0, 0, 0, 1);
    });
    if (minato_present_buffers_shown((__bridge void *)layer) && [layer isKindOfClass:CAMetalLayer.class]) {
        clear_drawables((CAMetalLayer *)layer);
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.hidden = NO;
    CALayer *buffers = buffers_layer(layer);
    buffers.hidden = NO;
    buffers.frame = layer.bounds;
    buffers.opaque = format != format_rgba_8888;
    buffers.backgroundColor = format != format_rgba_8888 || layer.opaque ? black : NULL;
    buffers.contents = (__bridge id)buffer->surface;
    [CATransaction commit];
    minato_present_record_post((__bridge void *)layer, (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    write_crc(window_id, buffer->posted, buffer, (int32_t)IOSurfaceGetWidth(buffer->surface),
              (int32_t)IOSurfaceGetHeight(buffer->surface), format, format != format_rgba_8888 || layer.opaque, NO);
    atomic_fetch_sub(&in_flight, 1);
    apkrt_window_presented(window_id);
    return NO;
}

/* Draws the buffer into the next drawable of the window's layer and presents it, in the foreground (the Simulator).
   YES once the command buffer is committed, whose end ends the buffer's read; NO without a layer or drawable, in the
   background, or for a buffer without a visible pixel on a layer that is not opaque, which it hides. */
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
    write_crc(window_id, buffer->posted, buffer, width, height, format, NO, NO);
    atomic_fetch_sub(&in_flight, 1);
    apkrt_window_presented(window_id);
    return YES;
}

/* The window's post `post` hid its layer (the hide rule) and it posted nothing since: it keeps only that front buffer,
   and its next lock makes another (locked()). A transparent window over a SurfaceView, a game's window, holds one
   full-screen buffer instead of three; Android's queue keeps its three, a named departure for memory. */
static void drop_spares(uint64_t window_id, uint64_t post) {
    @autoreleasepool {
        CALayer *layer = (__bridge_transfer CALayer *)apkrt_window_layer(window_id);
        if (layer == nil || !layer.hidden) {
            return;
        }
        pthread_mutex_lock(&state_lock);
        MinatoCPUWindow *window = windows[@(window_id)];
        if (window != nil && window->posts == post && window->locked == nil && window->front != nil) {
            NSMutableArray<MinatoCPUBuffer *> *kept = [NSMutableArray arrayWithObject:window->front];
            for (MinatoCPUBuffer *buffer in window->buffers) {
                if (buffer != window->front && buffer->reading) {
                    [kept addObject:buffer];
                }
            }
            window->buffers = kept;
        }
        pthread_mutex_unlock(&state_lock);
    }
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
        window->posted_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
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
        if (by_contents ? !shown(window_id, buffer, format, visible)
                        : !presented(window_id, buffer, width, height, format, visible)) {
            read_done(buffer);
        }
    }
    if (by_contents && !visible) {
        const uint64_t post = buffer->posted;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)spare_ns),
                       dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                           drop_spares(window_id, post);
                       });
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

/* TRIM_MEMORY_UI_HIDDEN: the buffers of each window without a lock go, the one its sublayer shows too; a GPU read
   still running keeps its buffer until it ends. */
static void trimmed(void *context) {
    (void)context;
    NSMutableArray<NSNumber *> *trimmed_ids = [NSMutableArray array];
    pthread_mutex_lock(&state_lock);
    for (NSNumber *window_id in windows) {
        MinatoCPUWindow *window = windows[window_id];
        if (window->locked == nil) {
            window->buffers = nil;
            window->front = nil;
            [trimmed_ids addObject:window_id];
        }
    }
    pthread_mutex_unlock(&state_lock);
    // The buffer the sublayer shows goes too, outside the lock.
    for (NSNumber *window_id in trimmed_ids) {
        @autoreleasepool {
            CALayer *layer = (__bridge_transfer CALayer *)apkrt_window_layer(window_id.unsignedLongLongValue);
            CALayer *buffers = layer != nil ? found_buffers_layer(layer) : nil;
            if (buffers != nil) {
                [CATransaction begin];
                [CATransaction setDisableActions:YES];
                buffers.contents = nil;
                [CATransaction commit];
            }
        }
    }
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

// The layers of the tree under `layer`, in the order Core Animation draws them (zPosition aside).
static void draw_order(CALayer *layer, NSMutableArray<CALayer *> *order) {
    [order addObject:layer];
    for (CALayer *sublayer in layer.sublayers) {
        draw_order(sublayer, order);
    }
}

// The opacity `layer` is drawn with, 0 when it or a layer above it is hidden.
static float drawn_opacity(CALayer *layer) {
    float opacity = 1;
    for (CALayer *at = layer; at != nil; at = at.superlayer) {
        opacity = at.hidden ? 0 : opacity * at.opacity;
    }
    return opacity;
}

// The lines after the first of a dump's facts: the window layer's frame in its root's points and its root's size,
// the opacity it is drawn with, each layer drawn over it that shows something, cut to the frame, and each dumped
// window drawn under it that it overlaps, so that a screenshot is compared only where the window is what it shows.
static NSString *placed(CALayer *layer, NSArray<CALayer *> *order, NSDictionary<NSValue *, NSNumber *> *dumped) {
    CALayer *root = layer;
    while (root.superlayer != nil) {
        root = root.superlayer;
    }
    const CGRect frame = [layer convertRect:layer.bounds toLayer:root];
    NSMutableString *lines = [NSMutableString stringWithFormat:@"frame %g %g %g %g in %g %g alpha %g\n",
                                                               frame.origin.x, frame.origin.y, frame.size.width,
                                                               frame.size.height, root.bounds.size.width,
                                                               root.bounds.size.height, drawn_opacity(layer)];
    const NSUInteger at = [order indexOfObjectIdenticalTo:layer];
    for (NSUInteger index = 0; at != NSNotFound && index < order.count; index++) {
        CALayer *other = order[index];
        BOOL inside = NO;
        for (CALayer *up = other; up != nil && !inside; up = up.superlayer) {
            inside = up == layer;
        }
        const CGRect cut = CGRectIntersection(frame, [other convertRect:other.bounds toLayer:root]);
        if (inside || CGRectIsEmpty(cut) || drawn_opacity(other) == 0) {
            continue;
        }
        NSNumber *window_id = dumped[[NSValue valueWithNonretainedObject:other]];
        if (index < at && window_id != nil) {
            [lines appendFormat:@"under %@\n", window_id];
        } else if (index > at && (other.contents != nil || [other isKindOfClass:[CAMetalLayer class]] ||
                                  (other.backgroundColor != NULL && CGColorGetAlpha(other.backgroundColor) > 0))) {
            [lines appendFormat:@"above %g %g %g %g\n", cut.origin.x, cut.origin.y, cut.size.width, cut.size.height];
        }
    }
    return lines;
}

int minato_graphics_window_dump(const char *folder) {
    if (![NSFileManager.defaultManager createDirectoryAtPath:@(folder) withIntermediateDirectories:YES attributes:nil
                                                       error:nil]) {
        return -1;
    }
    // An earlier dump's windows go, also those that are gone since.
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(folder) error:nil]) {
        if ([name hasPrefix:@"window-"]) {
            [NSFileManager.defaultManager removeItemAtPath:[@(folder) stringByAppendingPathComponent:name] error:nil];
        }
    }
    NSMutableArray *dumps = [NSMutableArray array];
    pthread_mutex_lock(&state_lock);
    for (NSNumber *window_id in windows) {
        MinatoCPUWindow *window = windows[window_id];
        if (window->front != nil) {
            [dumps addObject:@[window_id, window->front, @(window->width), @(window->height), @(window->format),
                                @(window->front->posted), @(window->buffers.count)]];
        }
    }
    pthread_mutex_unlock(&state_lock);
    // The dumped windows' layers by layer, and the draw order of their trees.
    NSMutableDictionary<NSValue *, NSNumber *> *layers = [NSMutableDictionary dictionary];
    NSMutableArray<CALayer *> *order = [NSMutableArray array];
    for (NSArray *dump in dumps) {
        CALayer *layer = (__bridge_transfer CALayer *)apkrt_window_layer([dump[0] unsignedLongLongValue]);
        CALayer *root = layer;
        while (root.superlayer != nil) {
            root = root.superlayer;
        }
        if (layer != nil && [order indexOfObjectIdenticalTo:root] == NSNotFound) {
            draw_order(root, order);
        }
        if (layer != nil) {
            layers[[NSValue valueWithNonretainedObject:layer]] = dump[0];
        }
    }
    int written = 0;
    for (NSArray *dump in dumps) {
        const uint64_t window_id = [dump[0] unsignedLongLongValue];
        MinatoCPUBuffer *buffer = dump[1];
        const int32_t width = [dump[2] intValue], height = [dump[3] intValue], format = [dump[4] intValue];
        CALayer *layer = (__bridge_transfer CALayer *)apkrt_window_layer(window_id);
        const BOOL hidden = layer == nil || layer.hidden;
        const BOOL black = format != format_rgba_8888 || layer.opaque;
        NSMutableData *pixels = [NSMutableData dataWithLength:(NSUInteger)width * (NSUInteger)height * 4];
        IOSurfaceLock(buffer->surface, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *bits = IOSurfaceGetBaseAddress(buffer->surface);
        const size_t row = IOSurfaceGetBytesPerRow(buffer->surface);
        uint8_t *rgba = pixels.mutableBytes;
        if (format == format_rgb_565) {
            widen_565(bits, row, width, height, rgba);
        } else {
            for (int32_t y = 0; y < height; y++) {
                uint8_t *line = rgba + (size_t)y * (size_t)width * 4;
                memcpy(line, bits + (size_t)y * row, (size_t)width * 4);
                for (int32_t x = 0; format == format_rgbx_8888 && x < width; x++) {
                    line[4 * x + 3] = 255;
                }
            }
        }
        IOSurfaceUnlock(buffer->surface, kIOSurfaceLockReadOnly, NULL);
        static const char *const names[] = {"", "RGBA_8888", "RGBX_8888", "", "RGB_565"};
        NSString *facts = [NSString stringWithFormat:@"%dx%d %s%s post %llu %s\n%@buffers %@\n", width, height,
                                                     names[format], black ? " on black" : "",
                                                     [dump[5] unsignedLongLongValue], hidden ? "hidden" : "shown",
                                                     layer != nil ? placed(layer, order, layers) : @"", dump[6]];
        NSString *base = [NSString stringWithFormat:@"%s/window-%llu", folder, (unsigned long long)window_id];
        if (![pixels writeToFile:[base stringByAppendingString:@".rgba"] atomically:YES] ||
            ![facts writeToFile:[base stringByAppendingString:@".txt"] atomically:YES encoding:NSUTF8StringEncoding
                          error:nil]) {
            return -1;
        }
        written++;
    }
    return written;
}
