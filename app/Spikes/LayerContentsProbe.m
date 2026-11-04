/* Spike S5 (ADR-0043 b2, the window's buffers): can graphics/host's window (graphics/host/window.m) show its CPU
   buffers' IOSurfaces as its layer's contents, without the two drawables its Metal pass draws into, which Android's
   window does not have? tools/layer-contents-probe.sh runs it on the Mac, where it opens a small window for about
   30 s, and with --iphone in a Debug build of the app launched with -runLayerProbe, which runs it on the key window
   once the scene is active and exits. Its report:
   1. Formats, through CARenderer offscreen, against what window.m's pass and Android's composition show: RGBA_8888
      (premultiplied, over a background), RGBX_8888 (an 'RGBA' surface with any alpha, as opaque, also over the layer's
      black background) and RGB_565 ('L565'), each with the errors of other readings to tell what Core Animation did.
   2. The hide rule: a hidden layer shows what is below it; contents and hidden NO in one transaction show the new
      buffer in the first frame rendered.
   3. On screen, with buffers of the screen's size (on the Mac the iPhone 15 Pro Max's, scaled into the window): the
      drawables as window.m presents them against contents, paced by the display link and unpaced. It reports the
      posts per tick, the waits for a free buffer, when a posted surface is in use and when a replaced one is not
      (IOSurfaceIsInUse, the only sign of a frame on screen without drawables, so what a recorder could take), the
      drawables' presented times, and the footprint that presenting adds to the three buffers.
   4. On the iPhone only: the render server's snapshot (drawViewHierarchyInRect:) of each format, and of the switch
      from contents to a drawable and back. */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <TargetConditionals.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif

#if TARGET_OS_OSX || DEBUG

enum { format_rgba = 1, format_rgbx = 2, format_565 = 4 };
enum { by_drawables, by_contents, by_contents_held };
static const char *const mode_names[] = {"drawables", "contents", "contents, each post held until in use"};
enum { buffer_count = 3 };

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipeline;

static void report(const char *format, ...) __printflike(1, 2);
static void report(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    putchar('\n');
    fflush(stdout);
}

static int64_t now_ns(void) {
    return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static double ms(int64_t ns) {
    return (double)ns / 1e6;
}

static double mib(int64_t bytes) {
    return (double)bytes / 1048576.0;
}

static int64_t footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS
               ? (int64_t)info.phys_footprint
               : -1;
}

static void on_main(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

static const char *format_name(int format) {
    return format == format_rgba ? "RGBA_8888" : format == format_rgbx ? "RGBX_8888" : "RGB_565";
}

static int compare_int64(const void *a, const void *b) {
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* The value at `fraction` of the sorted values, -1 for none; sorts them. */
static int64_t percentile(int64_t *values, size_t count, double fraction) {
    if (count == 0) {
        return -1;
    }
    qsort(values, count, sizeof *values, compare_int64);
    return values[(size_t)(fraction * (double)(count - 1) + 0.5)];
}

static uint32_t next_random(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

/* window.m's pass: each pixel of the drawable reads the buffer's at the same place, RGBX's alpha as 1. */
static NSString *const shader_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct Position { float4 position [[position]]; };\n"
     "vertex Position probe_vertex(uint index [[vertex_id]]) {\n"
     "    const float2 corner = float2((index << 1) & 2, index & 2);\n"
     "    return {float4(corner * 2 - 1, 0, 1)};\n"
     "}\n"
     "fragment float4 probe_fragment(Position in [[stage_in]],\n"
     "                               texture2d<float, access::read> pixels [[texture(0)]],\n"
     "                               constant uint &opaque [[buffer(0)]]) {\n"
     "    const float4 color = pixels.read(uint2(in.position.xy));\n"
     "    return opaque != 0 ? float4(color.rgb, 1) : color;\n"
     "}\n";

static BOOL set_up(void) {
    device = MTLCreateSystemDefaultDevice();
    queue = [device newCommandQueue];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:shader_source options:nil error:&error];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"probe_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"probe_fragment"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline = library != nil ? [device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
    if (pipeline == nil) {
        report("no Metal: %s", error != nil ? error.localizedDescription.UTF8String : "no device");
    }
    return pipeline != nil;
}

/* A buffer as window.m's new_buffer makes it. */
static IOSurfaceRef new_surface(int32_t width, int32_t height, int format) {
    const MTLPixelFormat pixel = format == format_565 ? MTLPixelFormatB5G6R5Unorm : MTLPixelFormatRGBA8Unorm;
    const size_t bytes = format == format_565 ? 2 : 4;
#if TARGET_OS_SIMULATOR
    // The Simulator's device asserts on asking about B5G6R5Unorm, which it does not have.
    const size_t minimum =
        format == format_565 ? 64 : (size_t)[device minimumLinearTextureAlignmentForPixelFormat:pixel];
#else
    const size_t minimum = (size_t)[device minimumLinearTextureAlignmentForPixelFormat:pixel];
#endif
    const size_t alignment = minimum > 64 ? minimum : 64;
    const size_t row = (IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)width * bytes) + alignment - 1) /
                       alignment * alignment;
    NSDictionary *properties = @{
        (id)kIOSurfaceWidth: @(width),
        (id)kIOSurfaceHeight: @(height),
        (id)kIOSurfaceBytesPerElement: @(bytes),
        (id)kIOSurfaceBytesPerRow: @(row),
        (id)kIOSurfacePixelFormat: @(format == format_565 ? 'L565' : 'RGBA'),
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)properties);
}

static void set_contents(CALayer *layer, IOSurfaceRef surface, BOOL hidden) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contents = (__bridge id)surface;
    layer.hidden = hidden;
    [CATransaction commit];
}

