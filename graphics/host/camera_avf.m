/* graphics/host's iPhone cameras (ADR-0073 decision 10): the device's own cameras through AVFoundation behind the
   runtime's camera functions (apkrt_set_cameras, whose contract they keep), and minato_graphics_set_cameras, which
   gives the runtime these or, where there is no camera (the Simulator, the Mac), the model cameras (camera_model.m).
   - Cameras: "0" the back camera (the triple, dual wide, dual or wide camera, the first the iPhone has, its ultra
     wide and tele reached by zoom ratios) and "1" the front wide camera; their facts by camera_rules.c from the
     AVCaptureDevice and its 8-bit YUV formats.
   - One running camera at a time (no concurrent cameras, ADR-0073 decision 1): an open takes the camera from the one
     open before, which is lost as below (cameraservice evicts a conflicting camera of the same app); an open answers
     -EACCES while iOS has not granted the camera (the app asks for it, ADR-0073 decision 8).
   - Configure: one AVCaptureSession with the device's format camera_rules.c chooses for the streams, an
     AVCaptureVideoDataOutput in the format's own pixel format (NV12, no conversion) for the PRIVATE and YUV streams,
     an AVCapturePhotoOutput for a JPEG stream at the still's size; the session runs from then on.
   - Requests: their controls applied to the device under lockForConfiguration when they change (focus, exposure,
     white balance, torch, zoom, frame rate), the AF trigger as one autofocus; a single request without a JPEG takes
     the next frame before the repeating one, one with a JPEG a photo (quality, flash, JPEG_ORIENTATION) whose frame
     carries the last video frame for its other streams. Each frame's 3A bits come from camera_rules.c's state
     machine on the device's isAdjusting* answers; its timestamp is the sample's on CLOCK_UPTIME_RAW.
   - Frames: at most max_captured wait for peek, an older one goes (never the one peek gave); a frame's PRIVATE and
     YUV tokens hold the sample's CVPixelBuffer until render, render_window or discard, which draw through
     video_frames.h as the decoders do.
   - Interruptions (ADR-0073 decision 9): once iOS interrupts the session (the background, another app, a runtime
     error), the camera is lost for the guest: peek, configure and request answer -ENODEV until its close, and the
     app opens the camera again, as on Android.
   The runtime's calls run on the guest's thread inside its syscall and may block on the host (startRunning,
   stopRunning); the captures come on a serial queue of the camera's, under its lock. Nothing throws. */
#import <TargetConditionals.h>

#include "minato_graphics.h"

#if TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <mach/mach_time.h>
#include <pthread.h>
#include <string.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "camera_rules.h"
#include "video_frames.h"

enum {
    linux_eio = 5,
    linux_eagain = 11,
    linux_enomem = 12,
    linux_eacces = 13,
    linux_enodev = 19,
    linux_einval = 22,
    linux_enospc = 28,
};

static const uint32_t max_captured = 3;

/* The cameras the iPhone has, found once: their devices, descriptions and formats by the descriptions' indices. */
static AVCaptureDevice *devices[2];
static struct minato_camera_device descriptions[2];
static NSArray<AVCaptureDeviceFormat *> *formats[2];
static uint32_t device_count;

static int64_t duration_ns(CMTime time) {
    return CMTIME_IS_NUMERIC(time) ? CMTimeConvertScale(time, 1000000000, kCMTimeRoundingMethod_Default).value : 0;
}

