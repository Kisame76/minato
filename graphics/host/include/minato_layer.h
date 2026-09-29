/* graphics/host's layer (ADR-0037, decision 2): the game view's CAMetalLayer in every build, and the parity
   benchmark's recorder. Objective-C, for the app's bridging header and the tests on the Mac. */
#ifndef MINATO_LAYER_H
#define MINATO_LAYER_H

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdint.h>
#include <sys/qos.h>

NS_ASSUME_NONNULL_BEGIN

/* A drawable of a recording. Drawable k is the k-th that nextDrawable returned during the recording, frame k of a
   run. Times are host time, CLOCK_UPTIME_RAW (the guest's CLOCK_MONOTONIC, F10), in ns. */
struct minato_layer_drawable {
    /* When nextDrawable returned it. */
    int64_t next_drawable_ns;
    /* MTLDrawable.presentedTime: 0 when the drawable never reached the screen or its presented handler has not
       run yet. */
    int64_t presented_ns;
    /* MTLDrawable.drawableID as nextDrawable returned it, and as its presented handler saw it. */
    uint64_t drawable_id;
    uint64_t presented_id;
    /* How often its presented handler ran: 1 once it did. */
    uint32_t handlers;
};

/* A recording, which keeps its numbers after -stopRecording:. */
@interface MinatoLayerRecording : NSObject
/* The drawables recorded, at most the capacity. */
@property(readonly) NSUInteger count;
/* Drawables past the capacity, not recorded. */
@property(readonly) NSUInteger lost;
/* nextDrawable calls that returned nil, which take no place. */
@property(readonly) NSUInteger nils;
/* Recorded drawables whose presented handler has not run yet. */
@property(readonly) NSUInteger unanswered;
/* Noted at the first drawable: the caller's QoS class, and the layer's pixel format, drawable size and
   maximumDrawableCount. */
@property(readonly) qos_class_t qosClass;
/* The QoS class as run.csv writes it: user-interactive, user-initiated, default, utility, background or
   unspecified. */
@property(readonly) NSString *qosName;
@property(readonly) MTLPixelFormat pixelFormat;
@property(readonly) CGSize drawableSize;
@property(readonly) NSUInteger maximumDrawableCount;
/* Drawable `index`, which is below `count`. */
- (struct minato_layer_drawable)drawable:(NSUInteger)index;
@end

/* The layer. Outside a recording its nextDrawable costs one flag test more than CAMetalLayer's. During a
   recording, nextDrawable puts each drawable it returns into the next place: the time, its ID, and a presented
   handler that adds the present time. The places are fixed and never wrap, so drawable k stays frame k.
   The recorder needs the device's Metal: the Simulator's MTLDrawable has no presented handlers, drawableID or
   presentedTime, so there the layer is CAMetalLayer as it is and records nothing. */
@interface MinatoMetalLayer : CAMetalLayer
/* Starts a recording with room for `capacity` drawables. NO while one runs, for a capacity of 0, without memory,
   or in the Simulator. It lets go of the recording before, so it belongs between runs, while no other thread is in
   nextDrawable. */
- (BOOL)startRecording:(NSUInteger)capacity;
/* Ends the recording, also while another thread draws: once this returns, nextDrawable records nothing more.
   Waits up to `timeout` seconds until each recorded drawable's presented handler ran. nil when no recording
   runs. */
- (nullable MinatoLayerRecording *)stopRecording:(NSTimeInterval)timeout;
@end

NS_ASSUME_NONNULL_END

#endif