// MARK: - 1 and 2: formats and the hide rule, offscreen

enum { reading_android, reading_straight, reading_blended, reading_swapped, reading_count };
static const char *const reading_names[reading_count] = {"as Android", "straight alpha", "alpha blended",
                                                         "red and blue swapped"};

/* What one source pixel shows over `below` (RGB) in a reading: as Android does (window.m's pass over what is
   below: RGBA premultiplied, RGBX and RGB_565 opaque, 565 widened as Metal's unorm does), or another one. */
static void shown(int format, int reading, const uint8_t *source, const uint8_t below[3], int out[3]) {
    if (format == format_565) {
        const unsigned value = (unsigned)source[0] | (unsigned)source[1] << 8;
        const unsigned r = value >> 11, g = (value >> 5) & 63, b = value & 31;
        out[0] = (int)((r * 255 + 15) / 31);
        out[1] = (int)((g * 255 + 31) / 63);
        out[2] = (int)((b * 255 + 15) / 31);
        if (reading == reading_swapped) {
            const int red = out[0];
            out[0] = out[2];
            out[2] = red;
        }
        return;
    }
    const int alpha = source[3];
    for (int c = 0; c < 3; c++) {
        const int value = source[reading == reading_swapped ? 2 - c : c];
        int result;
        if (format == format_rgbx && reading == reading_android) {
            result = value;
        } else if (reading == reading_straight) {
            result = (value * alpha + below[c] * (255 - alpha) + 127) / 255;
        } else {
            result = value + (below[c] * (255 - alpha) + 127) / 255;
        }
        out[c] = result > 255 ? 255 : result;
    }
}

/* The largest difference of a channel between the BGRA render and the surface shown in `reading`. */
static int largest_error(int format, int reading, BOOL flipped, IOSurfaceRef surface, const uint8_t *render,
                         int32_t width, int32_t height, const uint8_t below[3]) {
    const uint8_t *bits = IOSurfaceGetBaseAddress(surface);
    const size_t row = IOSurfaceGetBytesPerRow(surface), bytes = format == format_565 ? 2 : 4;
    int largest = 0;
    for (int32_t y = 0; y < height; y++) {
        const uint8_t *source_row = bits + (size_t)(flipped ? height - 1 - y : y) * row;
        for (int32_t x = 0; x < width; x++) {
            int want[3];
            shown(format, reading, source_row + (size_t)x * bytes, below, want);
            const uint8_t *got = render + ((size_t)y * (size_t)width + (size_t)x) * 4;
            const int have[3] = {got[2], got[1], got[0]};
            for (int c = 0; c < 3; c++) {
                const int error = abs(want[c] - have[c]);
                largest = error > largest ? error : largest;
            }
        }
    }
    return largest;
}

static void fill_pattern(IOSurfaceRef surface, int format, uint32_t seed) {
    IOSurfaceLock(surface, 0, NULL);
    uint8_t *bits = IOSurfaceGetBaseAddress(surface);
    const size_t row = IOSurfaceGetBytesPerRow(surface);
    const size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            uint8_t *pixel = bits + y * row + x * (format == format_565 ? 2 : 4);
            const uint32_t random = next_random(&seed);
            if (format == format_565) {
                pixel[0] = (uint8_t)random;
                pixel[1] = (uint8_t)(random >> 8);
            } else if (format == format_rgbx) {
                memcpy(pixel, &random, 4);  // any alpha, 0 too
            } else {
                const unsigned alpha = random >> 24;
                for (int c = 0; c < 3; c++) {
                    pixel[c] = (uint8_t)(((random >> (8 * c)) & 0xff) * alpha / 255);
                }
                pixel[3] = (uint8_t)alpha;
            }
        }
    }
    IOSurfaceUnlock(surface, 0, NULL);
}

@interface ProbeRenderer : NSObject {
@public
    CARenderer *renderer;
    id<MTLTexture> target;
    CALayer *root;
    int32_t width, height;
    uint8_t *pixels;
}
@end

@implementation ProbeRenderer
- (void)dealloc {
    free(pixels);
}
@end

static ProbeRenderer *new_renderer(int32_t width, int32_t height) {
    ProbeRenderer *probe = [ProbeRenderer new];
    probe->width = width;
    probe->height = height;
    probe->pixels = calloc((size_t)width * (size_t)height, 4);
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:(NSUInteger)width
                                                                                         height:(NSUInteger)height
                                                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    probe->target = [device newTextureWithDescriptor:descriptor];
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    probe->renderer = [CARenderer rendererWithMTLTexture:probe->target
                                                 options:@{
                                                     kCARendererColorSpace: (__bridge id)srgb,
                                                     kCARendererMetalCommandQueue: queue,
                                                 }];
    CGColorSpaceRelease(srgb);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    probe->root = [CALayer layer];
    probe->root.anchorPoint = CGPointZero;
    probe->root.bounds = CGRectMake(0, 0, width, height);
    CGColorRef background = CGColorCreateSRGB(0.2, 0.6, 1.0, 1.0);
    probe->root.backgroundColor = background;
    CGColorRelease(background);
    [CATransaction commit];
    probe->renderer.layer = probe->root;
    probe->renderer.bounds = probe->root.bounds;
    return probe;
}

static void render(ProbeRenderer *probe) {
    [CATransaction flush];
    [probe->renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
    [probe->renderer addUpdateRect:probe->renderer.bounds];
    [probe->renderer render];
    [probe->renderer endFrame];
    id<MTLCommandBuffer> after = [queue commandBuffer];
    [after commit];
    [after waitUntilCompleted];
    [probe->target getBytes:probe->pixels
                bytesPerRow:(NSUInteger)probe->width * 4
                 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)probe->width, (NSUInteger)probe->height)
                mipmapLevel:0];
}