/* `device` into `description`; its formats by the description's indices. */
static NSArray<AVCaptureDeviceFormat *> *describe(AVCaptureDevice *device, int32_t facing,
                                                  struct minato_camera_device *description) {
    memset(description, 0, sizeof *description);
    description->facing = facing;
    description->autofocus = [device isFocusModeSupported:AVCaptureFocusModeAutoFocus] ? 1 : 0;
    description->flash = device.hasFlash ? 1 : 0;
    description->aperture = device.lensAperture;
    description->min_focus_mm = (float)device.minimumFocusDistance;
    description->bias_min = device.minExposureTargetBias;
    description->bias_max = device.maxExposureTargetBias;
    description->wide_factor = 1.0f;
    NSArray<AVCaptureDeviceType> *ultra = @[ AVCaptureDeviceTypeBuiltInTripleCamera,
                                             AVCaptureDeviceTypeBuiltInDualWideCamera ];
    if ([ultra containsObject:device.deviceType] && device.virtualDeviceSwitchOverVideoZoomFactors.count > 0) {
        description->wide_factor = device.virtualDeviceSwitchOverVideoZoomFactors.firstObject.floatValue;
    }
    NSMutableArray<AVCaptureDeviceFormat *> *list = [NSMutableArray array];
    for (AVCaptureDeviceFormat *format in device.formats) {
        if (list.count >= MINATO_CAMERA_MAX_FORMATS) break;
        const FourCharCode subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
        if (subtype != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
            subtype != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
            continue;
        }
        struct minato_camera_format *entry = &description->formats[list.count];
        const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
        entry->width = size.width, entry->height = size.height, entry->yuv = 1;
        for (AVFrameRateRange *range in format.videoSupportedFrameRateRanges) {
            if (range.maxFrameRate > entry->fps_max) entry->fps_max = (float)range.maxFrameRate;
        }
        entry->field_of_view = format.videoFieldOfView;
        entry->iso_min = format.minISO, entry->iso_max = format.maxISO;
        entry->exposure_min_ns = duration_ns(format.minExposureDuration);
        entry->exposure_max_ns = duration_ns(format.maxExposureDuration);
        entry->zoom_max = (float)format.videoMaxZoomFactor;
        for (NSValue *value in format.supportedMaxPhotoDimensions) {
            if (entry->photo_count >= MINATO_CAMERA_MAX_PHOTO_SIZES) break;
            const CMVideoDimensions photo = value.CMVideoDimensionsValue;
            entry->photo[entry->photo_count][0] = photo.width;
            entry->photo[entry->photo_count][1] = photo.height;
            entry->photo_count++;
        }
        [list addObject:format];
    }
    description->format_count = (uint32_t)list.count;
    return list;
}

static AVCaptureDevice *find(NSArray<AVCaptureDeviceType> *types, AVCaptureDevicePosition position) {
    AVCaptureDeviceDiscoverySession *discovery =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                               mediaType:AVMediaTypeVideo
                                                                position:position];
    for (AVCaptureDeviceType type in types) {  // in the order asked for
        for (AVCaptureDevice *device in discovery.devices) {
            if ([device.deviceType isEqualToString:type]) return device;
        }
    }
    return nil;
}

static void find_cameras(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      @autoreleasepool {
          AVCaptureDevice *back = find(@[ AVCaptureDeviceTypeBuiltInTripleCamera,
                                          AVCaptureDeviceTypeBuiltInDualWideCamera,
                                          AVCaptureDeviceTypeBuiltInDualCamera,
                                          AVCaptureDeviceTypeBuiltInWideAngleCamera ],
                                       AVCaptureDevicePositionBack);
          AVCaptureDevice *front = find(@[ AVCaptureDeviceTypeBuiltInWideAngleCamera ], AVCaptureDevicePositionFront);
          AVCaptureDevice *found[2] = {back, front};
          const int32_t facing[2] = {1, 0};
          for (int n = 0; n < 2; n++) {
              if (found[n] == nil) continue;
              formats[device_count] = describe(found[n], facing[n], &descriptions[device_count]);
              if (minato_camera_facts(&descriptions[device_count], NULL, 0) == 0) continue;  // nothing to run
              devices[device_count++] = found[n];
          }
      }
    });
}

/* A captured frame: the guest's description, its images by stream, its JPEG. */
@interface MinatoAVFFrame : NSObject {
@public
    struct apkrt_camera_frame frame;
    CVPixelBufferRef images[4];
    NSData *jpeg;
}
@end

@implementation MinatoAVFFrame
- (void)dealloc {
    for (int n = 0; n < 4; n++) {
        if (images[n] != NULL) CVPixelBufferRelease(images[n]);
    }
}
@end

struct single {
    struct apkrt_camera_controls controls;
    uint64_t number;
    uint32_t targets;
};

@interface MinatoAVFCamera : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate, AVCapturePhotoCaptureDelegate> {
@public
    uint32_t index;
    pthread_mutex_t lock;
    AVCaptureSession *session;
    AVCaptureDeviceInput *input;
    AVCaptureVideoDataOutput *video;
    AVCapturePhotoOutput *photo;
    dispatch_queue_t queue;
    NSArray<id> *observers;
    struct apkrt_camera_stream streams[APKRT_CAMERA_MAX_STREAMS];
    uint32_t stream_count, video_streams;  // video_streams: the PRIVATE and YUV streams' bits
    int32_t jpeg_stream;                   // its index, or -1
    int32_t still_width, still_height;
    BOOL repeating, lost, peeked, photo_busy;
    struct apkrt_camera_controls repeating_controls;
    uint64_t repeating_number;
    uint32_t repeating_targets;
    NSMutableArray<NSValue *> *singles;  // struct single without a JPEG, for the next frames
    NSMutableArray<NSValue *> *photos;   // struct single with a JPEG, the first one's photo in progress
    uint64_t photo_epoch, photo_taken_epoch;  // flush and configure end the photo in progress: its epoch is older
    NSMutableArray<MinatoAVFFrame *> *captured;
    NSMutableDictionary<NSNumber *, NSValue *> *waiting;  // token to its retained CVPixelBufferRef
    uint64_t frames, next_token;
    struct minato_camera_3a three_a;
    uint32_t last_states;
    CVPixelBufferRef last_image;
    struct apkrt_camera_controls applied;
    BOOL has_applied;
    VTPixelTransferSessionRef transfer;
    uint64_t cropped_window;
}
@end

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, MinatoAVFCamera *> *cameras;
static uint64_t last_handle;

