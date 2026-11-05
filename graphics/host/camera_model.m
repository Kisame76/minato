/* graphics/host's model cameras (ADR-0073 decision 10), which apk-run on the Mac and the Simulator give the runtime
   (apkrt_set_cameras, whose contract they keep) in place of the iPhone's AVFoundation cameras, so that the guest's
   camera code runs where there is no camera:
   - "0" back (facing 1, orientation 90, a 4032 x 3024 array, autofocus and a flash) and "1" front (facing 0,
     orientation 270, 3088 x 2316, fixed focus, no flash), each with YUV and PRIVATE streams of 640 x 480, 1280 x 720
     and 1920 x 1080 at 15 to 30 fps and a JPEG of its array's size, zoom 1 to 8 (back) or 1 to 4 (front);
   - frames made when the guest asks for one: a request's capture is due at the request's frame rate (fps_max) after
     the one before, a single capture at once and before the repeating one's; each is NV12 ('420v', IOSurface-backed)
     color bars with a white block that moves 16 pixels a frame, so that a test can check their order and content,
     and a JPEG of the same picture (ImageIO) for a JPEG stream, at the JPEG quality asked;
   - its timestamp is the capture's time on CLOCK_UPTIME_RAW; the exposure 10 ms at ISO 100, the frame duration the
     rate's; after an AF trigger the FOCUSING bit for 3 frames, then FOCUSED; after an AE precapture trigger EXPOSING
     for 3 frames; a fixed-focus camera is always FOCUSED;
   - at most 4 captured frames wait for peek; an older one goes, its tokens ended. A frame's PRIVATE and YUV tokens
     wait for render, render_window or discard, which draw through video_frames.h as the decoders do.
   It runs on the guest's thread inside its syscall, as the decoders do, and never throws. */
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <pthread.h>
#include <string.h>
#include <time.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "minato_graphics.h"
#include "video_frames.h"

enum { linux_eio = 5, linux_eagain = 11, linux_enomem = 12, linux_ebusy = 16, linux_einval = 22 };

static const uint32_t max_captured = 4;
static const uint32_t settle_frames = 3;

struct model {
    int32_t facing, orientation, width, height, flash, autofocus;
    float zoom_max;
};

static const struct model models[2] = {
    {1, 90, 4032, 3024, 1, 1, 8.0f},
    {0, 270, 3088, 2316, 0, 0, 4.0f},
};

/* A captured frame: the guest's description, its images by stream, its JPEG. */
@interface MinatoModelFrame : NSObject {
@public
    struct apkrt_camera_frame frame;
    CVPixelBufferRef images[4];
    NSData *jpeg;
}
@end

@implementation MinatoModelFrame
- (void)dealloc {
    for (int n = 0; n < 4; n++) {
        if (images[n] != NULL) CVPixelBufferRelease(images[n]);
    }
}
@end

@interface MinatoModelCamera : NSObject {
@public
    uint32_t index;
    struct apkrt_camera_stream streams[APKRT_CAMERA_MAX_STREAMS];
    uint32_t stream_count;
    BOOL repeating;
    struct apkrt_camera_controls repeating_controls;
    uint64_t repeating_number;
    uint32_t repeating_targets;
    NSMutableArray<NSValue *> *singles;  // struct single, waiting captures
    NSMutableArray<MinatoModelFrame *> *captured;
    NSMutableDictionary<NSNumber *, NSValue *> *waiting;  // token to its retained CVPixelBufferRef
    uint64_t frames, next_token;
    uint64_t due_ns;  // the repeating request's next capture, on CLOCK_UPTIME_RAW
    uint32_t focusing, exposing;
    VTPixelTransferSessionRef transfer;
    uint64_t cropped_window;
}
@end

@implementation MinatoModelCamera
- (void)dealloc {
    for (NSValue *value in waiting.allValues) CVPixelBufferRelease((CVPixelBufferRef)value.pointerValue);
    if (transfer != NULL) {
        VTPixelTransferSessionInvalidate(transfer);
        CFRelease(transfer);
    }
}
@end

struct single {
    struct apkrt_camera_controls controls;
    uint64_t number;
    uint32_t targets;
};

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, MinatoModelCamera *> *cameras;
static uint64_t last_handle;
static BOOL camera_open[2];

static MinatoModelCamera *find_camera(uint64_t handle) {
    pthread_mutex_lock(&state_lock);
    MinatoModelCamera *camera = cameras[@(handle)];
    pthread_mutex_unlock(&state_lock);
    return camera;
}

static uint32_t count(void *context) {
    (void)context;
    return 2;
}

