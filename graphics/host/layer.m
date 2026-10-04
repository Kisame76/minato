// graphics/host's layer and the parity benchmark's recorder (ADR-0037, decision 2), minato_layer.h.
#import "minato_layer.h"

#import <objc/runtime.h>

#include <TargetConditionals.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/// A drawable's place. The thread that took the drawable writes the first two fields before its presented handler
/// can run; the handler writes the rest.
struct slot {
    int64_t next_drawable_ns;
    uint64_t drawable_id;
    _Atomic(int64_t) presented_ns;
    _Atomic(uint64_t) presented_id;
    _Atomic(uint32_t) handlers;
};

static int64_t now_ns(void) {
    return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

#if !TARGET_OS_SIMULATOR
/// presentedTime is in seconds of the same host clock as CACurrentMediaTime, which counts mach_absolute_time as
/// CLOCK_UPTIME_RAW does.
static int64_t host_ns(CFTimeInterval seconds) {
    return seconds > 0 ? (int64_t)llround(seconds * 1e9) : 0;
}
#endif

@implementation MinatoLayerRecording {
@public
    struct slot *_slots;
    NSUInteger _capacity;
    /// Places handed out, also past the capacity.
    _Atomic(uint64_t) _taken;
    _Atomic(uint64_t) _nils;
    /// Presented handlers that ran.
    _Atomic(uint64_t) _answered;
    /// Recorded nextDrawable calls under way, and whether -stopRecording: has begun: a call that sees it records
    /// nothing.
    _Atomic(int) _inside;
    _Atomic(int) _stopped;
    /// The notes, written by the call that took the first place.
    qos_class_t _qos;
    MTLPixelFormat _format;
    CGSize _size;
    NSUInteger _maximum;
    BOOL _opaque;
}

- (nullable instancetype)initWithCapacity:(NSUInteger)capacity {
    if ((self = [super init]) == nil) return nil;
    _slots = calloc(capacity, sizeof *_slots);
    if (_slots == NULL) return nil;
    _capacity = capacity;
    return self;
}

- (void)dealloc {
    free(_slots);
}

- (NSUInteger)count {
    const uint64_t taken = atomic_load(&_taken);
    return taken < _capacity ? (NSUInteger)taken : _capacity;
}

- (NSUInteger)lost {
    const uint64_t taken = atomic_load(&_taken);
    return taken > _capacity ? (NSUInteger)(taken - _capacity) : 0;
}

- (NSUInteger)nils {
    return (NSUInteger)atomic_load(&_nils);
}

- (NSUInteger)unanswered {
    const uint64_t answered = atomic_load(&_answered);
    const NSUInteger count = self.count;
    return answered < count ? count - (NSUInteger)answered : 0;
}

- (qos_class_t)qosClass {
    return _qos;
}

- (NSString *)qosName {
    switch (_qos) {
    case QOS_CLASS_USER_INTERACTIVE:
        return @"user-interactive";
    case QOS_CLASS_USER_INITIATED:
        return @"user-initiated";
    case QOS_CLASS_DEFAULT:
        return @"default";
    case QOS_CLASS_UTILITY:
        return @"utility";
    case QOS_CLASS_BACKGROUND:
        return @"background";
    default:
        return @"unspecified";
    }
}

- (MTLPixelFormat)pixelFormat {
    return _format;
}

- (CGSize)drawableSize {
    return _size;
}

- (NSUInteger)maximumDrawableCount {
    return _maximum;
}

- (BOOL)opaque {
    return _opaque;
}

- (struct minato_layer_drawable)drawable:(NSUInteger)index {
    struct minato_layer_drawable drawable = {0};
    if (index >= self.count) return drawable;
    struct slot *slot = &_slots[index];
    drawable.next_drawable_ns = slot->next_drawable_ns;
    drawable.presented_ns = atomic_load(&slot->presented_ns);
    drawable.drawable_id = slot->drawable_id;
    drawable.presented_id = atomic_load(&slot->presented_id);
    drawable.handlers = atomic_load(&slot->handlers);
    return drawable;
}

@end

#if !TARGET_OS_SIMULATOR
// The recording that MinatoMetalLayer and the class with MinatoLayerRecorder's nextDrawable share (ADR-0042 decision
// 4): `next` is the layer's nextDrawable above the recorder's, CAMetalLayer's for MinatoMetalLayer.
static id<CAMetalDrawable> record_next_drawable(CAMetalLayer *layer, MinatoLayerRecording *recording, IMP next) {
    id<CAMetalDrawable> (*const call)(id, SEL) = (id<CAMetalDrawable>(*)(id, SEL))next;
    // With stop_recording's order (_stopped, then _inside), either it waits for this call or this call sees it.
    atomic_fetch_add(&recording->_inside, 1);
    if (atomic_load(&recording->_stopped)) {
        atomic_fetch_sub(&recording->_inside, 1);
        return call(layer, @selector(nextDrawable));
    }
    id<CAMetalDrawable> drawable = call(layer, @selector(nextDrawable));
    const int64_t taken = now_ns();
    if (drawable == nil) {
        atomic_fetch_add(&recording->_nils, 1);
    } else {
        const uint64_t index = atomic_fetch_add(&recording->_taken, 1);
        if (index < recording->_capacity) {
            struct slot *slot = &recording->_slots[index];
            slot->next_drawable_ns = taken;
            slot->drawable_id = drawable.drawableID;
            if (index == 0) {
                recording->_qos = qos_class_self();
                recording->_format = layer.pixelFormat;
                recording->_size = layer.drawableSize;
                recording->_maximum = layer.maximumDrawableCount;
                // Once, also on the console, where a benchmark run's driver checks it (V5's late frames).
                recording->_opaque = layer.opaque;
                fprintf(stderr, "MinatoMetalLayer: the recording's first drawable: layer opaque %d\n",
                        recording->_opaque ? 1 : 0);
            }
            // The block holds the recording, and with it the places, until it ran.
            [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
                atomic_store(&slot->presented_ns, host_ns(presented.presentedTime));
                atomic_store(&slot->presented_id, presented.drawableID);
                atomic_fetch_add(&slot->handlers, 1);
                atomic_fetch_add(&recording->_answered, 1);
            }];
        }
    }
    atomic_fetch_sub(&recording->_inside, 1);
    return drawable;
}
#endif