static MinatoAVFCamera *find_camera(uint64_t handle) {
    pthread_mutex_lock(&state_lock);
    MinatoAVFCamera *camera = cameras[@(handle)];
    pthread_mutex_unlock(&state_lock);
    return camera;
}

/* A CMTime of the session's clock as CLOCK_UPTIME_RAW's nanoseconds. */
static int64_t uptime_ns(AVCaptureSession *session, CMTime time) {
    CMClockRef clock = session.synchronizationClock;
    if (clock != NULL) time = CMSyncConvertTime(time, clock, CMClockGetHostTimeClock());
    const uint64_t ticks = CMClockConvertHostTimeToSystemUnits(time);
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0) mach_timebase_info(&timebase);
    const uint64_t numer = timebase.numer, denom = timebase.denom;
    return (int64_t)(ticks / denom * numer + ticks % denom * numer / denom);
}

static void end_frames(MinatoAVFCamera *camera) {
    [camera->captured removeAllObjects];
    camera->peeked = NO;
}

/* A frame into the captured ones, the oldest that peek did not give going past max_captured. Under the lock. */
static void keep(MinatoAVFCamera *camera, MinatoAVFFrame *frame) {
    if (camera->captured.count >= max_captured) {
        const NSUInteger drop = camera->peeked ? 1 : 0;
        if (drop < camera->captured.count) [camera->captured removeObjectAtIndex:drop];
    }
    [camera->captured addObject:frame];
}

/* Stream n of `targets` given `image` and a token. */
static void give_image(MinatoAVFCamera *camera, MinatoAVFFrame *frame, uint32_t targets, CVPixelBufferRef image) {
    for (uint32_t n = 0; n < camera->stream_count; n++) {
        if ((targets & camera->video_streams & (1u << n)) == 0 || image == NULL) continue;
        frame->images[n] = CVPixelBufferRetain(image);
        frame->frame.tokens[n] = ++camera->next_token;
        frame->frame.streams |= 1u << n;
    }
}

static void describe_frame(MinatoAVFCamera *camera, MinatoAVFFrame *frame, const struct single *request,
                           int64_t timestamp) {
    AVCaptureDevice *device = devices[camera->index];
    frame->frame.number = request->number;
    frame->frame.frame = camera->frames++;
    frame->frame.timestamp_ns = timestamp;
    frame->frame.exposure_ns = duration_ns(device.exposureDuration);
    frame->frame.frame_duration_ns = duration_ns(device.activeVideoMinFrameDuration);
    frame->frame.iso = (int32_t)device.ISO;
    frame->frame.zoom_ratio = request->controls.zoom_ratio;
    const struct minato_camera_device *description = &descriptions[camera->index];
    const float diopters = description->min_focus_mm > 0 ? 1000.0f / description->min_focus_mm : 10.0f;
    frame->frame.focus_distance = description->autofocus ? (1.0f - device.lensPosition) * diopters : 0.0f;
}

@implementation MinatoAVFCamera
- (void)dealloc {
    for (NSValue *value in waiting.allValues) CVPixelBufferRelease((CVPixelBufferRef)value.pointerValue);
    if (last_image != NULL) CVPixelBufferRelease(last_image);
    if (transfer != NULL) {
        VTPixelTransferSessionInvalidate(transfer);
        CFRelease(transfer);
    }
    pthread_mutex_destroy(&lock);
}

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection *)connection {
    (void)output, (void)connection;
    CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample);
    if (image == NULL) return;
    AVCaptureDevice *device = devices[index];
    const struct minato_camera_adjusting now = {
        descriptions[index].autofocus, device.adjustingFocus ? 1 : 0, device.adjustingExposure ? 1 : 0,
        device.adjustingWhiteBalance ? 1 : 0, device.flashAvailable ? 1 : 0};
    const int64_t timestamp = uptime_ns(session, CMSampleBufferGetPresentationTimeStamp(sample));
    pthread_mutex_lock(&lock);
    if (lost) {
        pthread_mutex_unlock(&lock);
        return;
    }
    if (last_image != NULL) CVPixelBufferRelease(last_image);
    last_image = CVPixelBufferRetain(image);
    struct single request;
    if (singles.count > 0) {
        [singles[0] getValue:&request];
        [singles removeObjectAtIndex:0];
    } else if (repeating) {
        request = (struct single){repeating_controls, repeating_number, repeating_targets};
    } else {
        pthread_mutex_unlock(&lock);
        return;
    }
    last_states = minato_camera_3a_frame(&three_a, &now);
    MinatoAVFFrame *frame = [MinatoAVFFrame new];
    if (frame != nil) {
        describe_frame(self, frame, &request, timestamp);
        frame->frame.states = last_states;
        give_image(self, frame, request.targets, image);
        keep(self, frame);
    }
    pthread_mutex_unlock(&lock);
}

