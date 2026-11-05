// graphics/host's layer pair for the upscaler (ADR-0067, decision 4), minato_upscale.h. The guest's low layer hooks its
// drawables' presents; the hook only hands the frame to a serial queue of the host's, which takes a drawable of the
// visible layer (that may wait: B1's spike showed a nextDrawable inside the scheduled handler deadlocking the
// producer's queue), encodes the pass on the producer's own queue behind the frame and presents the visible drawable.
//
// The low layer is a plain CAMetalLayer subclass of this file's, not layer.m's MinatoMetalLayer: that one swaps its
// drawables' class for the pacing and the recorder, and two swaps of one drawable would collide. When the pair sits
// in the real window, the observer of the low layer's presents moves into layer.m (ADR-0067 step 2).
#import "minato_upscale.h"

#import <objc/runtime.h>

// The iOS Simulator's SDK has no MetalFX: there the pair always draws bilinearly (the pass's fallback).
#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#define MINATO_HAS_METALFX 1
#else
#define MINATO_HAS_METALFX 0
#endif

#include <dlfcn.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static int64_t now_ns(void) {
    return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

@class MinatoUpscalePair;

@interface MinatoLowLayer : CAMetalLayer
@property(weak) MinatoUpscalePair *pair;
@end

@interface MinatoUpscalePair ()
- (void)receive:(id<CAMetalDrawable>)low;
@end

#pragma mark - The hooked drawable class

// One subclass of each concrete drawable class, made once under a lock: present, presentAtTime: and
// presentAfterMinimumDuration: hand the drawable to the pair instead of Core Animation (the low layer is never shown),
// and none of them calls the original.
static void hooked(id self) {
    CAMetalLayer *layer = ((id<CAMetalDrawable>)self).layer;
    if (![layer isKindOfClass:MinatoLowLayer.class]) return;
    [((MinatoLowLayer *)layer).pair receive:self];
}

static void hooked_present(id self, SEL command) {
    (void)command;
    hooked(self);
}

static void hooked_present_at(id self, SEL command, CFTimeInterval time) {
    (void)command;
    (void)time;
    hooked(self);
}

static void hooked_present_after(id self, SEL command, CFTimeInterval duration) {
    (void)command;
    (void)duration;
    hooked(self);
}

static os_unfair_lock hooked_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSString *, NSValue *> *hooked_classes;

static Class hooked_class_of(Class concrete) {
    os_unfair_lock_lock(&hooked_lock);
    if (hooked_classes == nil) hooked_classes = [NSMutableDictionary new];
    NSString *key = NSStringFromClass(concrete);
    Class made = (Class)[hooked_classes[key] pointerValue];
    if (made == Nil) {
        NSString *name = [NSString stringWithFormat:@"MinatoUpscaleDrawable_%@", key];
        made = objc_allocateClassPair(concrete, name.UTF8String, 0);
        if (made != Nil) {
            const struct {
                SEL selector;
                IMP implementation;
            } methods[] = {{@selector(present), (IMP)hooked_present},
                           {@selector(presentAtTime:), (IMP)hooked_present_at},
                           {@selector(presentAfterMinimumDuration:), (IMP)hooked_present_after}};
            // Each present the class has is hooked; a class without any is left alone, and its frames then go to Core
            // Animation as they come (a missing one of the three must not switch the whole hook off).
            int hooked_count = 0;
            for (size_t index = 0; index < sizeof methods / sizeof *methods; index++) {
                const Method method = class_getInstanceMethod(concrete, methods[index].selector);
                if (method != NULL && class_addMethod(made, methods[index].selector, methods[index].implementation,
                                                      method_getTypeEncoding(method))) {
                    hooked_count++;
                }
            }
            if (hooked_count > 0) {
                objc_registerClassPair(made);
                hooked_classes[key] = [NSValue valueWithPointer:(void *)made];
            } else {
                fprintf(stderr, "MinatoUpscalePair: %s has no present method; its frames are not hooked\n",
                        class_getName(concrete));
                objc_disposeClassPair(made);
                made = Nil;
            }
        }
    }
    os_unfair_lock_unlock(&hooked_lock);
    return made;
}