// A layer's -startRecording:, on its state: the running recording, unretained in `active`, and the last one in
// `current`, which keeps the running one alive.
static BOOL start_recording(_Atomic(uintptr_t) *active, MinatoLayerRecording *__strong *current, NSUInteger capacity) {
#if TARGET_OS_SIMULATOR
    (void)active;
    (void)current;
    (void)capacity;
    return NO;
#else
    if (capacity == 0 || atomic_load(active) != 0) return NO;
    MinatoLayerRecording *recording = [[MinatoLayerRecording alloc] initWithCapacity:capacity];
    if (recording == nil) return NO;
    *current = recording;
    atomic_store_explicit(active, (uintptr_t)(__bridge void *)recording, memory_order_release);
    return YES;
#endif
}

// A layer's -stopRecording:.
static MinatoLayerRecording *_Nullable stop_recording(_Atomic(uintptr_t) *active, NSTimeInterval timeout) {
    const uintptr_t running = atomic_exchange(active, 0);
    if (running == 0) return nil;
    MinatoLayerRecording *recording = (__bridge MinatoLayerRecording *)(void *)running;
    atomic_store(&recording->_stopped, 1);
    // A call under way may wait up to a second in CAMetalLayer's nextDrawable.
    while (atomic_load(&recording->_inside) != 0) usleep(100);
    const int64_t deadline = now_ns() + (timeout > 0 ? (int64_t)llround(timeout * 1e9) : 0);
    while (recording.unanswered != 0 && now_ns() < deadline) usleep(1000);
    return recording;
}

@implementation MinatoMetalLayer {
    /// The running recording, unretained; 0 outside a run, which is the one flag test.
    _Atomic(uintptr_t) _active;
    /// The last recording, which keeps the running one alive.
    MinatoLayerRecording *_current;
}

