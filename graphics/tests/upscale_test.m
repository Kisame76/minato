/* graphics.upscale (ADR-0067 step 1): graphics/host's layer pair (upscale.m) on the Mac, without a window or a guest.
   A fake guest draws into the pair's low layer on its own MTLCommandQueue and presents with presentDrawable: and
   directly; the visible layer's drawable must then hold the upscaled picture:
     - a pattern of coloured cells at the low size, read back at the cell centres, with MTLFXSpatialScaler and with the
       bilinear pass the pair uses where the device has no scaler;
     - frame after frame, each preceded by long GPU work and each of its own colour: no pass may read an older or a
       later frame (the pass goes behind the frame on the producer's own queue, decision 4);
     - the presents counted on the visible layer, one for each frame, and the present hook of the low drawable never
       waiting for a drawable of the visible layer, which has none for a while (B1's deadlock), and a frame dropped when
       the visible layer gives none in time;
     - present, presentAtTime: and presentAfterMinimumDuration: all reach the pair.
   Usage: graphics-upscale-test. Exits 1 after the cases when one failed; "skipped:" without a Metal device. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minato_upscale.h"

static int failures;

#define EXPECT(condition)                                                                    \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            fprintf(stderr, "FAILED  %s:%d: %s\n", __func__, __LINE__, #condition);          \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

enum { low_w = 64, low_h = 36, visible_w = 128, visible_h = 72, cell = 4 };

static id<MTLDevice> device;
static id<MTLCommandQueue> producer;
static id<MTLBuffer> scratch;  // the long work's target

/* The visible layer as a host's might behave under load: nextDrawable that waits, or gives none, and notes the thread
   that asked, which must not be the one that presents. */
@interface TestVisibleLayer : CAMetalLayer
@property NSTimeInterval delay;
@property BOOL gives_none;
@property(atomic) int asked_on_main;
@property(atomic) int asked;
@end
@implementation TestVisibleLayer
- (nullable id<CAMetalDrawable>)nextDrawable {
    self.asked++;
    if (NSThread.isMainThread) self.asked_on_main++;
    if (self.delay > 0) [NSThread sleepForTimeInterval:self.delay];
    return self.gives_none ? nil : [super nextDrawable];
}
@end

