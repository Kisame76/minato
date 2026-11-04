// graphics/host's layer and the parity benchmark's recorder (ADR-0037, decision 2), minato_layer.h, and the presents
// on the layer's drawables (ADR-0041 decision 11 c), present_pace.h).
#import "minato_layer.h"

#import <QuartzCore/CATransaction.h>
#import <objc/runtime.h>

#include <TargetConditionals.h>
#include <math.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"
#include "present_pace.h"
#include "present_time.h"

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

/// The records a layer keeps: the presents on their way, a few more than its 3 drawables. A full layer forgets its
/// oldest, which a present that never reached a drawable left.
#define RECORDS 16
/// The times asked for whose presents are not shown, noted by minato_graphics_drop_present.
#define DROPS 8
/// The waits of the layer's last drawables in nextDrawable: more than the drawables on their way.
#define WAITS 8

/// A layer's presents (present_pace.h), under `lock`.
struct pace {
    os_unfair_lock lock;
    struct minato_present_record records[RECORDS];
    uint64_t tickets[RECORDS];
    unsigned first, count;
    uint64_t last_ticket;
    int64_t drops[DROPS];
    unsigned drop_count;
    /// Decision 11 c)'s rule, for the presents with a record.
    struct minato_present_pace rule;
    struct {
        const void *drawable;
        int64_t waited;
    } waits[WAITS];
    unsigned next_wait;
};

@implementation MinatoMetalLayer {
@public
    /// The running recording, unretained; 0 outside a run, which is the one flag test.
    _Atomic(uintptr_t) _active;
    /// The last recording, which keeps the running one alive.
    MinatoLayerRecording *_current;
    /// Zeroed with the layer, which is an unlocked lock and no records.
    struct pace _pace;
    /// The window's buffers' sublayer shows (minato_present_buffers_shown), and a drawable went to Core Animation
    /// since the last call.
    _Atomic(int) _buffers_shown, _drawn;
}

static void pace_adopt(MinatoMetalLayer *layer, id<CAMetalDrawable> drawable, int64_t waited);

// The Simulator's MTLDrawable has no presented handlers, drawableID or presentedTime: there the layer records
// nothing, and -startRecording: returns NO.
- (nullable id<CAMetalDrawable>)nextDrawable {
    const int64_t asked = now_ns();
#if TARGET_OS_SIMULATOR
    id<CAMetalDrawable> drawable = [super nextDrawable];
#else
    const uintptr_t active = atomic_load_explicit(&_active, memory_order_acquire);
    id<CAMetalDrawable> drawable =
        active == 0 ? [super nextDrawable]
                    : record_next_drawable(self, (__bridge MinatoLayerRecording *)(void *)active,
                                           class_getMethodImplementation(CAMetalLayer.class, @selector(nextDrawable)));
#endif
    pace_adopt(self, drawable, now_ns() - asked);
    return drawable;
}

- (BOOL)startRecording:(NSUInteger)capacity {
    return start_recording(&_active, &_current, capacity);
}

- (nullable MinatoLayerRecording *)stopRecording:(NSTimeInterval)timeout {
    return stop_recording(&_active, timeout);
}

@end

// The presents on MinatoMetalLayer's drawables (ADR-0041 decision 11 c), present_pace.h): each drawable the layer gives
// out becomes graphics/host's subclass of its class, whose present and presentAtTime: take the layer's oldest record
// and then hand the drawable to Core Animation, or not, as decision 11 c)'s rule decides (present_time.h) from the time
// its app asked for, its wait in nextDrawable and whether its record is ruled. Metal's presentDrawable: and
// presentDrawable:atTime: reach them, and so does MoltenVK's present from its scheduled handler; neither ANGLE nor
// MoltenVK presents otherwise. A drawable not handed over goes back to the layer's pool once it is let go, and its
// presented handlers run then, with presentedTime 0; MoltenVK lets an image's drawable go when it acquires that image
// again. The subclass is made once, for the first drawable's class; a drawable of another class goes as it comes.
static Class drawable_class, paced_class;
static dispatch_once_t paced_once;

/// The layer as a MinatoMetalLayer, nil for any other.
static MinatoMetalLayer *_Nullable paced_layer(void *layer) {
    id object = (__bridge id)layer;
    return [object isKindOfClass:MinatoMetalLayer.class] ? object : nil;
}