- (void)captureOutput:(AVCapturePhotoOutput *)output
    didFinishProcessingPhoto:(AVCapturePhoto *)taken
                       error:(NSError *)error {
    (void)output;
    NSData *bytes = error == nil ? taken.fileDataRepresentation : nil;
    const int64_t timestamp = uptime_ns(session, taken.timestamp);
    BOOL next = NO;
    pthread_mutex_lock(&lock);
    photo_busy = NO;
    if (photo_taken_epoch != photo_epoch) {
        next = photos.count > 0 && !lost;  // the photo's request went with a flush: a later one's next
    } else if (photos.count > 0) {
        struct single request;
        [photos[0] getValue:&request];
        [photos removeObjectAtIndex:0];
        next = photos.count > 0 && !lost;
        MinatoAVFFrame *frame = lost ? nil : [MinatoAVFFrame new];
        if (frame != nil) {
            describe_frame(self, frame, &request, timestamp);
            frame->frame.states = last_states;
            if (taken.resolvedSettings.flashEnabled) frame->frame.states |= APKRT_CAMERA_STATE_FLASH_FIRED;
            give_image(self, frame, request.targets, last_image);
            if (bytes.length > 0 && bytes.length <= APKRT_CAMERA_MAX_JPEG && jpeg_stream >= 0 &&
                (request.targets & (1u << (uint32_t)jpeg_stream)) != 0) {
                frame->jpeg = bytes;
                frame->frame.streams |= 1u << (uint32_t)jpeg_stream;
            }  // else the JPEG stream's buffer fails
            keep(self, frame);
        }
    }
    pthread_mutex_unlock(&lock);
    if (next) [self takePhoto];
}

/* The first waiting photo request's photo, when none is in progress. */
- (void)takePhoto {
    pthread_mutex_lock(&lock);
    if (photo_busy || photos.count == 0 || lost) {
        pthread_mutex_unlock(&lock);
        return;
    }
    struct single request;
    [photos[0] getValue:&request];
    photo_busy = YES;
    photo_taken_epoch = photo_epoch;
    const int32_t width = still_width, height = still_height;
    pthread_mutex_unlock(&lock);

    const struct apkrt_camera_controls *controls = &request.controls;
    NSDictionary *settings_format = @{
        AVVideoCodecKey : AVVideoCodecTypeJPEG,
        AVVideoCompressionPropertiesKey : @{AVVideoQualityKey : @(controls->jpeg_quality / 100.0)},
    };
    AVCapturePhotoSettings *settings = [AVCapturePhotoSettings photoSettingsWithFormat:settings_format];
    settings.maxPhotoDimensions = (CMVideoDimensions){width, height};
    // camera2's AE_MODE_ON_AUTO_FLASH (2) and ON_ALWAYS_FLASH (3), and FLASH_MODE_SINGLE (1) with AE_MODE_ON.
    AVCaptureFlashMode flash = AVCaptureFlashModeOff;
    if (controls->ae_mode == 2) flash = AVCaptureFlashModeAuto;
    if (controls->ae_mode == 3 || (controls->ae_mode == 1 && controls->flash_mode == 1)) flash = AVCaptureFlashModeOn;
    if ([photo.supportedFlashModes containsObject:@(flash)]) settings.flashMode = flash;
    AVCaptureConnection *connection = [photo connectionWithMediaType:AVMediaTypeVideo];
    const CGFloat angle = minato_camera_still_rotation(descriptions[index].facing, controls->jpeg_orientation);
    if ([connection isVideoRotationAngleSupported:angle]) connection.videoRotationAngle = angle;
    [photo capturePhotoWithSettings:settings delegate:self];
}
@end

static uint32_t count(void *context) {
    (void)context;
    find_cameras();
    return device_count;
}

static int facts(void *context, uint32_t camera, void *data, size_t capacity, size_t *size) {
    (void)context;
    find_cameras();
    if (camera >= device_count) return -linux_einval;
    const size_t needed = minato_camera_facts(&descriptions[camera], NULL, 0);
    *size = needed;
    if (capacity < needed) return -linux_enospc;
    minato_camera_facts(&descriptions[camera], data, capacity);
    return 0;
}