/* The facts' list as guest_interface.h has it. */
static void add(NSMutableData *list, uint32_t key, uint32_t type, const void *values, uint32_t count_of) {
    const uint32_t header[3] = {key, type, count_of};
    [list appendBytes:header length:sizeof header];
    [list appendBytes:values length:(NSUInteger)count_of * (type == APKRT_CAMERA_TYPE_I64 ? 8 : 4)];
    while (list.length % 8 != 0) [list appendBytes:"" length:1];
}

static NSData *facts_of(uint32_t index) {
    const struct model *model = &models[index];
    NSMutableData *list = [NSMutableData data];
    add(list, APKRT_CAMERA_KEY_FACING, APKRT_CAMERA_TYPE_I32, &model->facing, 1);
    add(list, APKRT_CAMERA_KEY_ORIENTATION, APKRT_CAMERA_TYPE_I32, &model->orientation, 1);
    const int32_t array[2] = {model->width, model->height};
    add(list, APKRT_CAMERA_KEY_PIXEL_ARRAY, APKRT_CAMERA_TYPE_I32, array, 2);
    const int32_t active[4] = {0, 0, model->width, model->height};
    add(list, APKRT_CAMERA_KEY_ACTIVE_ARRAY, APKRT_CAMERA_TYPE_I32, active, 4);
    const float physical[2] = {5.6f, 4.2f}, focal = 4.3f, aperture = 1.8f;
    add(list, APKRT_CAMERA_KEY_PHYSICAL_SIZE, APKRT_CAMERA_TYPE_F32, physical, 2);
    add(list, APKRT_CAMERA_KEY_FOCAL_LENGTHS, APKRT_CAMERA_TYPE_F32, &focal, 1);
    add(list, APKRT_CAMERA_KEY_APERTURES, APKRT_CAMERA_TYPE_F32, &aperture, 1);
    const float min_focus = model->autofocus ? 10.0f : 0.0f;
    add(list, APKRT_CAMERA_KEY_MIN_FOCUS_DISTANCE, APKRT_CAMERA_TYPE_F32, &min_focus, 1);
    add(list, APKRT_CAMERA_KEY_FLASH, APKRT_CAMERA_TYPE_I32, &model->flash, 1);
    const int32_t compensation[4] = {-12, 12, 1, 6};
    add(list, APKRT_CAMERA_KEY_AE_COMPENSATION, APKRT_CAMERA_TYPE_I32, compensation, 4);
    const float zoom[2] = {1.0f, model->zoom_max};
    add(list, APKRT_CAMERA_KEY_ZOOM, APKRT_CAMERA_TYPE_F32, zoom, 2);
    const int32_t fps[4] = {15, 30, 30, 30};
    add(list, APKRT_CAMERA_KEY_FPS_RANGES, APKRT_CAMERA_TYPE_I32, fps, 4);
    const int32_t iso[2] = {100, 1600};
    add(list, APKRT_CAMERA_KEY_ISO, APKRT_CAMERA_TYPE_I32, iso, 2);
    const int64_t exposure[2] = {100000, 66666666};
    add(list, APKRT_CAMERA_KEY_EXPOSURE_NS, APKRT_CAMERA_TYPE_I64, exposure, 2);
    const int32_t af_auto[3] = {0, 1, 4}, af_fixed[1] = {0};
    add(list, APKRT_CAMERA_KEY_AF_MODES, APKRT_CAMERA_TYPE_I32, model->autofocus ? af_auto : af_fixed,
        model->autofocus ? 3 : 1);
    const int32_t awb = 1, faces = 0;
    add(list, APKRT_CAMERA_KEY_AWB_MODES, APKRT_CAMERA_TYPE_I32, &awb, 1);
    add(list, APKRT_CAMERA_KEY_FACES, APKRT_CAMERA_TYPE_I32, &faces, 1);
    const int64_t frame_ns = 33333333;
    int64_t streams[4 * 4];
    const int64_t sizes[3][2] = {{640, 480}, {1280, 720}, {1920, 1080}};
    for (int n = 0; n < 3; n++) {
        streams[4 * n] = APKRT_CAMERA_FORMAT_YUV_420_888;
        streams[4 * n + 1] = sizes[n][0], streams[4 * n + 2] = sizes[n][1], streams[4 * n + 3] = frame_ns;
    }
    streams[12] = APKRT_CAMERA_FORMAT_JPEG, streams[13] = model->width, streams[14] = model->height;
    streams[15] = frame_ns;
    add(list, APKRT_CAMERA_KEY_STREAMS, APKRT_CAMERA_TYPE_I64, streams, 16);
    const int64_t stalls[4] = {APKRT_CAMERA_FORMAT_JPEG, model->width, model->height, 200000000};
    add(list, APKRT_CAMERA_KEY_STALLS, APKRT_CAMERA_TYPE_I64, stalls, 4);
    return list;
}