uint64_t minato_present_push(void *layer, const struct minato_present_record *record) {
    MinatoMetalLayer *paced = paced_layer(layer);
    if (paced == nil) return 0;
    struct pace *pace = &paced->_pace;
    os_unfair_lock_lock(&pace->lock);
    if (pace->count == RECORDS) {
        pace->first = (pace->first + 1) % RECORDS;
        pace->count--;
    }
    const unsigned slot = (pace->first + pace->count++) % RECORDS;
    pace->records[slot] = *record;
    const uint64_t ticket = pace->tickets[slot] = ++pace->last_ticket;
    os_unfair_lock_unlock(&pace->lock);
    return ticket;
}

void minato_present_take_back(void *layer, uint64_t ticket) {
    MinatoMetalLayer *paced = paced_layer(layer);
    if (paced == nil || ticket == 0) return;
    struct pace *pace = &paced->_pace;
    os_unfair_lock_lock(&pace->lock);
    for (unsigned index = 0; index < pace->count; index++) {
        if (pace->tickets[(pace->first + index) % RECORDS] != ticket) continue;
        // The later records move up one place.
        for (unsigned later = index + 1; later < pace->count; later++) {
            const unsigned to = (pace->first + later - 1) % RECORDS, from = (pace->first + later) % RECORDS;
            pace->records[to] = pace->records[from];
            pace->tickets[to] = pace->tickets[from];
        }
        pace->count--;
        break;
    }
    os_unfair_lock_unlock(&pace->lock);
}

void minato_present_forget(void *layer) {
    MinatoMetalLayer *paced = paced_layer(layer);
    if (paced == nil) return;
    os_unfair_lock_lock(&paced->_pace.lock);
    paced->_pace.count = 0;
    os_unfair_lock_unlock(&paced->_pace.lock);
}

// Polls, as the layer's lock is one for short holds; a drawable's present takes the oldest record (pace_hands_over).
// Without the subclass no drawable takes one, and nothing is waited for.
int minato_present_wait(void *layer, uint64_t swapchain, int64_t deadline) {
    MinatoMetalLayer *paced = paced_layer(layer);
    if (paced == nil || paced_class == Nil) return 1;
    struct pace *pace = &paced->_pace;
    for (;;) {
        BOOL left = NO;
        os_unfair_lock_lock(&pace->lock);
        for (unsigned index = 0; !left && index < pace->count; index++) {
            left = pace->records[(pace->first + index) % RECORDS].swapchain == swapchain;
        }
        os_unfair_lock_unlock(&pace->lock);
        if (!left) return 1;
        if (now_ns() >= deadline) return 0;
        usleep(1000);
    }
}

void minato_present_unpaced(void *drawable) {
    id object = (__bridge id)drawable;
    if (object != nil && paced_class != Nil && object_getClass(object) == paced_class) {
        object_setClass(object, drawable_class);
    }
}

int minato_graphics_drop_present(uint64_t window, int64_t asked) {
    CAMetalLayer *layer = (__bridge_transfer CAMetalLayer *)apkrt_window_layer(window);
    MinatoMetalLayer *paced = paced_layer((__bridge void *)layer);
    if (paced == nil) return -1;
    struct pace *pace = &paced->_pace;
    os_unfair_lock_lock(&pace->lock);
    const BOOL noted = pace->drop_count < DROPS;
    if (noted) pace->drops[pace->drop_count++] = asked;
    os_unfair_lock_unlock(&pace->lock);
    return noted ? 0 : -1;
}

/// Under the lock: how long the drawable waited in nextDrawable, which the layer forgets then; 0 when it has none.
static int64_t wait_taken(struct pace *pace, id<CAMetalDrawable> drawable) {
    for (unsigned index = 0; index < WAITS; index++) {
        if (pace->waits[index].drawable == (__bridge const void *)drawable) {
            pace->waits[index].drawable = NULL;
            return pace->waits[index].waited;
        }
    }
    return 0;
}