static void lose(MinatoAVFCamera *camera) {
    pthread_mutex_lock(&camera->lock);
    camera->lost = YES;
    pthread_mutex_unlock(&camera->lock);
}

static int open_camera(void *context, uint32_t index, uint64_t *handle) {
    (void)context;
    find_cameras();
    if (index >= device_count) return -linux_einval;
    if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized) {
        return -linux_eacces;
    }
    @autoreleasepool {
        MinatoAVFCamera *camera = [MinatoAVFCamera new];
        if (camera == nil) return -linux_enomem;
        camera->index = index;
        pthread_mutex_init(&camera->lock, NULL);
        camera->jpeg_stream = -1;
        camera->singles = [NSMutableArray array];
        camera->photos = [NSMutableArray array];
        camera->captured = [NSMutableArray array];
        camera->waiting = [NSMutableDictionary dictionary];
        camera->queue = dispatch_queue_create("minato.camera", DISPATCH_QUEUE_SERIAL);
        camera->session = [AVCaptureSession new];
        NSError *error = nil;
        camera->input = [AVCaptureDeviceInput deviceInputWithDevice:devices[index] error:&error];
        if (camera->input == nil || camera->session == nil) return -linux_eio;
        pthread_mutex_lock(&state_lock);
        NSArray<MinatoAVFCamera *> *older = cameras.allValues;
        if (cameras == nil) cameras = [NSMutableDictionary dictionary];
        *handle = ++last_handle;
        cameras[@(*handle)] = camera;
        pthread_mutex_unlock(&state_lock);
        // The camera open before is lost (cameraservice evicts a conflicting camera of the same app; iOS runs one
        // session at a time).
        for (MinatoAVFCamera *each in older) {
            lose(each);
            if (each->session.running) [each->session stopRunning];
        }
        __weak MinatoAVFCamera *weak = camera;
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        void (^lost)(NSNotification *) = ^(NSNotification *note) {
          (void)note;
          MinatoAVFCamera *strong = weak;
          if (strong != nil) lose(strong);
        };
        camera->observers = @[
            [center addObserverForName:AVCaptureSessionWasInterruptedNotification
                                object:camera->session
                                 queue:nil
                            usingBlock:lost],
            [center addObserverForName:AVCaptureSessionRuntimeErrorNotification
                                object:camera->session
                                 queue:nil
                            usingBlock:lost],
        ];
        return 0;
    }
}

static void end_token(MinatoAVFCamera *camera, uint64_t token) {
    NSValue *value = camera->waiting[@(token)];
    if (value == nil) return;
    CVPixelBufferRelease((CVPixelBufferRef)value.pointerValue);
    [camera->waiting removeObjectForKey:@(token)];
}