@implementation MinatoLowLayer
- (nullable id<CAMetalDrawable>)nextDrawable {
    id<CAMetalDrawable> drawable = [super nextDrawable];
    if (drawable == nil) return nil;
    Class concrete = object_getClass(drawable);
    if (![NSStringFromClass(concrete) hasPrefix:@"MinatoUpscaleDrawable_"]) {
        const Class made = hooked_class_of(concrete);
        if (made != Nil) object_setClass(drawable, made);
    }
    return drawable;
}
@end

#pragma mark - The bilinear pass

static NSString *const fallback_source = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 position [[position]]; float2 uv; };\n"
    "vertex V upscale_vertex(uint i [[vertex_id]]) {\n"
    "    float2 p = float2((i << 1) & 2, i & 2);\n"
    "    V out; out.position = float4(p * 2.0 - 1.0, 0.0, 1.0); out.uv = float2(p.x, 1.0 - p.y); return out;\n"
    "}\n"
    "fragment float4 upscale_fragment(V in [[stage_in]], texture2d<float> source [[texture(0)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    return source.sample(s, in.uv);\n"
    "}\n";

#pragma mark - The pair

@implementation MinatoUpscalePair {
    CAMetalLayer *_visible;
    MinatoLowLayer *_low;
    id<MTLCommandQueue> _producer;
    id<MTLDevice> _device;
    dispatch_queue_t _queue;
    _Atomic(BOOL) _spatial;
    // Only the serial queue touches these.
#if MINATO_HAS_METALFX
    id<MTLFXSpatialScaler> _scaler;
#endif
    NSUInteger _scaler_in_width, _scaler_in_height, _scaler_out_width, _scaler_out_height;
    id<MTLRenderPipelineState> _pipeline;
    _Atomic(uint64_t) _low_presents, _visible_presents, _completed, _dropped, _handed_over;
    // The frame that waits for the serial queue, which the next one replaces (under _waiting_lock).
    os_unfair_lock _waiting_lock;
    id<CAMetalDrawable> _waiting;
    uint64_t _waiting_frame;
    BOOL _queued;
    _Atomic(int64_t) _longest_hook, _last_gpu;
}

+ (BOOL)supportsDevice:(id<MTLDevice>)device {
#if MINATO_HAS_METALFX
    // Loaded by name when first asked, so that no build links the framework (and an OS without it just says no).
    static dispatch_once_t loaded;
    dispatch_once(&loaded, ^{
        dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY);
    });
    Class descriptor = NSClassFromString(@"MTLFXSpatialScalerDescriptor");
    return descriptor != Nil && [descriptor supportsDevice:device];
#else
    (void)device;
    return NO;
#endif
}

- (nullable instancetype)initWithVisibleLayer:(CAMetalLayer *)visible
                                      lowSize:(CGSize)low_size
                                producerQueue:(id<MTLCommandQueue>)producer
                                spatialScaler:(BOOL)spatial {
    if ((self = [super init]) == nil) return nil;
    if (visible.device == nil || low_size.width < 1 || low_size.height < 1 || producer == nil) return nil;
    _waiting_lock = OS_UNFAIR_LOCK_INIT;
    _visible = visible;
    _device = visible.device;
    _producer = producer;
    atomic_store(&_spatial, spatial && [MinatoUpscalePair supportsDevice:_device]);
    // The scaler writes into the visible drawables, and the observer reads them.
    visible.framebufferOnly = NO;
    _low = [MinatoLowLayer layer];
    _low.pair = self;
    _low.device = _device;
    _low.pixelFormat = visible.pixelFormat;
    _low.drawableSize = low_size;
    _low.framebufferOnly = NO;
    _low.maximumDrawableCount = 3;
    _low.opaque = YES;
    dispatch_queue_attr_t attributes =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    _queue = dispatch_queue_create("minato.upscale", attributes);
    return self;
}