static int facts(void *context, uint32_t camera, void *data, size_t capacity, size_t *size) {
    (void)context;
    if (camera >= 2) return -linux_einval;
    @autoreleasepool {
        NSData *list = facts_of(camera);
        *size = list.length;
        if (capacity < list.length) return -28;  // ENOSPC
        memcpy(data, list.bytes, list.length);
        return 0;
    }
}

static int open_camera(void *context, uint32_t index, uint64_t *handle) {
    (void)context;
    if (index >= 2) return -linux_einval;
    MinatoModelCamera *camera = [MinatoModelCamera new];
    if (camera == nil) return -linux_enomem;
    camera->index = index;
    camera->singles = [NSMutableArray array];
    camera->captured = [NSMutableArray array];
    camera->waiting = [NSMutableDictionary dictionary];
    pthread_mutex_lock(&state_lock);
    if (camera_open[index]) {
        pthread_mutex_unlock(&state_lock);
        return -linux_ebusy;
    }
    if (cameras == nil) cameras = [NSMutableDictionary dictionary];
    *handle = ++last_handle;
    cameras[@(*handle)] = camera;
    camera_open[index] = YES;
    pthread_mutex_unlock(&state_lock);
    return 0;
}

static void end_token(MinatoModelCamera *camera, uint64_t token) {
    NSValue *value = camera->waiting[@(token)];
    if (value == nil) return;
    CVPixelBufferRelease((CVPixelBufferRef)value.pointerValue);
    [camera->waiting removeObjectForKey:@(token)];
}

static int configure(void *context, uint64_t handle, const struct apkrt_camera_stream *streams, uint32_t count_of) {
    (void)context;
    MinatoModelCamera *camera = find_camera(handle);
    if (camera == nil || count_of > APKRT_CAMERA_MAX_STREAMS) return -linux_einval;
    memcpy(camera->streams, streams, count_of * sizeof *streams);
    camera->stream_count = count_of;
    camera->repeating = NO;
    [camera->singles removeAllObjects];
    [camera->captured removeAllObjects];
    return 0;
}

static int request(void *context, uint64_t handle, const struct apkrt_camera_controls *controls, uint64_t number,
                   uint32_t targets, uint32_t flags) {
    (void)context;
    MinatoModelCamera *camera = find_camera(handle);
    if (camera == nil) return -linux_einval;
    if (controls->af_trigger == 1) camera->focusing = settle_frames;  // CONTROL_AF_TRIGGER_START
    if (controls->ae_precapture == 1) camera->exposing = settle_frames;
    if ((flags & APKRT_CAMERA_REPEATING) != 0) {
        camera->repeating = YES;
        camera->repeating_controls = *controls;
        camera->repeating_number = number;
        camera->repeating_targets = targets;
        return 0;
    }
    if (camera->singles.count >= APKRT_CAMERA_MAX_WAITING) return -linux_eagain;
    const struct single single = {*controls, number, targets};
    [camera->singles addObject:[NSValue valueWithBytes:&single objCType:@encode(struct single)]];
    return 0;
}

static void stop_repeating(void *context, uint64_t handle) {
    (void)context;
    MinatoModelCamera *camera = find_camera(handle);
    if (camera != nil) camera->repeating = NO;
}

/* The bars' colors, as Y, Cb, Cr: white, yellow, cyan, green, magenta, red, blue, black. */
static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                   {106, 202, 222}, {81, 90, 240},  {41, 240, 110}, {16, 128, 128}};

/* The frame's picture into an NV12 image of the stream's size: bars, and a white block at 16 x frame. */
static CVPixelBufferRef pattern(uint32_t width, uint32_t height, uint64_t frame) {
    NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    CVPixelBufferRef image = NULL;
    if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                            (__bridge CFDictionaryRef)attributes, &image) != kCVReturnSuccess) {
        return NULL;
    }
    CVPixelBufferLockBaseAddress(image, 0);
    uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(image, 0);
    uint8_t *chroma = CVPixelBufferGetBaseAddressOfPlane(image, 1);
    const size_t luma_row = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    const size_t chroma_row = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    const uint32_t block = height / 8, left = (uint32_t)((frame * 16) % (width > block ? width - block : 1));
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const BOOL in_block = x >= left && x < left + block && y >= block && y < 2 * block;
            luma[y * luma_row + x] = in_block ? 235 : bars[x * 8 / width][0];
        }
    }
    for (uint32_t y = 0; y < (height + 1) / 2; y++) {
        for (uint32_t x = 0; x < (width + 1) / 2; x++) {
            const BOOL in_block = 2 * x >= left && 2 * x < left + block && 2 * y >= block && 2 * y < 2 * block;
            const uint8_t *color = bars[2 * x * 8 / width];
            chroma[y * chroma_row + 2 * x] = in_block ? 128 : color[1];
            chroma[y * chroma_row + 2 * x + 1] = in_block ? 128 : color[2];
        }
    }
    CVPixelBufferUnlockBaseAddress(image, 0);
    return image;
}