/* Each reading's largest error, upright and flipped; the best as Android's in `best`. */
static int errors_line(ProbeRenderer *probe, int format, IOSurfaceRef surface, const uint8_t below[3], char *line,
                       size_t size) {
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    int best = 256;
    size_t used = 0;
    for (int reading = 0; reading < reading_count; reading++) {
        if (format == format_565 && (reading == reading_straight || reading == reading_blended)) {
            continue;
        }
        if (format == format_rgba && reading == reading_blended) {
            continue;  // premultiplied: the same as Android's
        }
        const int upright =
            largest_error(format, reading, NO, surface, probe->pixels, probe->width, probe->height, below);
        const int flipped =
            largest_error(format, reading, YES, surface, probe->pixels, probe->width, probe->height, below);
        if (reading == reading_android) {
            best = upright < flipped ? upright : flipped;
        }
        used += (size_t)snprintf(line + used, size - used, "%s%s %d (flipped %d)", used ? ", " : "",
                                 reading_names[reading], upright, flipped);
    }
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    return best;
}

static void check_formats(void) {
    enum { width = 64, height = 48 };
    ProbeRenderer *probe = new_renderer(width, height);
    CAMetalLayer *layer = [CAMetalLayer layer];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.anchorPoint = CGPointZero;
    layer.frame = probe->root.bounds;
    layer.hidden = YES;
    [probe->root addSublayer:layer];
    [CATransaction commit];

    // What is below the window: the background alone.
    render(probe);
    const uint8_t below[3] = {probe->pixels[2], probe->pixels[1], probe->pixels[0]};
    report("1 formats (CARenderer %dx%d, sRGB, below %u %u %u)", width, height, below[0], below[1], below[2]);
    struct {
        int format;
        BOOL opaque, black;
    } const cases[] = {
        {format_rgba, NO, NO}, {format_rgbx, YES, NO}, {format_rgbx, YES, YES}, {format_565, YES, NO},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        IOSurfaceRef surface = new_surface(width, height, cases[index].format);
        fill_pattern(surface, cases[index].format, 0x9e3779b9u + (uint32_t)index);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.opaque = cases[index].opaque;
        CGColorRef black = CGColorCreateSRGB(0, 0, 0, 1);
        layer.backgroundColor = cases[index].black ? black : NULL;
        CGColorRelease(black);
        [CATransaction commit];
        set_contents(layer, surface, NO);
        render(probe);
        char line[512];
        const int best = errors_line(probe, cases[index].format, surface, below, line, sizeof line);
        const BOOL good = best <= 2;
        report("  %s%s%s: %s: %s", format_name(cases[index].format), cases[index].opaque ? " opaque" : "",
               cases[index].black ? " on black" : "", good ? "ok" : "DIFFERS", line);
        CFRelease(surface);
    }

    // 2: hidden shows what is below; shown again with new contents in one transaction, the first frame is new.
    IOSurfaceRef old = new_surface(width, height, format_rgba), fresh = new_surface(width, height, format_rgba);
    fill_pattern(old, format_rgba, 1);
    fill_pattern(fresh, format_rgba, 2);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.opaque = NO;
    layer.backgroundColor = NULL;
    [CATransaction commit];
    set_contents(layer, old, NO);
    render(probe);
    set_contents(layer, old, YES);
    render(probe);
    int hidden_error = 0;
    for (size_t index = 0; index < (size_t)width * height; index++) {
        for (int c = 0; c < 3; c++) {
            const int error = abs(probe->pixels[index * 4 + (size_t)(2 - c)] - below[c]);
            hidden_error = error > hidden_error ? error : hidden_error;
        }
    }
    set_contents(layer, fresh, NO);
    render(probe);
    char line[512];
    const int fresh_error = errors_line(probe, format_rgba, fresh, below, line, sizeof line);
    const int old_error = errors_line(probe, format_rgba, old, below, line, sizeof line);
    const BOOL hide_good = hidden_error <= 2 && fresh_error <= 2 && old_error > 2;
    report("2 hide rule: %s: hidden shows below with error %d; shown again in one transaction: new buffer error %d, "
           "old buffer error %d",
           hide_good ? "ok" : "DIFFERS", hidden_error, fresh_error, old_error);
    CFRelease(old);
    CFRelease(fresh);
}

// MARK: - 3: on screen

@interface ProbeBuffer : NSObject {
@public
    IOSurfaceRef surface;
    id<MTLTexture> texture;
    uint64_t posted;
    _Atomic(int) reading;
}
@end

@implementation ProbeBuffer
- (void)dealloc {
    if (surface != NULL) {
        CFRelease(surface);
    }
}
@end

static dispatch_semaphore_t ticked;
static _Atomic(uint64_t) ticks;
static _Atomic(int64_t) tick_period_ns;

@interface ProbeTicker : NSObject
@end

@implementation ProbeTicker
- (void)tick:(CADisplayLink *)link {
    atomic_store(&tick_period_ns, (int64_t)((link.targetTimestamp - link.timestamp) * 1e9));
    atomic_fetch_add(&ticks, 1);
    dispatch_semaphore_signal(ticked);
}
@end

static CADisplayLink *link_running;
#if TARGET_OS_OSX
static NSView *probe_view;
#endif