/// Takes the oldest record of the drawable's layer: whether Core Animation gets the drawable, presented with `time`
/// (nanoseconds of the uptime clock, 0 for none). It does not when its time asked for was noted by
/// minato_graphics_drop_present, which forgets the time then, nor when decision 11 c)'s rule drops it.
static BOOL pace_hands_over(id<CAMetalDrawable> drawable, int64_t time) {
    MinatoMetalLayer *layer = paced_layer((__bridge void *)drawable.layer);
    if (layer == nil) return YES;
    struct pace *pace = &layer->_pace;
    struct minato_present_record record = {0};
    BOOL hands_over = YES;
    os_unfair_lock_lock(&pace->lock);
    const int64_t waited = wait_taken(pace, drawable);
    if (pace->count != 0) {
        record = pace->records[pace->first];
        pace->first = (pace->first + 1) % RECORDS;
        pace->count--;
        for (unsigned index = 0; record.asked != 0 && index < pace->drop_count; index++) {
            if (pace->drops[index] == record.asked) {
                pace->drops[index] = pace->drops[--pace->drop_count];
                hands_over = NO;
                break;
            }
        }
        if (hands_over) {
            hands_over = minato_present_paced(&pace->rule, record.ruled, record.asked, time, waited, now_ns()) != 0;
        }
    }
    os_unfair_lock_unlock(&pace->lock);
    if (!hands_over && record.dropped != NULL) record.dropped(&record);
    return hands_over;
}

int minato_present_buffers_shown(void *layer) {
    MinatoMetalLayer *paced = paced_layer(layer);
    if (paced == nil) return 0;
    atomic_store(&paced->_buffers_shown, 1);
    return atomic_exchange(&paced->_drawn, 0);
}

/// A drawable of the layer goes to Core Animation: the window's buffers' sublayer hides, if it shows.
static void drawn(id<CAMetalDrawable> drawable) {
    MinatoMetalLayer *layer = paced_layer((__bridge void *)drawable.layer);
    if (layer == nil) return;
    atomic_store(&layer->_drawn, 1);
    if (!atomic_exchange(&layer->_buffers_shown, 0)) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (CALayer *sublayer in layer.sublayers) {
        if ([sublayer.name isEqualToString:@"minato.window.buffers"]) sublayer.hidden = YES;
    }
    [CATransaction commit];
}

static void paced_present(id self, SEL command) {
    if (pace_hands_over(self, 0)) {
        drawn(self);
        ((void (*)(id, SEL))class_getMethodImplementation(drawable_class, command))(self, command);
    }
}

static void paced_present_at(id self, SEL command, CFTimeInterval time) {
    // Core Animation's host time in seconds, as CLOCK_UPTIME_RAW counts it.
    if (pace_hands_over(self, time > 0 ? (int64_t)llround(time * 1e9) : 0)) {
        drawn(self);
        const IMP present_at = class_getMethodImplementation(drawable_class, command);
        ((void (*)(id, SEL, CFTimeInterval))present_at)(self, command, time);
    }
}

static void make_paced_class(Class concrete) {
    Class made = objc_allocateClassPair(concrete, "MinatoPacedDrawable", 0);
    if (made == Nil) return;
    const struct {
        SEL selector;
        IMP implementation;
    } methods[] = {{@selector(present), (IMP)paced_present}, {@selector(presentAtTime:), (IMP)paced_present_at}};
    for (size_t index = 0; index < sizeof methods / sizeof *methods; index++) {
        const Method method = class_getInstanceMethod(concrete, methods[index].selector);
        if (method == NULL || !class_addMethod(made, methods[index].selector, methods[index].implementation,
                                               method_getTypeEncoding(method))) {
            fprintf(stderr, "MinatoMetalLayer: %s has no %s; its drawables go to Core Animation as they come\n",
                    class_getName(concrete), sel_getName(methods[index].selector));
            objc_disposeClassPair(made);
            return;
        }
    }
    objc_registerClassPair(made);
    drawable_class = concrete;
    paced_class = made;
}

/// The drawable becomes the paced class, and the layer keeps how long it waited in nextDrawable for the rule.
static void pace_adopt(MinatoMetalLayer *layer, id<CAMetalDrawable> drawable, int64_t waited) {
    if (drawable == nil) return;
    Class concrete = object_getClass(drawable);
    dispatch_once(&paced_once, ^{
        make_paced_class(concrete);
    });
    if (concrete != drawable_class) return;
    object_setClass(drawable, paced_class);
    struct pace *pace = &layer->_pace;
    os_unfair_lock_lock(&pace->lock);
    pace->waits[pace->next_wait].drawable = (__bridge const void *)drawable;
    pace->waits[pace->next_wait].waited = waited;
    pace->next_wait = (pace->next_wait + 1) % WAITS;
    os_unfair_lock_unlock(&pace->lock);
}

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