static int configure(void *context, uint64_t handle, const struct apkrt_camera_stream *streams, uint32_t count_of) {
    (void)context;
    @autoreleasepool {
        MinatoAVFCamera *camera = find_camera(handle);
        if (camera == nil || count_of == 0 || count_of > APKRT_CAMERA_MAX_STREAMS) return -linux_einval;
        const int chosen = minato_camera_choose_format(&descriptions[camera->index], streams, count_of, 0);
        if (chosen < 0) return -linux_einval;
        AVCaptureDeviceFormat *format = formats[camera->index][(NSUInteger)chosen];
        AVCaptureDevice *device = devices[camera->index];
        uint32_t video_streams = 0;
        int32_t jpeg_stream = -1;
        for (uint32_t n = 0; n < count_of; n++) {
            if (streams[n].format == APKRT_CAMERA_FORMAT_JPEG) {
                jpeg_stream = (int32_t)n;
            } else {
                video_streams |= 1u << n;
            }
        }

        pthread_mutex_lock(&camera->lock);
        const BOOL lost = camera->lost;
        camera->repeating = NO;
        [camera->singles removeAllObjects];
        [camera->photos removeAllObjects];
        camera->photo_epoch++;
        end_frames(camera);
        pthread_mutex_unlock(&camera->lock);
        if (lost) return -linux_enodev;

        AVCaptureSession *session = camera->session;
        [session beginConfiguration];
        if (![session.inputs containsObject:camera->input]) {
            if (![session canAddInput:camera->input]) {
                [session commitConfiguration];
                return -linux_eio;
            }
            [session addInput:camera->input];
        }
        if (camera->video == nil) {
            AVCaptureVideoDataOutput *video = [AVCaptureVideoDataOutput new];
            video.alwaysDiscardsLateVideoFrames = YES;
            [video setSampleBufferDelegate:camera queue:camera->queue];
            if (![session canAddOutput:video]) {
                [session commitConfiguration];
                return -linux_eio;
            }
            [session addOutput:video];
            camera->video = video;
        }
        if (jpeg_stream >= 0 && camera->photo == nil) {
            AVCapturePhotoOutput *photo = [AVCapturePhotoOutput new];
            if ([session canAddOutput:photo]) {
                [session addOutput:photo];
                camera->photo = photo;
            }
        }
        NSError *error = nil;
        if ([device lockForConfiguration:&error]) {
            device.activeFormat = format;
            device.activeVideoMinFrameDuration = CMTimeMake(1, MINATO_CAMERA_MAX_FPS);
            [device unlockForConfiguration];
        }
        const FourCharCode subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
        camera->video.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(subtype)};
        AVCaptureConnection *connection = [camera->video connectionWithMediaType:AVMediaTypeVideo];
        const CGFloat angle = minato_camera_frame_rotation(descriptions[camera->index].facing);
        if ([connection isVideoRotationAngleSupported:angle]) connection.videoRotationAngle = angle;
        if (connection.supportsVideoMirroring) {
            connection.automaticallyAdjustsVideoMirroring = NO;
            connection.videoMirrored = NO;  // Android's frames are the sensor's, not mirrored
        }
        int32_t still_width = 0, still_height = 0;
        if (jpeg_stream >= 0) {
            const struct apkrt_camera_stream *jpeg = &streams[(uint32_t)jpeg_stream];
            still_width = (int32_t)jpeg->width, still_height = (int32_t)jpeg->height;
            camera->photo.maxPhotoDimensions = (CMVideoDimensions){still_width, still_height};
            AVCaptureConnection *still = [camera->photo connectionWithMediaType:AVMediaTypeVideo];
            if (still.supportsVideoMirroring) {
                still.automaticallyAdjustsVideoMirroring = NO;
                still.videoMirrored = NO;
            }
        }
        [session commitConfiguration];
        if (jpeg_stream >= 0 && camera->photo == nil) return -linux_einval;

        pthread_mutex_lock(&camera->lock);
        memcpy(camera->streams, streams, count_of * sizeof *streams);
        camera->stream_count = count_of;
        camera->video_streams = video_streams;
        camera->jpeg_stream = jpeg_stream;
        camera->still_width = still_width, camera->still_height = still_height;
        camera->has_applied = NO;
        pthread_mutex_unlock(&camera->lock);
        if (!session.running) [session startRunning];
        return 0;
    }
}

/* The lowest frame rate the device's active format runs at. */
static int32_t lowest_fps(AVCaptureDevice *device) {
    double lowest = MINATO_CAMERA_MAX_FPS;
    for (AVFrameRateRange *range in device.activeFormat.videoSupportedFrameRateRanges) {
        if (range.minFrameRate < lowest) lowest = range.minFrameRate;
    }
    return (int32_t)ceil(lowest);
}

/* A request's controls on the device, those that changed since the last. */
static void apply(MinatoAVFCamera *camera, const struct apkrt_camera_controls *controls) {
    struct apkrt_camera_controls base = *controls;
    base.af_trigger = 0, base.ae_precapture = 0;  // triggers act once, below
    const BOOL changed = !camera->has_applied || memcmp(&base, &camera->applied, sizeof base) != 0;
    if (!changed && controls->af_trigger == 0) return;
    AVCaptureDevice *device = devices[camera->index];
    const struct minato_camera_device *description = &descriptions[camera->index];
    NSError *error = nil;
    if (![device lockForConfiguration:&error]) return;
    int32_t width = 0, height = 0;
    minato_camera_still(description, &width, &height);  // the facts' active array
    float x = 0, y = 0;
    if (changed) {
        if (minato_camera_point(description->facing, controls->af_region, width, height, &x, &y) &&
            device.focusPointOfInterestSupported) {
            device.focusPointOfInterest = CGPointMake(x, y);
        }
        const BOOL continuous = controls->af_mode == 3 || controls->af_mode == 4;
        const AVCaptureFocusMode focus = continuous ? AVCaptureFocusModeContinuousAutoFocus : AVCaptureFocusModeLocked;
        if ([device isFocusModeSupported:focus]) device.focusMode = focus;
        if (minato_camera_point(description->facing, controls->ae_region, width, height, &x, &y) &&
            device.exposurePointOfInterestSupported) {
            device.exposurePointOfInterest = CGPointMake(x, y);
        }
        const AVCaptureExposureMode exposure =
            controls->ae_lock ? AVCaptureExposureModeLocked : AVCaptureExposureModeContinuousAutoExposure;
        if ([device isExposureModeSupported:exposure]) device.exposureMode = exposure;
        [device setExposureTargetBias:minato_camera_bias(description, controls->ae_compensation)
                    completionHandler:nil];
        const AVCaptureWhiteBalanceMode balance =
            controls->awb_lock ? AVCaptureWhiteBalanceModeLocked : AVCaptureWhiteBalanceModeContinuousAutoWhiteBalance;
        if ([device isWhiteBalanceModeSupported:balance]) device.whiteBalanceMode = balance;
        const AVCaptureTorchMode torch = controls->flash_mode == 2 ? AVCaptureTorchModeOn : AVCaptureTorchModeOff;
        if (device.hasTorch && [device isTorchModeSupported:torch]) device.torchMode = torch;  // FLASH_MODE_TORCH
        device.videoZoomFactor =
            minato_camera_zoom_factor(description, controls->zoom_ratio, (float)device.activeFormat.videoMaxZoomFactor);
        if (controls->fps_max > 0) {
            const int32_t low = lowest_fps(device);
            const int32_t most = controls->fps_max < low ? low : controls->fps_max;
            const int32_t least = controls->fps_min < low ? low : controls->fps_min > most ? most : controls->fps_min;
            device.activeVideoMinFrameDuration = CMTimeMake(1, most);
            device.activeVideoMaxFrameDuration = CMTimeMake(1, least);
        }
        camera->applied = base;
        camera->has_applied = YES;
    }
    if (controls->af_trigger == 1 && [device isFocusModeSupported:AVCaptureFocusModeAutoFocus]) {
        device.focusMode = AVCaptureFocusModeAutoFocus;  // one focus, then locked
    }
    if (controls->af_trigger == 2) {
        const BOOL continuous = controls->af_mode == 3 || controls->af_mode == 4;
        const AVCaptureFocusMode focus = continuous ? AVCaptureFocusModeContinuousAutoFocus : AVCaptureFocusModeLocked;
        if ([device isFocusModeSupported:focus]) device.focusMode = focus;
    }
    [device unlockForConfiguration];
}