static void start_ticks(void) {
    on_main(^{
        ProbeTicker *ticker = [ProbeTicker new];
#if TARGET_OS_OSX
        link_running = [probe_view displayLinkWithTarget:ticker selector:@selector(tick:)];
#else
        link_running = [CADisplayLink displayLinkWithTarget:ticker selector:@selector(tick:)];
#endif
        link_running.preferredFrameRateRange = CAFrameRateRangeMake(120, 120, 120);
        [link_running addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    });
}

static void stop_ticks(void) {
    on_main(^{
        [link_running invalidate];
        link_running = nil;
    });
}

/* IOSurfaceIsInUse's changes for each buffer, polled every 100 µs. */
enum { changes_capacity = 8192 };
static struct {
    int64_t time[changes_capacity];
    uint8_t in_use[changes_capacity];
    unsigned count;
} changes[buffer_count];
static IOSurfaceRef watched[buffer_count];
static atomic_int watching;

static void *watch(void *unused) {
    (void)unused;
    int last[buffer_count] = {0};
    while (atomic_load(&watching)) {
        for (int index = 0; index < buffer_count; index++) {
            const int in_use = IOSurfaceIsInUse(watched[index]) ? 1 : 0;
            if (in_use != last[index] && changes[index].count < changes_capacity) {
                changes[index].time[changes[index].count] = now_ns();
                changes[index].in_use[changes[index].count++] = (uint8_t)in_use;
                last[index] = in_use;
            }
        }
        usleep(100);
    }
    return NULL;
}

/* The first change of buffer `index` to `in_use` at or after `from`, before `until`; -1 for none. */
static int64_t change_after(int index, int in_use, int64_t from, int64_t until) {
    for (unsigned at = 0; at < changes[index].count; at++) {
        if (changes[index].time[at] >= until) {
            break;
        }
        if (changes[index].time[at] >= from && changes[index].in_use[at] == in_use) {
            return changes[index].time[at];
        }
    }
    return -1;
}

enum { posts_capacity = 4096 };
static struct {
    int64_t time[posts_capacity];
    int buffer[posts_capacity];
    int paced[posts_capacity];
    int64_t lock_wait[posts_capacity];
    int64_t drawable_wait[posts_capacity];
    _Atomic(int64_t) presented[posts_capacity];  // the drawable's presentedTime in ns, 0 for not presented
} posts;

/* Writes a frame as an app's redraw does: all of the buffer, gray, with a white bar that moves 8 rows a post. */
static void draw_frame(ProbeBuffer *buffer, int format, unsigned post) {
    IOSurfaceLock(buffer->surface, 0, NULL);
    uint8_t *bits = IOSurfaceGetBaseAddress(buffer->surface);
    const size_t row = IOSurfaceGetBytesPerRow(buffer->surface), height = IOSurfaceGetHeight(buffer->surface);
    const uint32_t gray = format == format_565 ? 0x84108410u : 0xff808080u;
    memset_pattern4(bits, &gray, row * height);
    const size_t top = (size_t)post * 8 % (height - 16);
    memset(bits + top * row, 0xff, 16 * row);
    IOSurfaceUnlock(buffer->surface, 0, NULL);
}

static void present_drawable(CAMetalLayer *layer, ProbeBuffer *buffer, int format, unsigned post) {
    const int64_t asked = now_ns();
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    posts.drawable_wait[post] = now_ns() - asked;
    if (drawable == nil) {
        atomic_store(&buffer->reading, 0);
        return;
    }
#if !TARGET_OS_SIMULATOR
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        atomic_store(&posts.presented[post], (int64_t)(presented.presentedTime * 1e9));
    }];
#endif
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    const uint32_t opaque = format == format_rgba ? 0 : 1;
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:buffer->texture atIndex:0];
    [encoder setFragmentBytes:&opaque length:sizeof opaque atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        atomic_store(&buffer->reading, 0);
    }];
    [commands presentDrawable:drawable];
    [commands commit];
}

/* window.m's choice: of the buffers that are neither the front nor read, the one posted longest ago; by contents,
   read means in use (IOSurfaceIsInUse). Waits up to a second; nil after that. */
static ProbeBuffer *free_buffer(NSArray<ProbeBuffer *> *buffers, ProbeBuffer *front, int mode) {
    const int64_t deadline = now_ns() + 1000000000;
    do {
        ProbeBuffer *found = nil;
        for (ProbeBuffer *buffer in buffers) {
            const BOOL busy = mode != by_drawables ? IOSurfaceIsInUse(buffer->surface) : atomic_load(&buffer->reading);
            if (buffer != front && !busy && (found == nil || buffer->posted < found->posted)) {
                found = buffer;
            }
        }
        if (found != nil) {
            return found;
        }
        usleep(50);
    } while (now_ns() < deadline);
    return nil;
}

/* Waits until the footprint stops falling, up to 5 s: what a removed layer held goes some time after. */
static void settle(void) {
    int64_t last = footprint();
    for (int round = 0; round < 50; round++) {
        usleep(100000);
        const int64_t now = footprint();
        if (last - now < 262144 && round >= 4) {
            return;
        }
        last = now;
    }
}