- (CAMetalLayer *)lowLayer {
    return _low;
}

- (BOOL)usesSpatialScaler {
    return atomic_load(&_spatial);
}

- (uint64_t)lowPresents {
    return atomic_load(&_low_presents);
}

- (uint64_t)visiblePresents {
    return atomic_load(&_visible_presents);
}

- (uint64_t)completedPasses {
    return atomic_load(&_completed);
}

- (uint64_t)droppedFrames {
    return atomic_load(&_dropped);
}

- (int64_t)lastPassGpuNs {
    return atomic_load(&_last_gpu);
}

- (int64_t)longestHookNs {
    return atomic_load(&_longest_hook);
}

- (BOOL)waitIdle:(NSTimeInterval)timeout {
    const int64_t until = now_ns() + (int64_t)(timeout * 1e9);
    while (atomic_load(&_completed) + atomic_load(&_dropped) < atomic_load(&_handed_over)) {
        if (now_ns() > until) return NO;
        usleep(500);
    }
    return YES;
}

/// On the host's serial queue: the waiting frames, one after the other, until none is left.
- (void)drain {
    while (YES) {
        os_unfair_lock_lock(&_waiting_lock);
        id<CAMetalDrawable> low = _waiting;
        const uint64_t frame = _waiting_frame;
        _waiting = nil;
        if (low == nil) _queued = NO;
        os_unfair_lock_unlock(&_waiting_lock);
        if (low == nil) return;
        [self pass:low frame:frame];
    }
}

/// The hook, on the producer's scheduled-handler thread (or the thread of a direct present): hand over, never wait.
- (void)receive:(id<CAMetalDrawable>)low {
    const int64_t start = now_ns();
    const uint64_t frame = atomic_fetch_add(&_low_presents, 1);
    atomic_fetch_add(&_handed_over, 1);
    // The newest frame is the one that waits: a frame still waiting is let go (its drawable goes back to the guest's
    // pool) and counts as dropped. The queue takes whatever waits when it gets to it.
    os_unfair_lock_lock(&_waiting_lock);
    id<CAMetalDrawable> replaced = _waiting;
    _waiting = low;
    _waiting_frame = frame;
    const BOOL start_queue = !_queued;
    _queued = YES;
    os_unfair_lock_unlock(&_waiting_lock);
    if (replaced != nil) atomic_fetch_add(&_dropped, 1);
    replaced = nil;
    if (start_queue) {
        dispatch_async(_queue, ^{
            [self drain];
        });
    }
    const int64_t took = now_ns() - start;
    int64_t seen = atomic_load(&_longest_hook);
    while (took > seen && !atomic_compare_exchange_weak(&_longest_hook, &seen, took)) {
    }
}

#if MINATO_HAS_METALFX
- (void)makeScalerForInput:(id<MTLTexture>)input output:(id<MTLTexture>)output {
    if (_scaler != nil && _scaler_in_width == input.width && _scaler_in_height == input.height &&
        _scaler_out_width == output.width && _scaler_out_height == output.height) {
        return;
    }
    MTLFXSpatialScalerDescriptor *descriptor = [NSClassFromString(@"MTLFXSpatialScalerDescriptor") new];
    descriptor.inputWidth = input.width;
    descriptor.inputHeight = input.height;
    descriptor.outputWidth = output.width;
    descriptor.outputHeight = output.height;
    descriptor.colorTextureFormat = input.pixelFormat;
    descriptor.outputTextureFormat = output.pixelFormat;
    descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
    _scaler = [descriptor newSpatialScalerWithDevice:_device];
    // The scaler reads the low texture and writes the visible one: their usages must hold what it needs (the drawables'
    // usages can differ from the Mac's, and an encode with the wrong ones is a Metal validation failure or black).
    if (_scaler != nil && ((_scaler.colorTextureUsage & input.usage) != _scaler.colorTextureUsage ||
                           (_scaler.outputTextureUsage & output.usage) != _scaler.outputTextureUsage)) {
        fprintf(stderr,
                "MinatoUpscalePair: the drawables' texture usages (%lu, %lu) do not hold the scaler's (%lu, %lu); "
                "the passes draw bilinearly\n",
                (unsigned long)input.usage, (unsigned long)output.usage, (unsigned long)_scaler.colorTextureUsage,
                (unsigned long)_scaler.outputTextureUsage);
        _scaler = nil;
    }
    if (_scaler == nil) atomic_store(&_spatial, NO);
    _scaler_in_width = input.width;
    _scaler_in_height = input.height;
    _scaler_out_width = output.width;
    _scaler_out_height = output.height;
}
#endif