static int request(void *context, uint64_t handle, const struct apkrt_camera_controls *controls, uint64_t number,
                   uint32_t targets, uint32_t flags) {
    (void)context;
    @autoreleasepool {
        MinatoAVFCamera *camera = find_camera(handle);
        if (camera == nil) return -linux_einval;
        pthread_mutex_lock(&camera->lock);
        const BOOL lost = camera->lost;
        const BOOL full = camera->singles.count + camera->photos.count >= APKRT_CAMERA_MAX_WAITING;
        const BOOL still = camera->jpeg_stream >= 0 && (targets & (1u << (uint32_t)camera->jpeg_stream)) != 0;
        pthread_mutex_unlock(&camera->lock);
        if (lost) return -linux_enodev;
        if ((flags & APKRT_CAMERA_REPEATING) == 0 && full) return -linux_eagain;
        apply(camera, controls);
        pthread_mutex_lock(&camera->lock);
        minato_camera_3a_trigger(&camera->three_a, controls->af_trigger, controls->ae_precapture);
        if ((flags & APKRT_CAMERA_REPEATING) != 0) {
            camera->repeating = YES;
            camera->repeating_controls = *controls;
            camera->repeating_controls.af_trigger = 0, camera->repeating_controls.ae_precapture = 0;
            camera->repeating_number = number;
            camera->repeating_targets = still ? targets & ~(1u << (uint32_t)camera->jpeg_stream) : targets;
            pthread_mutex_unlock(&camera->lock);
            return 0;
        }
        const struct single single = {*controls, number, targets};
        NSValue *value = [NSValue valueWithBytes:&single objCType:@encode(struct single)];
        [still ? camera->photos : camera->singles addObject:value];
        pthread_mutex_unlock(&camera->lock);
        if (still) [camera takePhoto];
        return 0;
    }
}

static void stop_repeating(void *context, uint64_t handle) {
    (void)context;
    MinatoAVFCamera *camera = find_camera(handle);
    if (camera == nil) return;
    pthread_mutex_lock(&camera->lock);
    camera->repeating = NO;
    pthread_mutex_unlock(&camera->lock);
}

static int peek(void *context, uint64_t handle, struct apkrt_camera_capture *out) {
    (void)context;
    MinatoAVFCamera *camera = find_camera(handle);
    if (camera == nil) return -linux_einval;
    pthread_mutex_lock(&camera->lock);
    int answer = 0;
    if (camera->lost) {
        answer = -linux_enodev;
    } else if (camera->captured.count == 0) {
        answer = -linux_eagain;
    } else {
        MinatoAVFFrame *frame = camera->captured[0];
        out->frame = frame->frame;
        out->jpeg = frame->jpeg.bytes;  // the frame stays while peeked (keep)
        out->jpeg_size = frame->jpeg.length;
        camera->peeked = YES;
    }
    pthread_mutex_unlock(&camera->lock);
    return answer;
}

