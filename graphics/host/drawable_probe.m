/* ADR-0041 decision 11 c)'s probe of the drawables' pool; see minato_graphics.h. Host code only. On the iPhone, a layer
   of its own lies over the app's window, which it finds through UIKit's runtime, as graphics/host does not link UIKit,
   and a display link paces the frames. On the Mac, where it runs for the tests, an offscreen layer and a sleep stand in
   for both, so its numbers there say nothing about the pool. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <TargetConditionals.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "minato_graphics.h"

/* The first frames of a run, which settle the pipeline, count for nothing. */
enum { kSettling = 30, kHeld = 3 };

static int64_t now_ns(void) {
    return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static int compare(const void *left, const void *right) {
    const int64_t a = *(const int64_t *)left, b = *(const int64_t *)right;
    return (a > b) - (a < b);
}

/* The p-th percentile of `count` values, which it sorts. */
static double percentile_ms(int64_t *values, int count, int p) {
    qsort(values, (size_t)count, sizeof *values, compare);
    return (double)values[(count - 1) * p / 100] / 1e6;
}

/* One run: `frames` frames at `rate` a second, each of which clears a drawable and presents it, then lets it go or
   holds it until it takes the drawable kHeld frames later, as MoltenVK holds an image's drawable until it acquires that
   image again. */
@interface MinatoDrawableRun : NSObject
@property(nonatomic) CAMetalLayer *layer;
@property(nonatomic) id<MTLCommandQueue> queue;
@property(nonatomic) BOOL holds;
@property(nonatomic) int rate, frames, frame;
@property(nonatomic) BOOL done;
@end

@implementation MinatoDrawableRun {
    int64_t *_waits, *_intervals, _last;
    id<CAMetalDrawable> _held[kHeld];
}

- (void)dealloc {
    free(_waits);
    free(_intervals);
}

- (BOOL)start {
    _waits = calloc((size_t)self.frames, sizeof *_waits);
    _intervals = calloc((size_t)self.frames, sizeof *_intervals);
    return _waits != NULL && _intervals != NULL;
}

- (void)tick:(id)sender {
    (void)sender;
    const int frame = self.frame;
    const int64_t tick = now_ns();
    if (frame > 0) {
        _intervals[frame - 1] = tick - _last;
    }
    _last = tick;
    if (self.holds) {
        _held[frame % kHeld] = nil;
    }
    const int64_t asked = now_ns();
    id<CAMetalDrawable> drawable = [self.layer nextDrawable];
    _waits[frame] = now_ns() - asked;
    if (drawable != nil) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        const double gray = frame % 2 == 0 ? 0.30 : 0.36;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(gray, gray, gray, 1);
        id<MTLCommandBuffer> commands = [self.queue commandBuffer];
        [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
        [commands presentDrawable:drawable];
        [commands commit];
        if (self.holds) {
            _held[frame % kHeld] = drawable;
        }
    }
    self.frame = frame + 1;
    if (self.frame == self.frames) {
        for (int index = 0; index < kHeld; index++) {
            _held[index] = nil;
        }
        self.done = YES;
    }
}

/* What the run saw, after its settling frames. */
- (void)report {
    const int counted = self.frames - kSettling;
    const double period_ms = 1000.0 / self.rate;
    int long_waits = 0;
    for (int frame = kSettling; frame < self.frames; frame++) {
        long_waits += (double)_waits[frame] / 1e6 > period_ms / 2;
    }
    const double p50 = percentile_ms(_waits + kSettling, counted, 50);
    const double p99 = percentile_ms(_waits + kSettling, counted, 99);
    const double ticks = percentile_ms(_intervals + kSettling, counted - 1, 50);
    printf("drawable probe: %d Hz, %s: %d frames, nextDrawable waited p50 %.2f ms, p99 %.2f ms, %d longer than half a "
           "period; ticks %.2f ms apart (p50)\n",
           self.rate, self.holds ? "held 3 frames" : "let go at present", counted, p50, p99, long_waits, ticks);
}

@end

#if TARGET_OS_IPHONE
/* The app's window's layer, found through UIKit's runtime: nil without one. */
static CALayer *window_layer(CGFloat *scale) {
    id application = [NSClassFromString(@"UIApplication") valueForKey:@"sharedApplication"];
    for (id scene in [application valueForKey:@"connectedScenes"]) {
        if (![scene respondsToSelector:NSSelectorFromString(@"windows")]) continue;
        for (id window in [scene valueForKey:@"windows"]) {
            *scale = [[window valueForKeyPath:@"screen.scale"] doubleValue];
            return [window valueForKey:@"layer"];
        }
    }
    return nil;
}
#endif

int minato_drawable_probe(double seconds) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            fprintf(stderr, "drawable probe: no Metal device\n");
            return 1;
        }
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.opaque = YES;
#if TARGET_OS_IPHONE
        CGFloat scale = 1;
        CALayer *window = window_layer(&scale);
        if (window == nil) {
            fprintf(stderr, "drawable probe: no window to lie over\n");
            return 1;
        }
        layer.frame = window.bounds;
        layer.contentsScale = scale;
        layer.drawableSize = CGSizeMake(window.bounds.size.width * scale, window.bounds.size.height * scale);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [window addSublayer:layer];
        [CATransaction commit];
#else
        layer.drawableSize = CGSizeMake(64, 64);
#endif
        printf("drawable probe: a layer of %.0f x %.0f pixels, %lu drawables\n", layer.drawableSize.width,
               layer.drawableSize.height, (unsigned long)layer.maximumDrawableCount);
        const id<MTLCommandQueue> queue = [device newCommandQueue];
        const int rates[] = {60, 120};
        for (size_t which = 0; which < sizeof rates / sizeof *rates; which++) {
            for (int holds = 0; holds <= 1; holds++) {
                MinatoDrawableRun *run = [MinatoDrawableRun new];
                run.layer = layer;
                run.queue = queue;
                run.holds = holds;
                run.rate = rates[which];
                run.frames = kSettling + (int)(seconds * rates[which]);
                if (![run start]) {
                    fprintf(stderr, "drawable probe: no memory\n");
                    return 1;
                }
#if TARGET_OS_IPHONE
                CADisplayLink *link = [CADisplayLink displayLinkWithTarget:run selector:@selector(tick:)];
                const float rate = (float)rates[which];
                link.preferredFrameRateRange = CAFrameRateRangeMake(rate, rate, rate);
                [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSDefaultRunLoopMode];
                while (!run.done) {
                    [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                                           beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
                }
                [link invalidate];
#else
                while (!run.done) {
                    [run tick:nil];
                    usleep((useconds_t)(1000000 / rates[which]));
                }
#endif
                [run report];
            }
        }
#if TARGET_OS_IPHONE
        [layer removeFromSuperlayer];
#endif
        fflush(stdout);
    }
    return 0;
}