// The Simulator's MTLDrawable has no presented handlers, drawableID or presentedTime: there the layer keeps
// CAMetalLayer's nextDrawable, and -startRecording: returns NO.
#if !TARGET_OS_SIMULATOR
- (nullable id<CAMetalDrawable>)nextDrawable {
    const uintptr_t active = atomic_load_explicit(&_active, memory_order_acquire);
    if (active == 0) return [super nextDrawable];
    return record_next_drawable(self, (__bridge MinatoLayerRecording *)(void *)active,
                                class_getMethodImplementation(CAMetalLayer.class, @selector(nextDrawable)));
}
#endif

- (BOOL)startRecording:(NSUInteger)capacity {
    return start_recording(&_active, &_current, capacity);
}

- (nullable MinatoLayerRecording *)stopRecording:(NSTimeInterval)timeout {
    return stop_recording(&_active, timeout);
}

@end

/// A layer's recording state under MinatoLayerRecorder, as MinatoMetalLayer's ivars, tied to the layer.
@interface MinatoLayerRecorderState : NSObject {
@public
    _Atomic(uintptr_t) _active;
    MinatoLayerRecording *_current;
}
@end

@implementation MinatoLayerRecorderState
@end

static char recorder_state_key;
/// The class with the recorder's nextDrawable, set before its layers draw.
static Class recorder_class;

#if !TARGET_OS_SIMULATOR
// The nextDrawable that +attachToClass: gives the class. The state is the layer's associated object, set once by
// +startRecording:capacity: and kept as long as the layer, so it is read without a retain.
static id<CAMetalDrawable> recorder_next_drawable(CAMetalLayer *self, SEL command) {
    const IMP next = class_getMethodImplementation(class_getSuperclass(recorder_class), command);
    __unsafe_unretained MinatoLayerRecorderState *state = objc_getAssociatedObject(self, &recorder_state_key);
    const uintptr_t active = state != nil ? atomic_load_explicit(&state->_active, memory_order_acquire) : 0;
    if (active == 0) return ((id<CAMetalDrawable>(*)(id, SEL))next)(self, command);
    return record_next_drawable(self, (__bridge MinatoLayerRecording *)(void *)active, next);
}
#endif

@implementation MinatoLayerRecorder

+ (BOOL)attachToClass:(Class)layerClass {
#if TARGET_OS_SIMULATOR
    (void)layerClass;
    return NO;
#else
    if (recorder_class != Nil) return recorder_class == layerClass;
    if (layerClass == Nil || ![layerClass isSubclassOfClass:CAMetalLayer.class] ||
        [layerClass isSubclassOfClass:MinatoMetalLayer.class]) {
        return NO;
    }
    // The class first: once the method is there, its layers may call it on any thread. class_addMethod refuses a
    // class that defines nextDrawable itself.
    recorder_class = layerClass;
    const char *types = method_getTypeEncoding(class_getInstanceMethod(CAMetalLayer.class, @selector(nextDrawable)));
    if (!class_addMethod(layerClass, @selector(nextDrawable), (IMP)recorder_next_drawable, types)) {
        recorder_class = Nil;
        return NO;
    }
    return YES;
#endif
}

+ (BOOL)startRecording:(CAMetalLayer *)layer capacity:(NSUInteger)capacity {
    if (recorder_class == Nil || ![layer isKindOfClass:recorder_class]) return NO;
    MinatoLayerRecorderState *state = objc_getAssociatedObject(layer, &recorder_state_key);
    if (state == nil) {
        state = [MinatoLayerRecorderState new];
        objc_setAssociatedObject(layer, &recorder_state_key, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return start_recording(&state->_active, &state->_current, capacity);
}

+ (nullable MinatoLayerRecording *)stopRecording:(CAMetalLayer *)layer timeout:(NSTimeInterval)timeout {
    MinatoLayerRecorderState *state = objc_getAssociatedObject(layer, &recorder_state_key);
    return state != nil ? stop_recording(&state->_active, timeout) : nil;
}

@end