static void pop(void *context, uint64_t handle) {
    (void)context;
    MinatoAVFCamera *camera = find_camera(handle);
    if (camera == nil) return;
    pthread_mutex_lock(&camera->lock);
    if (camera->captured.count > 0 && camera->peeked) {
        MinatoAVFFrame *frame = camera->captured[0];
        for (int n = 0; n < 4; n++) {
            if (frame->images[n] == NULL) continue;
            camera->waiting[@(frame->frame.tokens[n])] = [NSValue valueWithPointer:frame->images[n]];
            frame->images[n] = NULL;  // the waiting entry holds it now
        }
        [camera->captured removeObjectAtIndex:0];
    }
    camera->peeked = NO;
    pthread_mutex_unlock(&camera->lock);
}

static CVPixelBufferRef take_waiting(MinatoAVFCamera *camera, uint64_t token) {
    pthread_mutex_lock(&camera->lock);
    NSValue *value = camera->waiting[@(token)];
    if (value != nil) [camera->waiting removeObjectForKey:@(token)];
    pthread_mutex_unlock(&camera->lock);
    return value == nil ? NULL : (CVPixelBufferRef)value.pointerValue;
}

static int render(void *context, uint64_t handle, uint64_t token, const struct apkrt_video_target *target) {
    (void)context;
    @autoreleasepool {
        MinatoAVFCamera *camera = find_camera(handle);
        CVPixelBufferRef image = camera != nil ? take_waiting(camera, token) : NULL;
        if (image == NULL) return -linux_einval;
        const int answer = minato_video_frame_render(image, target, &camera->transfer);
        CVPixelBufferRelease(image);
        return answer;
    }
}

static void discard(void *context, uint64_t handle, uint64_t token) {
    (void)context;
    MinatoAVFCamera *camera = find_camera(handle);
    if (camera == nil) return;
    pthread_mutex_lock(&camera->lock);
    end_token(camera, token);
    pthread_mutex_unlock(&camera->lock);
}

static int render_window(void *context, uint64_t handle, uint64_t token, uint64_t window, int64_t time_ns,
                         uint32_t flags) {
    (void)context;
    @autoreleasepool {
        MinatoAVFCamera *camera = find_camera(handle);
        CVPixelBufferRef image = camera != nil ? take_waiting(camera, token) : NULL;
        if (image == NULL) return -linux_einval;
        const int answer = minato_video_frame_render_window(image, window, time_ns, flags, &camera->cropped_window);
        CVPixelBufferRelease(image);
        return answer;
    }
}

static int flush(void *context, uint64_t handle) {
    (void)context;
    MinatoAVFCamera *camera = find_camera(handle);
    if (camera == nil) return -linux_einval;
    pthread_mutex_lock(&camera->lock);
    [camera->singles removeAllObjects];
    [camera->photos removeAllObjects];  // a photo in progress ends without a frame
    camera->photo_epoch++;
    end_frames(camera);
    for (NSNumber *token in camera->waiting.allKeys) end_token(camera, token.unsignedLongLongValue);
    pthread_mutex_unlock(&camera->lock);
    return 0;
}

static void close_camera(void *context, uint64_t handle) {
    (void)context;
    @autoreleasepool {
        MinatoAVFCamera *camera = find_camera(handle);
        if (camera == nil) return;
        for (id observer in camera->observers) [NSNotificationCenter.defaultCenter removeObserver:observer];
        lose(camera);  // the delegates' late frames go
        if (camera->session.running) [camera->session stopRunning];
        [camera->video setSampleBufferDelegate:nil queue:NULL];
        if (camera->cropped_window != 0) minato_video_frame_give_back_gravity(camera->cropped_window);
        // A device setting of the camera's (the torch) does not outlive it.
        AVCaptureDevice *device = devices[camera->index];
        NSError *error = nil;
        if (device.torchActive && [device lockForConfiguration:&error]) {
            device.torchMode = AVCaptureTorchModeOff;
            [device unlockForConfiguration];
        }
        dispatch_sync(camera->queue, ^{  // a capture still running on the queue is done
        });
        pthread_mutex_lock(&state_lock);
        [cameras removeObjectForKey:@(handle)];
        pthread_mutex_unlock(&state_lock);
    }
}

static const struct apkrt_cameras functions = {count,    facts,  open_camera, configure, request,
                                               stop_repeating,   peek,        pop,       render,
                                               discard,  render_window, flush,  close_camera, NULL};

int minato_graphics_set_cameras(void) {
    if (count(NULL) == 0) return minato_graphics_set_camera_model();
    return apkrt_set_cameras(&functions) == 0 ? 0 : -1;
}

#else

int minato_graphics_set_cameras(void) {
    return minato_graphics_set_camera_model();
}

#endif
