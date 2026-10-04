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
/* Noted at the first drawable: the caller's QoS class, and the layer's pixel format, drawable size,
   maximumDrawableCount and opacity. The opacity also goes to stderr, once, as "MinatoMetalLayer: the recording's
   first drawable: layer opaque 0" or 1, for a benchmark run's console (V5's late frames). */
@property(readonly) qos_class_t qosClass;
/* The QoS class as run.csv writes it: user-interactive, user-initiated, default, utility, background or
   unspecified. */
@property(readonly) NSString *qosName;
@property(readonly) MTLPixelFormat pixelFormat;
@property(readonly) CGSize drawableSize;
@property(readonly) NSUInteger maximumDrawableCount;
@property(readonly) BOOL opaque;
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

/* The recorder for a layer whose class has no nextDrawable of its own and cannot be a MinatoMetalLayer, such as
   Godot's GDTMetalLayer in the twin (ADR-0042 decision 4): the same recording, through a nextDrawable that
   +attachToClass: gives the class at run time. Each layer of the class keeps its own recording. Outside a recording
   that nextDrawable costs a lookup of the layer's state more than the class's own. In the Simulator it records
   nothing, as MinatoMetalLayer. */
@interface MinatoLayerRecorder : NSObject
/* Gives the class the recording nextDrawable, before its layers draw: YES, also again for that class. NO for a class
   that is no CAMetalLayer, is MinatoMetalLayer or below it, or defines nextDrawable itself, while another class has
   it, or in the Simulator. */
+ (BOOL)attachToClass:(Class)layerClass;
/* As MinatoMetalLayer's -startRecording:, for a layer of that class; NO for any other layer. */
+ (BOOL)startRecording:(CAMetalLayer *)layer capacity:(NSUInteger)capacity;
/* As MinatoMetalLayer's -stopRecording:, nil when the layer runs no recording. */
+ (nullable MinatoLayerRecording *)stopRecording:(CAMetalLayer *)layer timeout:(NSTimeInterval)timeout;
@end

NS_ASSUME_NONNULL_END

#endif