static void run_screen(CALayer *container, int mode, int format, BOOL black, int32_t width, int32_t height) {
    settle();
    const int64_t start_footprint = footprint();
    __block CAMetalLayer *layer;
    on_main(^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer = [CAMetalLayer layer];
        layer.frame = container.bounds;
        layer.opaque = format != format_rgba;
        if (black) {
            CGColorRef color = CGColorCreateSRGB(0, 0, 0, 1);
            layer.backgroundColor = color;
            CGColorRelease(color);
        }
        if (mode == by_drawables) {
            layer.device = device;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.drawableSize = CGSizeMake(width, height);
        }
        [container addSublayer:layer];
        [CATransaction commit];
    });
    NSMutableArray<ProbeBuffer *> *buffers = [NSMutableArray array];
    for (int index = 0; index < buffer_count; index++) {
        ProbeBuffer *buffer = [ProbeBuffer new];
        buffer->surface = new_surface(width, height, format);
        if (mode == by_drawables) {
            const MTLPixelFormat pixel = format == format_565 ? MTLPixelFormatB5G6R5Unorm : MTLPixelFormatRGBA8Unorm;
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
            descriptor.pixelFormat = pixel;
            descriptor.width = (NSUInteger)width;
            descriptor.height = (NSUInteger)height;
            descriptor.usage = MTLTextureUsageShaderRead;
            descriptor.storageMode = MTLStorageModeShared;
            buffer->texture = [device newTextureWithDescriptor:descriptor iosurface:buffer->surface plane:0];
            if (buffer->texture == nil) {
                report("3 %s: no texture of an IOSurface here", format_name(format));
                return;
            }
        }
        draw_frame(buffer, format, 0);
        watched[index] = buffer->surface;
        changes[index].count = 0;
        [buffers addObject:buffer];
    }
    const int64_t buffered_footprint = footprint();

    pthread_t watcher;
    atomic_store(&watching, 1);
    pthread_create(&watcher, NULL, watch, NULL);
    memset(&posts, 0, sizeof posts);
    unsigned count = 0, missed = 0, timeouts = 0, stalls = 0;
    ProbeBuffer *front = nil;
    ticked = dispatch_semaphore_create(0);
    start_ticks();
    const uint64_t first_tick = atomic_load(&ticks);
    int64_t paced_end = 0, unpaced_start = 0;
    for (int paced = 1; paced >= 0; paced--) {
        const int64_t phase_start = now_ns(), phase_end = phase_start + (paced ? 3000000000 : 1500000000);
        if (!paced) {
            paced_end = phase_start;
            unpaced_start = phase_start;
        }
        while (now_ns() < phase_end && count < posts_capacity) {
            if (paced) {
                // A link that stops ticking, as for an occluded window on the Mac, counts and ends the phase.
                if (dispatch_semaphore_wait(ticked, dispatch_time(DISPATCH_TIME_NOW, 100000000)) != 0) {
                    stalls++;
                    break;
                }
                while (dispatch_semaphore_wait(ticked, DISPATCH_TIME_NOW) == 0) {
                    missed++;
                }
            }
            const int64_t asked = now_ns();
            ProbeBuffer *buffer = free_buffer(buffers, front, mode);
            posts.lock_wait[count] = now_ns() - asked;
            if (buffer == nil) {
                timeouts++;
                continue;
            }
            draw_frame(buffer, format, count);
            buffer->posted = count + 1;
            posts.buffer[count] = (int)[buffers indexOfObject:buffer];
            posts.paced[count] = paced;
            posts.time[count] = now_ns();
            if (mode != by_drawables) {
                set_contents(layer, buffer->surface, NO);
                // Held: the post returns once Core Animation uses the surface, so that a later free one is free
                // because it was replaced, never because it was not taken yet.
                const int64_t posted = now_ns();
                while (mode == by_contents_held && !IOSurfaceIsInUse(buffer->surface) &&
                       now_ns() - posted < 100000000) {
                    usleep(20);
                }
                posts.drawable_wait[count] = now_ns() - posted;
            } else {
                atomic_store(&buffer->reading, 1);
                present_drawable(layer, buffer, format, count);
            }
            front = buffer;
            count++;
        }
    }
    const int64_t run_end = now_ns();
    const uint64_t tick_count = atomic_load(&ticks) - first_tick;
    stop_ticks();
    usleep(200000);
    const int64_t presenting_footprint = footprint();
#if TARGET_OS_OSX
    // footprint -v names each IOSurface: the screen-sized ones are the three buffers and whatever presenting adds.
    char command[96], line[512], names[512] = "";
    snprintf(command, sizeof command, "/usr/bin/footprint -v -p %d 2>/dev/null", getpid());
    char size[32];
    snprintf(size, sizeof size, " %dx%d ", width, height);
    unsigned surfaces = 0, drawables = 0;
    FILE *listing = popen(command, "r");
    while (listing != NULL && fgets(line, sizeof line, listing) != NULL) {
        if (strstr(line, "IOSurface SID") != NULL && strstr(line, size) != NULL) {
            surfaces++;
            drawables += strstr(line, "Drawable") != NULL;
        }
    }
    if (listing != NULL) {
        pclose(listing);
    }
    snprintf(names, sizeof names, "%u IOSurfaces of %dx%d while presenting, %u of them drawables", surfaces, width,
             height, drawables);