/* A JPEG of the frame's picture at the stream's size and the quality asked (1 to 100). */
static NSData *jpeg_of(uint32_t width, uint32_t height, uint64_t frame, uint8_t quality) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context =
        CGBitmapContextCreate(NULL, width, height, 8, 0, space, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGColorSpaceRelease(space);
    if (context == NULL) return nil;
    const CGFloat rgb[8][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}};
    for (int n = 0; n < 8; n++) {
        CGContextSetRGBFillColor(context, rgb[n][0], rgb[n][1], rgb[n][2], 1);
        CGContextFillRect(context, CGRectMake((CGFloat)n * width / 8, 0, (CGFloat)width / 8 + 1, height));
    }
    const uint32_t block = height / 8, left = (uint32_t)((frame * 16) % (width > block ? width - block : 1));
    CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    CGContextFillRect(context, CGRectMake(left, height - 2 * block, block, block));  // CG's y runs up
    CGImageRef picture = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    if (picture == NULL) return nil;
    NSMutableData *bytes = [NSMutableData data];
    CGImageDestinationRef destination =
            CGImageDestinationCreateWithData((__bridge CFMutableDataRef)bytes, CFSTR("public.jpeg"), 1, NULL);
    if (destination != NULL) {
        NSDictionary *options = @{(id)kCGImageDestinationLossyCompressionQuality : @(quality / 100.0)};
        CGImageDestinationAddImage(destination, picture, (__bridge CFDictionaryRef)options);
        if (!CGImageDestinationFinalize(destination)) bytes = nil;
        CFRelease(destination);
    }
    CGImageRelease(picture);
    return bytes;
}

static uint64_t uptime_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* One capture of `targets` with `controls` for request `number`, into the captured frames. */
static BOOL capture(MinatoModelCamera *camera, const struct apkrt_camera_controls *controls, uint64_t number,
                    uint32_t targets) {
    MinatoModelFrame *frame = [MinatoModelFrame new];
    if (frame == nil) return NO;
    const uint64_t now = uptime_ns();
    const int32_t fps = controls->fps_max > 0 ? controls->fps_max : 30;
    frame->frame.number = number;
    frame->frame.frame = camera->frames;
    frame->frame.timestamp_ns = (int64_t)now;
    frame->frame.exposure_ns = 10000000;
    frame->frame.frame_duration_ns = 1000000000 / fps;
    frame->frame.iso = 100;
    frame->frame.zoom_ratio = controls->zoom_ratio;
    const BOOL autofocus = models[camera->index].autofocus != 0;
    if (camera->focusing > 0) {
        camera->focusing--;
        frame->frame.states |= APKRT_CAMERA_STATE_FOCUSING;
    } else {
        frame->frame.states |= APKRT_CAMERA_STATE_FOCUSED;
    }
    if (camera->exposing > 0) {
        camera->exposing--;
        frame->frame.states |= APKRT_CAMERA_STATE_EXPOSING;
    }
    if (models[camera->index].flash) frame->frame.states |= APKRT_CAMERA_STATE_FLASH_READY;
    frame->frame.focus_distance = autofocus ? 1.0f : 0.0f;
    for (uint32_t n = 0; n < camera->stream_count; n++) {
        if ((targets & (1u << n)) == 0) continue;
        const struct apkrt_camera_stream *stream = &camera->streams[n];
        if (stream->format == APKRT_CAMERA_FORMAT_JPEG) {
            frame->jpeg = jpeg_of(stream->width, stream->height, camera->frames, controls->jpeg_quality);
            if (frame->jpeg == nil) continue;  // the stream's buffer fails
        } else {
            frame->images[n] = pattern(stream->width, stream->height, camera->frames);
            if (frame->images[n] == NULL) continue;
            frame->frame.tokens[n] = ++camera->next_token;
        }
        frame->frame.streams |= 1u << n;
    }
    camera->frames++;
    if (camera->captured.count >= max_captured) [camera->captured removeObjectAtIndex:0];  // its images go with it
    [camera->captured addObject:frame];
    return YES;
}