static TestVisibleLayer *make_visible(NSUInteger drawables) {
    TestVisibleLayer *layer = [TestVisibleLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.drawableSize = CGSizeMake(visible_w, visible_h);
    layer.maximumDrawableCount = drawables;
    return layer;
}

/* The pixel of the frames' pattern at cell (x, y), as BGRA. */
static void cell_color(int cx, int cy, uint8_t bgra[4]) {
    bgra[0] = 0x80, bgra[1] = (uint8_t)(cy * 28), bgra[2] = (uint8_t)(cx * 16), bgra[3] = 255;
}

/* A frame's colour for the order test. */
static void frame_color(uint64_t k, uint8_t bgra[4]) {
    bgra[0] = (uint8_t)((k * 5) % 251), bgra[1] = (uint8_t)((k * 13) % 241), bgra[2] = (uint8_t)((k * 7) % 239 + 10),
    bgra[3] = 255;
}

enum present_kind { by_command_buffer, by_present, by_present_at, by_present_after };

/* The guest's frame: long work first (several fills of a big buffer on the producer queue), then the picture, then the
   present. `pattern` draws the cells by a copy from a buffer, else the picture is the colour `bgra` by a clear. */
static void guest_frame(CAMetalLayer *low, BOOL pattern, const uint8_t bgra[4], enum present_kind kind) {
    id<CAMetalDrawable> drawable = [low nextDrawable];
    EXPECT(drawable != nil);
    if (drawable == nil) return;
    id<MTLCommandBuffer> buffer = [producer commandBuffer];
    id<MTLBlitCommandEncoder> work = [buffer blitCommandEncoder];
    for (int round = 0; round < 6; round++) {
        [work fillBuffer:scratch range:NSMakeRange(0, scratch.length) value:(uint8_t)round];
    }
    [work endEncoding];
    if (pattern) {
        const size_t row = low_w * 4;
        id<MTLBuffer> source = [device newBufferWithLength:row * low_h options:MTLResourceStorageModeShared];
        uint8_t *bytes = source.contents;
        for (int y = 0; y < low_h; y++) {
            for (int x = 0; x < low_w; x++) cell_color(x / cell, y / cell, bytes + (size_t)y * row + (size_t)x * 4);
        }
        id<MTLBlitCommandEncoder> copy = [buffer blitCommandEncoder];
        [copy copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * low_h
                  sourceSize:MTLSizeMake(low_w, low_h, 1) toTexture:drawable.texture destinationSlice:0
            destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [copy endEncoding];
    } else {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake(bgra[2] / 255.0, bgra[1] / 255.0, bgra[0] / 255.0, 1.0);
        [[buffer renderCommandEncoderWithDescriptor:pass] endEncoding];
    }
    if (kind == by_command_buffer) [buffer presentDrawable:drawable];
    [buffer commit];
    switch (kind) {
    case by_present: [drawable present]; break;
    case by_present_at: [drawable presentAtTime:CACurrentMediaTime()]; break;
    case by_present_after: [drawable presentAfterMinimumDuration:0.0]; break;
    default: break;
    }
}

/* The guest's commit returns before Metal runs the present from the scheduled handler: wait until `count` presents
   reached the hook, then until their passes are done. */
static BOOL settled(MinatoUpscalePair *pair, uint64_t count, NSTimeInterval timeout) {
    const int64_t until = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (int64_t)(timeout * 1e9);
    while (pair.lowPresents < count) {
        if ((int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) > until) return NO;
        usleep(500);
    }
    return [pair waitIdle:timeout];
}

static BOOL near(const uint8_t *pixel, const uint8_t expected[4], int tolerance) {
    for (int channel = 0; channel < 3; channel++) {
        if (abs((int)pixel[channel] - (int)expected[channel]) > tolerance) return NO;
    }
    return YES;
}

/* The pattern, read at the middle of each scaled cell. */
static void pattern_case(BOOL spatial) {
    CAMetalLayer *visible = make_visible(3);
    MinatoUpscalePair *pair = [[MinatoUpscalePair alloc] initWithVisibleLayer:visible
                                                                      lowSize:CGSizeMake(low_w, low_h)
                                                                producerQueue:producer
                                                                spatialScaler:spatial];
    EXPECT(pair != nil);
    if (pair == nil) return;
    EXPECT(pair.usesSpatialScaler == (spatial && [MinatoUpscalePair supportsDevice:device]));
    EXPECT((int)pair.lowLayer.drawableSize.width == low_w && (int)pair.lowLayer.drawableSize.height == low_h);
    EXPECT(!pair.lowLayer.framebufferOnly && pair.lowLayer.superlayer == nil);
    __block int wrong = -1, checked = 0;
    __block uint64_t seen = 99;
    const int tolerance = pair.usesSpatialScaler ? 16 : 2;
    pair.observer = ^(uint64_t frame, const uint8_t *pixels, size_t row_bytes) {
        seen = frame;
        wrong = 0;
        for (int cy = 0; cy < low_h / cell; cy++) {
            for (int cx = 0; cx < low_w / cell; cx++) {
                uint8_t expected[4];
                cell_color(cx, cy, expected);
                const int scale = visible_w / low_w;
                const uint8_t *pixel = pixels + (size_t)(cy * cell * scale + cell * scale / 2) * row_bytes +
                                       (size_t)(cx * cell * scale + cell * scale / 2) * 4;
                checked++;
                if (!near(pixel, expected, tolerance)) wrong++;
            }
        }
    };
    guest_frame(pair.lowLayer, YES, NULL, by_command_buffer);
    EXPECT(settled(pair, 1, 5));
    EXPECT(seen == 0 && checked == (low_w / cell) * (low_h / cell));
    EXPECT(wrong == 0);
    EXPECT(pair.lowPresents == 1 && pair.visiblePresents == 1 && pair.completedPasses == 1 && pair.droppedFrames == 0);
    printf("  pattern (%s): %d cells checked, %d wrong\n", pair.usesSpatialScaler ? "MetalFX" : "bilinear", checked,
           wrong);
}

/* Frame after frame, each after long work: the pass reads its own frame, never an older one. */
static void order_case(BOOL spatial, int frames) {
    CAMetalLayer *visible = make_visible(3);
    MinatoUpscalePair *pair = [[MinatoUpscalePair alloc] initWithVisibleLayer:visible
                                                                      lowSize:CGSizeMake(low_w, low_h)
                                                                producerQueue:producer
                                                                spatialScaler:spatial];
    EXPECT(pair != nil);
    if (pair == nil) return;
    NSLock *lock = [NSLock new];
    __block int wrong = 0, observed = 0;
    __block uint64_t last = 0;
    __block BOOL in_order = YES;
    pair.observer = ^(uint64_t frame, const uint8_t *pixels, size_t row_bytes) {
        uint8_t expected[4];
        frame_color(frame, expected);
        // The middle, and the four corners: a uniform frame is uniform everywhere.
        const size_t at[5][2] = {{visible_w / 2, visible_h / 2}, {3, 3}, {visible_w - 4, 3}, {3, visible_h - 4},
                                 {visible_w - 4, visible_h - 4}};
        BOOL fine = YES;
        for (int i = 0; i < 5; i++) fine = fine && near(pixels + at[i][1] * row_bytes + at[i][0] * 4, expected, 3);
        [lock lock];
        wrong += !fine;
        in_order = in_order && (observed == 0 || frame > last);
        last = frame;
        observed++;
        [lock unlock];
    };
    for (int k = 0; k < frames; k++) {
        uint8_t color[4];
        frame_color((uint64_t)k, color);
        guest_frame(pair.lowLayer, NO, color, by_command_buffer);
    }
    EXPECT(settled(pair, (uint64_t)frames, 30));
    // A guest faster than the passes loses the frames the next one replaces (the newest wins); what is shown is each
    // frame's own picture, in order, and the last frame is always shown.
    EXPECT(observed >= 10 && wrong == 0);
    EXPECT(in_order && last == (uint64_t)frames - 1);
    EXPECT(pair.lowPresents == (uint64_t)frames);
    EXPECT(pair.completedPasses == (uint64_t)observed && pair.visiblePresents == (uint64_t)observed);
    EXPECT(pair.completedPasses + pair.droppedFrames == (uint64_t)frames);
    printf("  order (%s): %d frames, %d wrong\n", pair.usesSpatialScaler ? "MetalFX" : "bilinear", observed, wrong);
}

/* The visible layer waits for a while (no drawable free): the hook of a present from this very thread returns at once,
   and the pass is the host queue's. */
static void the_hook_never_waits(void) {
    TestVisibleLayer *visible = make_visible(3);
    visible.delay = 0.4;
    MinatoUpscalePair *pair = [[MinatoUpscalePair alloc] initWithVisibleLayer:visible
                                                                      lowSize:CGSizeMake(low_w, low_h)
                                                                producerQueue:producer
                                                                spatialScaler:YES];
    EXPECT(pair != nil);
    if (pair == nil) return;
    __block int observed = 0;
    pair.observer = ^(uint64_t frame, const uint8_t *pixels, size_t row_bytes) {
        (void)frame, (void)pixels, (void)row_bytes;
        observed++;
    };
    uint8_t color[4] = {10, 20, 30, 255};
    const int64_t start = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    guest_frame(pair.lowLayer, NO, color, by_present_after);  // the present runs on this thread
    const int64_t returned = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    EXPECT(returned < 100 * 1000000);       // the guest was not held up by the visible layer's 400 ms
    EXPECT(pair.longestHookNs < 5 * 1000000);
    EXPECT(pair.completedPasses == 0);      // the pass is still waiting for its drawable
    EXPECT(settled(pair, 1, 5));
    EXPECT(pair.visiblePresents == 1 && pair.completedPasses == 1 && pair.droppedFrames == 0 && observed == 1);
    EXPECT(visible.asked == 1 && visible.asked_on_main == 0);  // the host's queue asked, not the presenting thread
    printf("  hook: guest returned in %.3f ms, longest hook %.3f ms, with a visible layer that waits 400 ms\n",
           (double)returned / 1e6, (double)pair.longestHookNs / 1e6);
}

/* The visible layer gives none in time: the frame is dropped, the guest is not stopped and draws on. */
static void a_starved_frame_is_dropped(void) {
    TestVisibleLayer *visible = make_visible(3);
    visible.gives_none = YES;
    MinatoUpscalePair *pair = [[MinatoUpscalePair alloc] initWithVisibleLayer:visible
                                                                      lowSize:CGSizeMake(low_w, low_h)
                                                                producerQueue:producer
                                                                spatialScaler:YES];
    EXPECT(pair != nil);
    if (pair == nil) return;
    uint8_t color[4] = {10, 20, 30, 255};
    // more frames than the low pool holds
    for (int k = 0; k < 8; k++) guest_frame(pair.lowLayer, NO, color, by_command_buffer);
    EXPECT(settled(pair, 8, 5));
    EXPECT(pair.droppedFrames == 8 && pair.visiblePresents == 0 && pair.completedPasses == 0);
    visible.gives_none = NO;
    guest_frame(pair.lowLayer, NO, color, by_command_buffer);
    EXPECT(settled(pair, 9, 5));
    EXPECT(pair.visiblePresents == 1 && pair.completedPasses == 1 && pair.droppedFrames == 8);
}

/* A frame that waits for a visible layer that waits is replaced by the next: the newest wins, the older is dropped. */
static void the_newest_frame_wins(void) {
    TestVisibleLayer *visible = make_visible(3);
    visible.delay = 0.4;
    MinatoUpscalePair *pair = [[MinatoUpscalePair alloc] initWithVisibleLayer:visible
                                                                      lowSize:CGSizeMake(low_w, low_h)
                                                                producerQueue:producer
                                                                spatialScaler:YES];
    EXPECT(pair != nil);
    if (pair == nil) return;
    NSLock *lock = [NSLock new];
    __block uint64_t newest = 0;
    __block int observed = 0;
    pair.observer = ^(uint64_t frame, const uint8_t *pixels, size_t row_bytes) {
        (void)pixels, (void)row_bytes;
        [lock lock];
        newest = frame;
        observed++;
        [lock unlock];
    };
    for (int k = 0; k < 3; k++) {
        uint8_t color[4];
        frame_color((uint64_t)k, color);
        guest_frame(pair.lowLayer, NO, color, by_command_buffer);
    }
    EXPECT(settled(pair, 3, 10));
    EXPECT(pair.droppedFrames == 1 && pair.completedPasses == 2 && pair.visiblePresents == 2);
    EXPECT(observed == 2 && newest == 2);  // the last frame is the one that was shown
    // The guest was not stopped on the way: all three frames drew on the low layer's pool of three.
}

/* All three presents of a drawable reach the pair. */
static void every_present_is_hooked(void) {
    CAMetalLayer *visible = make_visible(3);
    MinatoUpscalePair *pair = [[MinatoUpscalePair alloc] initWithVisibleLayer:visible
                                                                      lowSize:CGSizeMake(low_w, low_h)
                                                                producerQueue:producer
                                                                spatialScaler:YES];
    EXPECT(pair != nil);
    if (pair == nil) return;
    uint8_t color[4] = {10, 20, 30, 255};
    const enum present_kind kinds[] = {by_present, by_present_at, by_present_after, by_command_buffer};
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) guest_frame(pair.lowLayer, NO, color, kinds[i]);
    EXPECT(settled(pair, 4, 10));
    EXPECT(pair.lowPresents == 4 && pair.visiblePresents == 4 && pair.completedPasses == 4);
}

/* Step 0's scaler rows on the Mac (usage: graphics-upscale-test --measure): the GPU time of a pass and how close its
   picture is to the full-size one, from a synthetic scene (thin lines, a checker, a gradient, text-like strokes) boxed
   down to each scale and scaled back up to the 1290 x 2796 screen by MetalFX and by the bilinear pass. This Mac's GPU
   is not the iPhone's, and a synthetic scene says little about an app's art: the rows are for comparing the two
   passes with each other. */
static void measure_scene(int scene) {
    const int w = 1290, h = 2796;
    const size_t row = (size_t)w * 4;
    uint8_t *full = malloc(row * (size_t)h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = full + (size_t)y * row + (size_t)x * 4;
            if (scene == 1) {  // soft: a gradient with a low-frequency texture, no hard edges
                const double fx = (double)x / w, fy = (double)y / h;
                const double t = 0.5 + 0.25 * sin(fx * 23.0) * cos(fy * 31.0) + 0.2 * sin((fx + fy) * 9.0);
                p[0] = (uint8_t)(255 * fmin(fmax(t, 0.0), 1.0));
                p[1] = (uint8_t)(255 * fx);
                p[2] = (uint8_t)(255 * fy);
                p[3] = 255;
                continue;
            }
            int v = (x * 255) / w;                                  // a gradient
            if (((x / 24) + (y / 24)) % 2 == 0 && y < h / 3) v = 255 - v / 2;  // a checker
            if (y % 37 == 0 || x % 41 == 0) v = 255;                // thin lines
            if (y > 2 * h / 3 && ((x * 7 + y * 3) % 29) < 3) v = 0;  // strokes
            p[0] = (uint8_t)v, p[1] = (uint8_t)(255 - v), p[2] = (uint8_t)((x + y) % 256), p[3] = 255;
        }
    }
    printf("scene: %s\n",
           scene == 1 ? "soft (gradient and a low-frequency texture)" : "edges (lines, a checker, strokes)");
    printf("scale  low size    pass GPU ms (MetalFX / bilinear)   PSNR dB vs full size (MetalFX / bilinear)\n");
    const double scales[] = {0.85, 0.67, 0.5};
    for (size_t which = 0; which < 3; which++) {
        const double scale = scales[which];
        const int lw = (int)(w * scale), lh = (int)(h * scale);
        // The low frame: each low pixel the mean of the full pixels it covers.
        const size_t lrow = (size_t)lw * 4;
        uint8_t *low = malloc(lrow * (size_t)lh);
        for (int y = 0; y < lh; y++) {
            for (int x = 0; x < lw; x++) {
                const int x0 = x * w / lw, y0 = y * h / lh;
                const int x1 = (x + 1) * w / lw > x0 + 1 ? (x + 1) * w / lw : x0 + 1;
                const int y1 = (y + 1) * h / lh > y0 + 1 ? (y + 1) * h / lh : y0 + 1;
                int sum[3] = {0, 0, 0}, n = 0;
                for (int yy = y0; yy < y1; yy++) {
                    for (int xx = x0; xx < x1; xx++, n++) {
                        for (int c = 0; c < 3; c++) sum[c] += full[(size_t)yy * row + (size_t)xx * 4 + (size_t)c];
                    }
                }
                for (int c = 0; c < 3; c++) low[(size_t)y * lrow + (size_t)x * 4 + (size_t)c] = (uint8_t)(sum[c] / n);
                low[(size_t)y * lrow + (size_t)x * 4 + 3] = 255;
            }
        }
        double gpu[2] = {0, 0}, psnr[2] = {0, 0};
        for (int mode = 0; mode < 2; mode++) {  // 0 MetalFX, 1 bilinear
            TestVisibleLayer *visible = make_visible(3);
            visible.drawableSize = CGSizeMake(w, h);
            MinatoUpscalePair *pair = [[MinatoUpscalePair alloc] initWithVisibleLayer:visible
                                                                              lowSize:CGSizeMake(lw, lh)
                                                                        producerQueue:producer
                                                                        spatialScaler:mode == 0];
            if (pair == nil || (mode == 0 && !pair.usesSpatialScaler)) continue;
            id<MTLBuffer> source = [device newBufferWithBytes:low
                                                       length:lrow * (size_t)lh
                                                      options:MTLResourceStorageModeShared];
            __block uint8_t *got = NULL;
            pair.observer = ^(uint64_t frame, const uint8_t *pixels, size_t row_bytes) {
                if (frame == 0) {
                    got = malloc(row_bytes * (size_t)h);
                    memcpy(got, pixels, row_bytes * (size_t)h);
                }
            };
            NSMutableArray<NSNumber *> *times = [NSMutableArray new];
            for (int k = 0; k < 40; k++) {
                id<CAMetalDrawable> d = [pair.lowLayer nextDrawable];
                id<MTLCommandBuffer> b = [producer commandBuffer];
                id<MTLBlitCommandEncoder> copy = [b blitCommandEncoder];
                [copy copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:lrow sourceBytesPerImage:lrow * (size_t)lh
                          sourceSize:MTLSizeMake((NSUInteger)lw, (NSUInteger)lh, 1) toTexture:d.texture
                    destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [copy endEncoding];
                [b presentDrawable:d];
                [b commit];
                if (!settled(pair, (uint64_t)k + 1, 10)) break;
                if (k == 0) pair.observer = nil;  // the readback of the first frame only
                if (k >= 5 && pair.lastPassGpuNs > 0) [times addObject:@(pair.lastPassGpuNs)];
            }
            [times sortUsingSelector:@selector(compare:)];
            gpu[mode] = times.count ? [times[times.count / 2] doubleValue] / 1e6 : 0;
            if (got != NULL) {
                double error = 0;
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        for (int c = 0; c < 3; c++) {
                            const double d = (double)full[(size_t)y * row + (size_t)x * 4 + (size_t)c] -
                                             (double)got[(size_t)y * row + (size_t)x * 4 + (size_t)c];
                            error += d * d;
                        }
                    }
                }
                error /= (double)w * h * 3;
                psnr[mode] = error > 0 ? 10 * log10(255.0 * 255.0 / error) : 99;
                free(got);
            }
        }
        printf("%.2f   %4d x %4d   %.3f / %.3f                      %.2f / %.2f\n", scale, lw, lh, gpu[0], gpu[1],
               psnr[0], psnr[1]);
        free(low);
    }
    free(full);
}

static void measure(void) {
    measure_scene(0);
    measure_scene(1);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            printf("skipped: no Metal device\n");
            return 0;
        }
        producer = [device newCommandQueue];
        if (argc > 1 && strcmp(argv[1], "--measure") == 0) {
            measure();
            return 0;
        }
        scratch = [device newBufferWithLength:64 << 20 options:MTLResourceStorageModePrivate];
        printf("MTLFXSpatialScaler on this device: %s\n", [MinatoUpscalePair supportsDevice:device] ? "yes" : "no");
        pattern_case(NO);
        order_case(NO, 40);
        if ([MinatoUpscalePair supportsDevice:device]) {
            pattern_case(YES);
            order_case(YES, 200);
        } else {
            printf("  (no scaler here: the MetalFX cases are left out)\n");
        }
        the_hook_never_waits();
        a_starved_frame_is_dropped();
        the_newest_frame_wins();
        every_present_is_hooked();
        printf("graphics.upscale: %s\n", failures == 0 ? "all fine" : "failed");
    }
    return failures == 0 ? 0 : 1;
}