#endif
    atomic_store(&watching, 0);
    pthread_join(watcher, NULL);

    // The paced posts' figures.
    int64_t *lock = calloc(count, sizeof *lock), *drawable = calloc(count, sizeof *drawable);
    int64_t *pickup = calloc(count, sizeof *pickup), *release = calloc(count, sizeof *release);
    int64_t *intervals = calloc(count, sizeof *intervals);
    size_t paced_count = 0, pickups = 0, releases = 0, interval_count = 0;
    unsigned never_in_use = 0, free_while_front = 0, not_presented = 0;
    int64_t last_shown = 0;
    for (unsigned post = 0; post < count; post++) {
        if (!posts.paced[post]) {
            continue;
        }
        lock[paced_count] = posts.lock_wait[post];
        drawable[paced_count++] = posts.drawable_wait[post];
        // Until the same buffer is posted again.
        int64_t again = run_end;
        for (unsigned later = post + 1; later < count; later++) {
            if (posts.buffer[later] == posts.buffer[post]) {
                again = posts.time[later];
                break;
            }
        }
        const int64_t replaced = post + 1 < count ? posts.time[post + 1] : run_end;
        int64_t shown_at = 0;
        if (mode != by_drawables) {
            const int64_t in_use = change_after(posts.buffer[post], 1, posts.time[post], again);
            if (in_use < 0) {
                never_in_use++;
                continue;
            }
            pickup[pickups++] = in_use - posts.time[post];
            if (change_after(posts.buffer[post], 0, in_use, replaced) >= 0) {
                free_while_front++;
            }
            const int64_t freed = change_after(posts.buffer[post], 0, replaced, again);
            if (freed >= 0 && post + 1 < count) {
                release[releases++] = freed - replaced;
                // The next frame is on screen once this one is free: the recorder's stand-in for presentedTime.
                shown_at = freed;
            }
        } else {
            shown_at = atomic_load(&posts.presented[post]);
            if (shown_at == 0) {
                not_presented++;
            }
        }
        if (shown_at > 0 && last_shown > 0) {
            intervals[interval_count++] = shown_at - last_shown;
        }
        if (shown_at > 0) {
            last_shown = shown_at;
        }
    }
    const double paced_seconds = ms(paced_end - posts.time[0]) / 1000;
    const unsigned unpaced = count - (unsigned)paced_count;
    unsigned unpaced_never = 0;
    for (unsigned post = 0; mode != by_drawables && post < count; post++) {
        if (posts.paced[post]) {
            continue;
        }
        int64_t again = run_end;
        for (unsigned later = post + 1; later < count; later++) {
            if (posts.buffer[later] == posts.buffer[post]) {
                again = posts.time[later];
                break;
            }
        }
        unpaced_never += change_after(posts.buffer[post], 1, posts.time[post], again) < 0;
    }
    const int64_t period = atomic_load(&tick_period_ns);
    report("3 %s%s by %s, %dx%d:", format_name(format), black ? " on black" : "", mode_names[mode], width, height);
    report("  paced: %zu posts in %.2f s, %llu ticks of %.2f ms in the run, %u missed, %u waits over 1 s%s",
           paced_count, paced_seconds, tick_count, ms(period), missed, timeouts, stalls ? ", THE LINK STOPPED" : "");
    report("  lock wait p50 %.2f p99 %.2f ms", ms(percentile(lock, paced_count, 0.5)),
           ms(percentile(lock, paced_count, 0.99)));
    if (mode == by_drawables) {
        report("  nextDrawable wait p50 %.2f p99 %.2f ms; not presented %u", ms(percentile(drawable, paced_count, 0.5)),
               ms(percentile(drawable, paced_count, 0.99)), not_presented);
    } else {
        if (mode == by_contents_held) {
            report("  post held p50 %.2f p99 %.2f ms", ms(percentile(drawable, paced_count, 0.5)),
                   ms(percentile(drawable, paced_count, 0.99)));
        }
        report("  in use after the post p50 %.2f p99 %.2f ms, never in use %u, free while front %u",
               ms(percentile(pickup, pickups, 0.5)), ms(percentile(pickup, pickups, 0.99)), never_in_use,
               free_while_front);
        report("  free after its replacement p50 %.2f p99 %.2f ms (%zu)", ms(percentile(release, releases, 0.5)),
               ms(percentile(release, releases, 0.99)), releases);
    }
    report("  frames shown %s: interval p50 %.2f p99 %.2f ms (%zu)",
           mode != by_drawables ? "(as the replaced buffer's release)" : "(presentedTime)",
           ms(percentile(intervals, interval_count, 0.5)), ms(percentile(intervals, interval_count, 0.99)),
           interval_count);
    const double unpaced_seconds = ms(run_end - unpaced_start) / 1000;
    report("  unpaced: %u posts in %.2f s (%.0f a second), never in use %u", unpaced, unpaced_seconds,
           unpaced / unpaced_seconds, unpaced_never);
    free(lock);
    free(drawable);
    free(pickup);
    free(release);
    free(intervals);

    on_main(^{
        [CATransaction begin];
        [layer removeFromSuperlayer];
        [CATransaction commit];
        [CATransaction flush];
    });
    layer = nil;
    [buffers removeAllObjects];
    settle();
    report("  footprint: buffers %.1f MiB, presenting +%.1f MiB, %.1f MiB left after the layer's removal",
           mib(buffered_footprint - start_footprint), mib(presenting_footprint - buffered_footprint),
           mib(footprint() - start_footprint));
#if TARGET_OS_OSX
    report("  %s", names);
#endif
}

// MARK: - 4: the render server's snapshot (iPhone)

#if TARGET_OS_IOS
/* Four bands of each format, and what Android shows of them over `below`. */
static void fill_bands(IOSurfaceRef surface, int format, const uint8_t below[3], int want[4][3]) {
    static const uint8_t rgba[4][4] = {{200, 40, 40, 255}, {40, 200, 40, 255}, {0, 0, 0, 0}, {64, 32, 16, 128}};
    static const uint8_t rgbx[4][4] = {{200, 40, 40, 0}, {40, 200, 40, 0x40}, {40, 40, 200, 0xff}, {128, 128, 128, 16}};
    static const uint16_t rgb565[4] = {31 << 11, 63 << 5, 31, 16 << 11 | 32 << 5 | 16};
    IOSurfaceLock(surface, 0, NULL);
    uint8_t *bits = IOSurfaceGetBaseAddress(surface);
    const size_t row = IOSurfaceGetBytesPerRow(surface), width = IOSurfaceGetWidth(surface);
    const size_t height = IOSurfaceGetHeight(surface);
    for (int band = 0; band < 4; band++) {
        uint8_t pixel[4];
        if (format == format_565) {
            pixel[0] = (uint8_t)rgb565[band];
            pixel[1] = (uint8_t)(rgb565[band] >> 8);
        } else {
            memcpy(pixel, format == format_rgba ? rgba[band] : rgbx[band], 4);
        }
        shown(format, reading_android, pixel, below, want[band]);
        for (size_t y = height * (size_t)band / 4; y < height * (size_t)(band + 1) / 4; y++) {
            for (size_t x = 0; x < width; x++) {
                memcpy(bits + y * row + x * (format == format_565 ? 2 : 4), pixel, format == format_565 ? 2 : 4);
            }
        }
    }
    IOSurfaceUnlock(surface, 0, NULL);
}

