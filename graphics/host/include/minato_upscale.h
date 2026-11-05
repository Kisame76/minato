/* graphics/host's layer pair for the upscaler (ADR-0067, decision 4): a guest's window that is drawn at a low size and
   shown at the layer's own size. The guest draws into the pair's hidden low layer exactly as into any CAMetalLayer;
   when its frame is presented, the host takes a drawable of the visible layer, scales the low drawable into it with
   MTLFXSpatialScaler and presents that one. Objective-C, for the app's bridging header and the tests on the Mac. */
#ifndef MINATO_UPSCALE_H
#define MINATO_UPSCALE_H

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

/* A frame's pixels after the pass, as the visible layer's drawable holds them: BGRA, rows of `row_bytes`. Runs on a
   queue of the host's, after the pass completed; `frame` counts the presents of the low layer from 0. */
typedef void (^MinatoUpscaleObserver)(uint64_t frame, const uint8_t *pixels, size_t row_bytes);

@interface MinatoUpscalePair : NSObject
/* Whether this device has MTLFXSpatialScaler. NO does not stop the pair: it draws the low frame bilinearly instead
   (the baseline of ADR-0067 decision 4 that does not depend on the device); Core Animation's own scaling of a single
   layer without a pair is the other fallback. */
+ (BOOL)supportsDevice:(id<MTLDevice>)device;

/* The pair for `visible` (its device set, in the pixel format the guest draws in, which the pair gives the low layer
   too) with the guest's frames at `low_size` in pixels. `producer` is the guest's MTLCommandQueue, the queue the
   frames are committed on: the host's pass is committed on it too, after the frame, so that Metal orders it behind the
   frame by its same-queue rules (decision 4). `spatial` asks for MTLFXSpatialScaler; where the device has none, or
   its texture usages do not fit the two layers' drawables (checked at the first pass), or for NO, the pass is a
   bilinear draw of the low texture, so that the pair works on every device. nil for an empty size or a layer without
   a device. The pair sets `visible.framebufferOnly` to NO (the scaler writes into its drawables and the test's
   readback reads them), which costs some bandwidth on the device: a window sets it only while a pair hangs and puts
   it back when the pair goes. A frame that waits for the visible layer is replaced by a newer one (the newest wins,
   the older counts as dropped), so that a layer with no drawable for a while neither piles up frames nor shows old
   ones later. */
- (nullable instancetype)initWithVisibleLayer:(CAMetalLayer *)visible
                                      lowSize:(CGSize)low_size
                                producerQueue:(id<MTLCommandQueue>)producer
                                spatialScaler:(BOOL)spatial;

/* Whether the passes use MTLFXSpatialScaler (else the bilinear draw). It is NO from the pass on that found no scaler
   it can use. */
@property(readonly) BOOL usesSpatialScaler;

/* The hidden layer the guest draws into: never in a view, `framebufferOnly` NO so that the scaler reads it. */
@property(readonly) CAMetalLayer *lowLayer;
/* Presents of the low layer's drawables that reached the host, by `present` or `presentAtTime:` (a `presentDrawable:`
   of a command buffer calls the first from its scheduled handler). */
@property(readonly) uint64_t lowPresents;
/* Passes committed on the visible layer, each with its `presentDrawable:`: the presents counted on the visible
   layer. */
@property(readonly) uint64_t visiblePresents;
/* Passes whose command buffer completed, and frames dropped because the visible layer gave no drawable in time. */
@property(readonly) uint64_t completedPasses;
@property(readonly) uint64_t droppedFrames;
/* The longest a low drawable's present took inside the hook, in ns: it only hands the frame on, never waits for a
   drawable of the visible layer (that is on the host's serial queue, B1's deadlock). */
@property(readonly) int64_t longestHookNs;
/* The GPU time of the newest completed pass in ns (its command buffer's GPUEndTime - GPUStartTime); 0 before one. */
@property(readonly) int64_t lastPassGpuNs;
/* For tests: the observer gets each frame's pixels after its pass; set it before the first present. */
@property(copy, nullable) MinatoUpscaleObserver observer;

/* Waits up to `timeout` seconds until every frame handed over has had its pass completed or dropped. */
- (BOOL)waitIdle:(NSTimeInterval)timeout;
@end

NS_ASSUME_NONNULL_END

#endif