- (BOOL)makePipeline:(MTLPixelFormat)format {
    if (_pipeline != nil) return YES;
    NSError *error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:fallback_source options:nil error:&error];
    if (library == nil) {
        fprintf(stderr, "MinatoUpscalePair: the bilinear pass does not compile: %s\n",
                error.localizedDescription.UTF8String);
        return NO;
    }
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"upscale_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"upscale_fragment"];
    descriptor.colorAttachments[0].pixelFormat = format;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    return _pipeline != nil;
}

/// On the host's serial queue: take the visible layer's drawable (it may wait), and commit the pass on the producer's
/// queue, behind the frame that is presented. `low` is held until the pass completed, so that its texture does not go
/// back to the layer's pool while the pass reads it.
- (void)pass:(id<CAMetalDrawable>)low frame:(uint64_t)frame {
    id<CAMetalDrawable> visible = [_visible nextDrawable];
    if (visible == nil) {
        atomic_fetch_add(&_dropped, 1);
        return;
    }
    id<MTLCommandBuffer> buffer = [_producer commandBuffer];
    BOOL encoded = NO;
#if MINATO_HAS_METALFX
    if (atomic_load(&_spatial)) {
        [self makeScalerForInput:low.texture output:visible.texture];
        if (_scaler != nil) {
            _scaler.colorTexture = low.texture;
            _scaler.outputTexture = visible.texture;
            _scaler.inputContentWidth = low.texture.width;
            _scaler.inputContentHeight = low.texture.height;
            [_scaler encodeToCommandBuffer:buffer];
            encoded = YES;
        }
    }
#endif
    if (!encoded && [self makePipeline:visible.texture.pixelFormat]) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = visible.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:_pipeline];
        [encoder setFragmentTexture:low.texture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        encoded = YES;
    }
    MinatoUpscaleObserver observer = self.observer;
    id<MTLBuffer> readback = nil;
    const size_t row_bytes = visible.texture.width * 4;
    if (encoded && observer != nil) {
        readback = [_device newBufferWithLength:row_bytes * visible.texture.height
                                        options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
        [blit copyFromTexture:visible.texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(visible.texture.width, visible.texture.height, 1)
                     toBuffer:readback
            destinationOffset:0
       destinationBytesPerRow:row_bytes
     destinationBytesPerImage:row_bytes * visible.texture.height];
        [blit endEncoding];
    }
    if (!encoded) {
        atomic_fetch_add(&_dropped, 1);
        return;
    }
    [buffer presentDrawable:visible];
    [buffer addScheduledHandler:^(id<MTLCommandBuffer> scheduled) {
        (void)scheduled;
        atomic_fetch_add(&self->_visible_presents, 1);
    }];
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)low;  // held until here
        atomic_store(&self->_last_gpu, (int64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9));
        if (readback != nil) observer(frame, readback.contents, row_bytes);
        atomic_fetch_add(&self->_completed, 1);
    }];
    [buffer commit];
}

@end