/* The captures due now: the single ones at once, the repeating one at its rate. */
static void capture_due(MinatoModelCamera *camera) {
    while (camera->singles.count > 0) {
        struct single single;
        [camera->singles[0] getValue:&single];
        [camera->singles removeObjectAtIndex:0];
        capture(camera, &single.controls, single.number, single.targets);
    }
    if (!camera->repeating) return;
    const uint64_t now = uptime_ns();
    if (now < camera->due_ns) return;
    const int32_t fps = camera->repeating_controls.fps_max > 0 ? camera->repeating_controls.fps_max : 30;
    capture(camera, &camera->repeating_controls, camera->repeating_number, camera->repeating_targets);
    camera->due_ns = now + 1000000000ull / (uint64_t)fps;
}

static int peek(void *context, uint64_t handle, struct apkrt_camera_capture *out) {
    (void)context;
    @autoreleasepool {
        MinatoModelCamera *camera = find_camera(handle);
        if (camera == nil) return -linux_einval;
        capture_due(camera);
        if (camera->captured.count == 0) return -linux_eagain;
        MinatoModelFrame *frame = camera->captured[0];
        out->frame = frame->frame;
        out->jpeg = frame->jpeg.bytes;
        out->jpeg_size = frame->jpeg.length;
        return 0;
    }
}

static void pop(void *context, uint64_t handle) {
    (void)context;
    MinatoModelCamera *camera = find_camera(handle);
    if (camera == nil || camera->captured.count == 0) return;
    MinatoModelFrame *frame = camera->captured[0];
    for (int n = 0; n < 4; n++) {
        if (frame->images[n] == NULL) continue;
        camera->waiting[@(frame->frame.tokens[n])] = [NSValue valueWithPointer:frame->images[n]];
        frame->images[n] = NULL;  // the waiting entry holds it now
    }
    [camera->captured removeObjectAtIndex:0];
}

static CVPixelBufferRef take_waiting(MinatoModelCamera *camera, uint64_t token) {
    NSValue *value = camera->waiting[@(token)];
    if (value == nil) return NULL;
    [camera->waiting removeObjectForKey:@(token)];
    return (CVPixelBufferRef)value.pointerValue;
}

static int render(void *context, uint64_t handle, uint64_t token, const struct apkrt_video_target *target) {
    (void)context;
    @autoreleasepool {
        MinatoModelCamera *camera = find_camera(handle);
        CVPixelBufferRef image = camera != nil ? take_waiting(camera, token) : NULL;
        if (image == NULL) return -linux_einval;
        const int answer = minato_video_frame_render(image, target, &camera->transfer);
        CVPixelBufferRelease(image);
        return answer;
    }
}

static void discard(void *context, uint64_t handle, uint64_t token) {
    (void)context;
    MinatoModelCamera *camera = find_camera(handle);
    if (camera != nil) end_token(camera, token);
}

static int render_window(void *context, uint64_t handle, uint64_t token, uint64_t window, int64_t time_ns,
                         uint32_t flags) {
    (void)context;
    @autoreleasepool {
        MinatoModelCamera *camera = find_camera(handle);
        CVPixelBufferRef image = camera != nil ? take_waiting(camera, token) : NULL;
        if (image == NULL) return -linux_einval;
        const int answer = minato_video_frame_render_window(image, window, time_ns, flags, &camera->cropped_window);
        CVPixelBufferRelease(image);
        return answer;
    }
}

static int flush(void *context, uint64_t handle) {
    (void)context;
    MinatoModelCamera *camera = find_camera(handle);
    if (camera == nil) return -linux_einval;
    [camera->singles removeAllObjects];
    [camera->captured removeAllObjects];
    for (NSNumber *token in camera->waiting.allKeys) end_token(camera, token.unsignedLongLongValue);
    return 0;
}

static void close_camera(void *context, uint64_t handle) {
    (void)context;
    MinatoModelCamera *camera = find_camera(handle);
    if (camera == nil) return;
    if (camera->cropped_window != 0) minato_video_frame_give_back_gravity(camera->cropped_window);
    pthread_mutex_lock(&state_lock);
    [cameras removeObjectForKey:@(handle)];
    camera_open[camera->index] = NO;
    pthread_mutex_unlock(&state_lock);
}

static const struct apkrt_cameras functions = {count,    facts,  open_camera, configure, request,
                                               stop_repeating,   peek,        pop,       render,
                                               discard,  render_window, flush,  close_camera, NULL};

const struct apkrt_cameras *minato_graphics_camera_model(void) {
    return &functions;
}

int minato_graphics_set_camera_model(void) {
    return apkrt_set_cameras(&functions) == 0 ? 0 : -1;
}