/* The snapshot's sRGB colors at the centers of the four bands. */
static void snapshot_bands(UIWindow *window, uint8_t colors[4][3]) {
    on_main(^{
        UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
        format.scale = 1;
        format.preferredRange = UIGraphicsImageRendererFormatRangeStandard;
        format.opaque = YES;
        UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithBounds:window.bounds
                                                                                     format:format];
        UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
            (void)context;
            [window drawViewHierarchyInRect:window.bounds afterScreenUpdates:YES];
        }];
        const size_t width = (size_t)window.bounds.size.width, height = (size_t)window.bounds.size.height;
        uint8_t *pixels = calloc(width * height, 4);
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef bitmap = CGBitmapContextCreate(pixels, width, height, 8, width * 4, srgb,
                                                    (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
        CGContextDrawImage(bitmap, CGRectMake(0, 0, width, height), image.CGImage);
        for (int band = 0; band < 4; band++) {
            const uint8_t *pixel = pixels + ((height * (size_t)(2 * band + 1) / 8) * width + width / 2) * 4;
            memcpy(colors[band], pixel, 3);
        }
        CGContextRelease(bitmap);
        CGColorSpaceRelease(srgb);
        free(pixels);
    });
}

static int bands_error(uint8_t colors[4][3], int want[4][3]) {
    int largest = 0;
    for (int band = 0; band < 4; band++) {
        for (int c = 0; c < 3; c++) {
            const int error = abs(colors[band][c] - want[band][c]);
            largest = error > largest ? error : largest;
        }
    }
    return largest;
}

/* Each drawable in a color of its own, so that a snapshot tells the last one from the one before. */
static const uint8_t drawable_colors[3][3] = {{250, 200, 0}, {0, 200, 250}, {200, 0, 250}};
static unsigned drawables_presented;

static void present_color(CAMetalLayer *layer) {
    const uint8_t *color = drawable_colors[drawables_presented++ % 3];
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(color[0] / 255.0, color[1] / 255.0, color[2] / 255.0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    [commands presentDrawable:drawable];
    [commands commit];
}

/* What band 1 of the snapshot shows, and what the layer's contents hold. */
static void switch_line(UIWindow *window, const char *step, CAMetalLayer *layer, IOSurfaceRef buffer) {
    uint8_t colors[4][3];
    snapshot_bands(window, colors);
    __block NSString *held;
    on_main(^{
        id contents = layer.contents;
        if (contents == nil) {
            held = @"nil";
        } else if (CFGetTypeID((__bridge CFTypeRef)contents) == IOSurfaceGetTypeID()) {
            held = contents == (__bridge id)buffer ? @"the buffer" : @"another IOSurface";
        } else {
            held = NSStringFromClass([contents class]);
        }
    });
    const uint8_t *last = drawable_colors[(drawables_presented + 2) % 3];
    report("4 switch, %s: band 1 shows %u %u %u (the last drawable %u %u %u, the buffer's band 200 40 40), contents %s",
           step, colors[0][0], colors[0][1], colors[0][2], last[0], last[1], last[2], held.UTF8String);
}

static void check_snapshots(UIWindow *window, CALayer *container, int32_t width, int32_t height) {
    const uint8_t below[3] = {51, 153, 255};
    __block CAMetalLayer *layer;
    on_main(^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer = [CAMetalLayer layer];
        layer.frame = container.bounds;
        [container addSublayer:layer];
        [CATransaction commit];
    });
    IOSurfaceRef surfaces[3];
    const int formats[3] = {format_rgba, format_rgbx, format_565};
    for (int index = 0; index < 3; index++) {
        surfaces[index] = new_surface(width, height, formats[index]);
        int want[4][3];
        fill_bands(surfaces[index], formats[index], below, want);
        const int format = formats[index];
        on_main(^{
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            layer.opaque = format != format_rgba;
            CGColorRef black = CGColorCreateSRGB(0, 0, 0, 1);
            layer.backgroundColor = format == format_rgbx ? black : NULL;
            CGColorRelease(black);
            [CATransaction commit];
        });
        set_contents(layer, surfaces[index], NO);
        usleep(150000);
        uint8_t colors[4][3];
        snapshot_bands(window, colors);
        const int error = bands_error(colors, want);
        report("4 snapshot %s: %s, error %d: %u %u %u / %u %u %u / %u %u %u / %u %u %u", format_name(formats[index]),
               error <= 4 ? "ok" : "DIFFERS", error, colors[0][0], colors[0][1], colors[0][2], colors[1][0],
               colors[1][1], colors[1][2], colors[2][0], colors[2][1], colors[2][2], colors[3][0], colors[3][1],
               colors[3][2]);
    }

    // The switch, with RGBA_8888's bands (the Simulator has no RGB_565). On the window's layer itself, new: first a
    // drawable alone, the control that the snapshot shows drawables, then contents, a drawable, contents again, a
    // drawable, contents let go (nil) and a drawable, then the layer's own contents of the first drawable put back
    // and a drawable. Then on a child CALayer that holds the buffer over a new window layer: hidden, the parent's
    // drawable shows; shown, the buffer.
    on_main(^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [layer removeFromSuperlayer];
        layer = [CAMetalLayer layer];
        layer.frame = container.bounds;
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.drawableSize = CGSizeMake(width, height);
        [container addSublayer:layer];
        [CATransaction commit];
    });
    __block id own = nil;
    const char *steps[9] = {"a drawable alone", "contents", "a drawable after contents", "contents after a drawable",
                            "a drawable again", "contents let go", "a drawable after contents let go",
                            "the layer's own contents put back", "a drawable after they are back"};
    for (int step = 0; step < 9; step++) {
        if (step == 1 || step == 3) {
            set_contents(layer, surfaces[0], NO);
        } else if (step == 5) {
            set_contents(layer, NULL, NO);
        } else if (step == 7) {
            on_main(^{
                [CATransaction begin];
                [CATransaction setDisableActions:YES];
                layer.contents = own;
                [CATransaction commit];
            });
        } else {
            present_color(layer);
        }
        usleep(150000);
        if (step == 0) {
            on_main(^{
                own = layer.contents;
            });
        }
        switch_line(window, steps[step], layer, surfaces[0]);
    }
    __block CALayer *child;
    on_main(^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [layer removeFromSuperlayer];
        layer = [CAMetalLayer layer];
        layer.frame = container.bounds;
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.drawableSize = CGSizeMake(width, height);
        child = [CALayer layer];
        child.frame = layer.bounds;
        [layer addSublayer:child];
        [container addSublayer:layer];
        [CATransaction commit];
    });
    const char *child_steps[4] = {"child: the buffer", "child hidden, a drawable", "child shown again",
                                  "child hidden again, a drawable"};
    for (int step = 0; step < 4; step++) {
        set_contents(child, surfaces[0], step == 1 || step == 3);
        if (step == 1 || step == 3) {
            present_color(layer);
        }
        usleep(150000);
        switch_line(window, child_steps[step], layer, surfaces[0]);
    }
    on_main(^{
        [layer removeFromSuperlayer];
    });
    for (int index = 0; index < 3; index++) {
        CFRelease(surfaces[index]);
    }
}
#endif

