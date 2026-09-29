// graphics/host's layer and the parity benchmark's recorder (ADR-0037, decision 2), minato_layer.h.
#import "minato_layer.h"

#include <math.h>
#include <stdatomic.h>
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

/// presentedTime is in seconds of the same host clock as CACurrentMediaTime, which counts mach_absolute_time as
/// CLOCK_UPTIME_RAW does.
static int64_t host_ns(CFTimeInterval seconds) {
    return seconds > 0 ? (int64_t)llround(seconds * 1e9) : 0;
}

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

@implementation MinatoMetalLayer {
    /// The running recording, unretained; 0 outside a run, which is the one flag test.
    _Atomic(uintptr_t) _active;
    /// The last recording, which keeps the running one alive.
    MinatoLayerRecording *_current;
}

- (nullable id<CAMetalDrawable>)nextDrawable {
    const uintptr_t active = atomic_load_explicit(&_active, memory_order_acquire);
    if (active == 0) return [super nextDrawable];
    return [self recordNextDrawable:(__bridge MinatoLayerRecording *)(void *)active];
}

- (nullable id<CAMetalDrawable>)recordNextDrawable:(MinatoLayerRecording *)recording {
    // With -stopRecording:'s order (_stopped, then _inside), either it waits for this call or this call sees it.
    atomic_fetch_add(&recording->_inside, 1);
    if (atomic_load(&recording->_stopped)) {
        atomic_fetch_sub(&recording->_inside, 1);
        return [super nextDrawable];
    }
    id<CAMetalDrawable> drawable = [super nextDrawable];
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
                recording->_format = self.pixelFormat;
                recording->_size = self.drawableSize;
                recording->_maximum = self.maximumDrawableCount;
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

- (BOOL)startRecording:(NSUInteger)capacity {
    if (capacity == 0 || atomic_load(&_active) != 0) return NO;
    MinatoLayerRecording *recording = [[MinatoLayerRecording alloc] initWithCapacity:capacity];
    if (recording == nil) return NO;
    _current = recording;
    atomic_store_explicit(&_active, (uintptr_t)(__bridge void *)recording, memory_order_release);
    return YES;
}

- (nullable MinatoLayerRecording *)stopRecording:(NSTimeInterval)timeout {
    const uintptr_t active = atomic_exchange(&_active, 0);
    if (active == 0) return nil;
    MinatoLayerRecording *recording = (__bridge MinatoLayerRecording *)(void *)active;
    atomic_store(&recording->_stopped, 1);
    // A call under way may wait up to a second in CAMetalLayer's nextDrawable.
    while (atomic_load(&recording->_inside) != 0) usleep(100);
    const int64_t deadline = now_ns() + (timeout > 0 ? (int64_t)llround(timeout * 1e9) : 0);
    while (recording.unanswered != 0 && now_ns() < deadline) usleep(1000);
    return recording;
}

@end