// MARK: - The run

static void run_probe(CALayer *container, int32_t width, int32_t height, void *window) {
#if TARGET_OS_OSX
    (void)window;
#endif
    report("LAYER PROBE (spike S5) on %s, %s", device.name.UTF8String,
           NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
    check_formats();
#if TARGET_OS_OSX
    // Without a display that is on and unlocked, nothing is composited and the display link does not tick.
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    if (CGDisplayIsAsleep(CGMainDisplayID()) || [session[@"CGSSessionScreenIsLocked"] boolValue]) {
        report("3 on screen: not run, the display is asleep or the screen locked");
        report("LAYER PROBE done");
        return;
    }
#endif
    run_screen(container, by_contents, format_rgba, NO, width, height);
    run_screen(container, by_contents_held, format_rgba, NO, width, height);
    run_screen(container, by_contents_held, format_rgbx, YES, width, height);
    run_screen(container, by_contents_held, format_565, NO, width, height);
    run_screen(container, by_drawables, format_rgba, NO, width, height);
#if TARGET_OS_IOS
    check_snapshots((__bridge UIWindow *)window, container, width, height);
#endif
    report("LAYER PROBE done");
}

static CALayer *new_container(CGRect frame) {
    CALayer *container = [CALayer layer];
    container.frame = frame;
    container.zPosition = 1000;
    CGColorRef background = CGColorCreateSRGB(0.2, 0.6, 1.0, 1.0);
    container.backgroundColor = background;
    CGColorRelease(background);
    container.masksToBounds = YES;
    return container;
}

#if TARGET_OS_OSX
int main(void) {
    @autoreleasepool {
        if (!set_up()) {
            return 1;
        }
        NSApplication *application = NSApplication.sharedApplication;
        application.activationPolicy = NSApplicationActivationPolicyAccessory;
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 300, 650)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"Minato layer probe";
        window.level = NSFloatingWindowLevel;  // never occluded, which would stop its display link
        window.releasedWhenClosed = NO;
        probe_view = window.contentView;
        probe_view.wantsLayer = YES;
        CALayer *container = new_container(probe_view.bounds);
        [probe_view.layer addSublayer:container];
        [window orderFrontRegardless];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
            usleep(500000);
            run_probe(container, 1290, 2796, NULL);
            exit(0);
        });
        [application run];
    }
    return 0;
}
#else
/* -runLayerProbe: once the scene is active, on its key window, then the app exits; without a window in 30 s it says
   so and exits too, so that devicectl's console ends. */
__attribute__((constructor)) static void layer_probe_requested(void) {
    if (![NSProcessInfo.processInfo.arguments containsObject:@"-runLayerProbe"]) {
        return;
    }
    static id observer;
    observer = [NSNotificationCenter.defaultCenter
        addObserverForName:UISceneDidActivateNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *notification) {
                    UIWindowScene *scene = notification.object;
                    UIWindow *window = scene.keyWindow ?: scene.windows.firstObject;
                    if (window == nil || observer == nil || !set_up()) {
                        return;
                    }
                    [NSNotificationCenter.defaultCenter removeObserver:observer];
                    observer = nil;
                    CALayer *container = new_container(window.bounds);
                    [window.layer addSublayer:container];
                    const CGSize pixels = scene.screen.nativeBounds.size;
                    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
                        usleep(500000);
                        run_probe(container, (int32_t)pixels.width, (int32_t)pixels.height,
                                  (__bridge void *)window);
                        exit(0);
                    });
                }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (observer != nil) {
            report("LAYER PROBE: no active scene with a window in 30 s");
            exit(1);
        }
    });
}
#endif

#endif
