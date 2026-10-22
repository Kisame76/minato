/* graphics/host's video decoders (ADR-0060 e2b3), which minato_graphics_set_providers gives the runtime
   (apkrt_set_video_decoders, whose contract they keep): one VTDecompressionSession per decoder, H.264 and HEVC.
   - Input: an access unit in Annex B, as MediaCodec takes it. Its parameter sets (H.264's SPS and PPS, HEVC's VPS,
     SPS and PPS), from the codec data or in the stream, are kept by their IDs (video_parameters.c), every one of them
     in the format description; a set of a new ID, or one whose ID comes with other bytes, makes a new session, after
     the old one's frames, and a set that comes again as it was changes nothing (H.264's nal_ref_idc aside). Access
     unit delimiters go; the other NAL units are decoded, each with a 4-byte length as VideoToolbox takes them. A
     unit before any parameter set, or one VideoToolbox refuses, decodes to nothing, as garbage does on Android's
     software decoder [inferred].
   - Output: VideoToolbox's native 4:2:0 ('420v' or '420f', so that it converts nothing) copied into NV12 frames of
     the host's own, in presentation order, as Android's decoders hand them out: decoded frames are held, sorted by
     time, while there are no more of them than the stream's reorder depth (video_parameters.c: the SPS's, 16 where it
     cannot be read), and the earliest becomes ready when one more comes. At most eight frames are ready at once: a
     queue then answers -EAGAIN until frames are popped, as it does while eight units are still in VideoToolbox. A
     decoded image beyond the runtime's limits (APKRT_VIDEO_DECODER_MAX_DIMENSION) or not 4:2:0 in two planes of 8
     bits is dropped, so that no frame reaches the runtime that it would refuse. The crop is the picture's whole size,
     which VideoToolbox's buffers have.
   - The end of the stream: the delayed frames, then a frame of size 0 with APKRT_VIDEO_DECODER_END_OF_STREAM.
   - A flush or a destroy moves the decoder's generation, so that no frame of its earlier input comes after it, waits
     for VideoToolbox's frames in flight and ends the session; a flush's next input makes a new one, as the next unit
     does after VideoToolbox ended a session (iOS in the background) or found its decoder broken.
   VideoToolbox calls back on threads of its own; a decoder's mutex guards its frames and generation and is never held
   across a call into VideoToolbox. The runtime calls a decoder's functions one at a time. ARC. */
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <pthread.h>
#include <string.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "minato_graphics.h"
#include "video_parameters.h"

/* Linux's errno values, which the guest gets. */
enum { linux_eagain = 11, linux_enomem = 12, linux_einval = 22 };

/* The decoded frames a decoder holds before a queue waits for pops. */
static const NSUInteger max_frames = 8;

@interface MinatoDecodedFrame : NSObject {
@public
    NSMutableData *bytes;
    struct apkrt_video_frame frame;
}
@end

@implementation MinatoDecodedFrame
@end

@interface MinatoVideoDecoder : NSObject {
@public
    uint32_t type;
    NSMutableDictionary<NSNumber *, NSData *> *sets[3];  // the stream's VPSs, SPSs and PPSs, by their IDs
    BOOL parameters_changed;  // since the session's format description
    CMVideoFormatDescriptionRef format;
    VTDecompressionSessionRef session;
    pthread_mutex_t mutex;  // for the fields below, VideoToolbox's threads and the runtime's
    NSMutableArray<MinatoDecodedFrame *> *frames;  // ready, in presentation order
    NSMutableArray<MinatoDecodedFrame *> *held;    // decoded, not yet ready, in presentation order
    NSUInteger reorder_depth;                      // the frames held before the earliest is ready
    NSUInteger in_flight;                          // units in VideoToolbox, not yet called back
    MinatoDecodedFrame *peeked;
    uint64_t generation;
}
@end

@implementation MinatoVideoDecoder
- (instancetype)init {
    self = [super init];
    if (self != nil) {
        pthread_mutex_init(&mutex, NULL);
        frames = [NSMutableArray array];
        held = [NSMutableArray array];
        reorder_depth = 16;
        for (int kind = 0; kind < 3; kind++) sets[kind] = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void)dealloc {
    if (format != NULL) {
        CFRelease(format);
    }
    pthread_mutex_destroy(&mutex);
}
@end

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, MinatoVideoDecoder *> *decoders;
static uint64_t last_handle;  // handles count up from 1 and are never used again

static MinatoVideoDecoder *find_decoder(uint64_t handle) {
    pthread_mutex_lock(&state_lock);
    MinatoVideoDecoder *decoder = decoders[@(handle)];
    pthread_mutex_unlock(&state_lock);
    return decoder;
}

/* A NAL unit's type, H.264's or HEVC's. */
static unsigned nal_type(uint32_t type, const uint8_t *nal) {
    return type == APKRT_VIDEO_DECODER_HEVC ? (nal[0] >> 1) & 0x3fu : nal[0] & 0x1fu;
}

/* Calls `each` for every NAL unit of the Annex B bytes, without its start code. */
static void each_nal(const uint8_t *bytes, size_t size, void (^each)(const uint8_t *nal, size_t length)) {
    size_t at = 0, start = SIZE_MAX;
    while (at + 3 <= size) {
        if (bytes[at] == 0 && bytes[at + 1] == 0 && bytes[at + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = at;
                while (end > start && bytes[end - 1] == 0) end--;  // a 4-byte start code's leading zero
                if (end > start) each(bytes + start, end - start);
            }
            at += 3;
            start = at;
        } else {
            at++;
        }
    }
    if (start != SIZE_MAX && start < size) each(bytes + start, size - start);
}

/* Whether two parameter sets are the same, H.264's nal_ref_idc bits aside. */
static BOOL same_set(NSData *a, NSData *b, BOOL hevc) {
    if (a == nil || b == nil || a.length != b.length || a.length == 0) return NO;
    const uint8_t *x = a.bytes, *y = b.bytes;
    const uint8_t mask = hevc ? 0xff : 0x1f;
    return (x[0] & mask) == (y[0] & mask) && memcmp(x + 1, y + 1, a.length - 1) == 0;
}

/* Takes the stream's parameter sets in by their IDs; appends its other units, each with a 4-byte length, to `units`. */
static void read_units(MinatoVideoDecoder *decoder, const uint8_t *bytes, size_t size, NSMutableData *units) {
    each_nal(bytes, size, ^(const uint8_t *nal, size_t length) {
        const unsigned kind = nal_type(decoder->type, nal);
        const BOOL hevc = decoder->type == APKRT_VIDEO_DECODER_HEVC;
        int which = -1;  // VPS 0, SPS 1, PPS 2
        if (hevc ? kind == 32 : NO) {
            which = 0;
        } else if (hevc ? kind == 33 : kind == 7) {
            which = 1;
        } else if (hevc ? kind == 34 : kind == 8) {
            which = 2;
        }
        if (which >= 0) {
            const int id = hevc ? minato_hevc_parameter_set_id(nal, length) : minato_avc_parameter_set_id(nal, length);
            if (id < 0) return;  // a set it cannot read: VideoToolbox would refuse it too
            @autoreleasepool {
                NSData *data = [NSData dataWithBytes:nal length:length];
                if (same_set(decoder->sets[which][@(id)], data, hevc)) return;
                decoder->sets[which][@(id)] = data;
                decoder->parameters_changed = YES;
                if (which == 1) {
                    const int depth = hevc ? minato_hevc_reorder_depth(nal, length)
                                           : minato_avc_reorder_depth(nal, length);
                    pthread_mutex_lock(&decoder->mutex);
                    decoder->reorder_depth = depth >= 0 ? (NSUInteger)depth : 16;
                    pthread_mutex_unlock(&decoder->mutex);
                }
            }
            return;
        }
        if (hevc ? kind == 35 : kind == 9) {
            return;  // an access unit delimiter
        }
        if (length > UINT32_MAX) return;
        const uint8_t length_bytes[4] = {(uint8_t)(length >> 24), (uint8_t)(length >> 16), (uint8_t)(length >> 8),
                                         (uint8_t)length};
        [units appendBytes:length_bytes length:4];
        [units appendBytes:nal length:length];
    });
}

/* Inserts `frame` into `list` by its time. */
static void insert_by_time(NSMutableArray<MinatoDecodedFrame *> *list, MinatoDecodedFrame *frame) {
    NSUInteger at = list.count;
    while (at > 0 && list[at - 1]->frame.time_us > frame->frame.time_us) at--;
    [list insertObject:frame atIndex:at];
}

/* Moves the earliest held frames to the ready ones while more than `keep` are held, with the decoder's mutex held. */
static void release_held(MinatoVideoDecoder *decoder, NSUInteger keep) {
    while (decoder->held.count > keep) {
        [decoder->frames addObject:decoder->held[0]];
        [decoder->held removeObjectAtIndex:0];
    }
}

/* An NV12 frame of the decoder's from a decoded image, or nil. */
static MinatoDecodedFrame *copy_image(CVImageBufferRef image, int64_t time_us) {
    const OSType pixels = CVPixelBufferGetPixelFormatType(image);
    if (CVPixelBufferGetPlaneCount(image) != 2 || (pixels != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
                                                   pixels != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
        return nil;
    }
    if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return nil;
    const size_t width = CVPixelBufferGetWidth(image), height = CVPixelBufferGetHeight(image);
    const size_t luma_row = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    const size_t chroma_row = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    const size_t stride = luma_row > chroma_row ? luma_row : chroma_row, chroma_rows = (height + 1) / 2;
    const uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(image, 0);
    const uint8_t *chroma = CVPixelBufferGetBaseAddressOfPlane(image, 1);
    MinatoDecodedFrame *made = nil;
    // The runtime's limits, so that it never refuses a frame of ours (frame_bytes in runtime/src/video_decoders.cpp).
    const size_t limit = APKRT_VIDEO_DECODER_MAX_DIMENSION;
    NSMutableData *bytes = nil;
    if (width > 0 && height > 0 && width <= limit && height <= limit && stride >= width && stride <= 4 * limit &&
        luma != NULL && chroma != NULL && CVPixelBufferGetHeightOfPlane(image, 0) >= height &&
        CVPixelBufferGetHeightOfPlane(image, 1) >= chroma_rows &&
        (bytes = [NSMutableData dataWithLength:stride * (height + chroma_rows)]) != nil) {
        made = [MinatoDecodedFrame new];
        made->bytes = bytes;
        uint8_t *out = made->bytes.mutableBytes;
        for (size_t row = 0; row < height; row++) {
            memcpy(out + row * stride, luma + row * luma_row, luma_row);
        }
        for (size_t row = 0; row < chroma_rows; row++) {
            memcpy(out + (height + row) * stride, chroma + row * chroma_row, chroma_row);
        }
        made->frame = (struct apkrt_video_frame){
            .size = made->bytes.length, .width = (uint32_t)width, .height = (uint32_t)height,
            .stride = (uint32_t)stride, .slice_height = (uint32_t)height, .crop_right = (uint16_t)(width - 1),
            .crop_bottom = (uint16_t)(height - 1), .time_us = time_us};
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    return made;
}

/* VideoToolbox's answer for one unit: its frame is held, and the earliest one becomes ready past the reorder depth. */
static void decoded(void *context, void *frame_context, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime time, CMTime duration) {
    (void)duration;
    MinatoVideoDecoder *decoder = (__bridge MinatoVideoDecoder *)context;
    @autoreleasepool {
        const int64_t time_us =
            CMTimeConvertScale(time, 1000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
        const BOOL kept = status == noErr && image != NULL && (flags & kVTDecodeInfo_FrameDropped) == 0;
        MinatoDecodedFrame *made = kept ? copy_image(image, time_us) : nil;
        pthread_mutex_lock(&decoder->mutex);
        if ((uint64_t)(uintptr_t)frame_context == decoder->generation) {
            if (decoder->in_flight > 0) decoder->in_flight--;
            if (made != nil) {
                insert_by_time(decoder->held, made);
                release_held(decoder, decoder->reorder_depth);
            }
        }
        pthread_mutex_unlock(&decoder->mutex);
    }
}

/* Waits for VideoToolbox's frames in flight and ends the session, with the decoder's mutex not held. */
static void end_session(MinatoVideoDecoder *decoder) {
    if (decoder->session == NULL) return;
    VTDecompressionSessionWaitForAsynchronousFrames(decoder->session);
    VTDecompressionSessionInvalidate(decoder->session);
    CFRelease(decoder->session);
    decoder->session = NULL;
}

/* A session for the stream's parameter sets, the old one's frames first; NO while they are incomplete or refused. */
static BOOL ensure_session(MinatoVideoDecoder *decoder) {
    if (decoder->session != NULL && !decoder->parameters_changed) return YES;
    const BOOL hevc = decoder->type == APKRT_VIDEO_DECODER_HEVC;
    if (decoder->sets[1].count == 0 || decoder->sets[2].count == 0 || (hevc && decoder->sets[0].count == 0)) return NO;
    if (decoder->session != NULL) {
        VTDecompressionSessionFinishDelayedFrames(decoder->session);
        end_session(decoder);
    }
    decoder->parameters_changed = NO;
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status;
    // Every set, VPSs first (HEVC), then SPSs and PPSs, each kind by its IDs: at most 16 + 32 + 256.
    const uint8_t *sets[16 + 32 + 256];
    size_t sizes[16 + 32 + 256], count = 0;
    for (int which = hevc ? 0 : 1; which < 3; which++) {
        NSArray<NSNumber *> *ids = [decoder->sets[which].allKeys sortedArrayUsingSelector:@selector(compare:)];
        for (NSNumber *id in ids) {
            NSData *data = decoder->sets[which][id];
            sets[count] = data.bytes, sizes[count] = data.length, count++;
        }
    }
    if (hevc) {
        status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, count, sets, sizes, 4, NULL, &format);
    } else {
        status = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, count, sets, sizes, 4, &format);
    }
    if (status != noErr) return NO;
    if (decoder->format != NULL) CFRelease(decoder->format);
    decoder->format = format;
    NSDictionary *attributes = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @[
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        ],
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    const VTDecompressionOutputCallbackRecord callback = {decoded, (__bridge void *)decoder};
    VTDecompressionSessionRef session = NULL;
    if (VTDecompressionSessionCreate(NULL, format, NULL, (__bridge CFDictionaryRef)attributes, &callback, &session) !=
        noErr) {
        return NO;
    }
    decoder->session = session;
    return YES;
}

/* Decodes the units of one access unit with its time. */
static void decode(MinatoVideoDecoder *decoder, NSData *units, int64_t time_us) {
    CMBlockBufferRef block = NULL;
    if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, units.length, NULL, NULL, 0, units.length,
                                           kCMBlockBufferAssureMemoryNowFlag, &block) != kCMBlockBufferNoErr) {
        return;
    }
    if (CMBlockBufferReplaceDataBytes(units.bytes, block, 0, units.length) != kCMBlockBufferNoErr) {
        CFRelease(block);
        return;
    }
    const CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake(time_us, 1000000), kCMTimeInvalid};
    const size_t size = units.length;
    CMSampleBufferRef sample = NULL;
    if (CMSampleBufferCreateReady(NULL, block, decoder->format, 1, 1, &timing, 1, &size, &sample) == noErr) {
        pthread_mutex_lock(&decoder->mutex);
        const uint64_t generation = decoder->generation;
        decoder->in_flight++;
        pthread_mutex_unlock(&decoder->mutex);
        const VTDecodeFrameFlags flags =
            kVTDecodeFrame_EnableAsynchronousDecompression | kVTDecodeFrame_EnableTemporalProcessing;
        const OSStatus status = VTDecompressionSessionDecodeFrame(decoder->session, sample, flags,
                                                                  (void *)(uintptr_t)generation, NULL);
        if (status != noErr) {  // no call back comes for it
            pthread_mutex_lock(&decoder->mutex);
            if (decoder->generation == generation && decoder->in_flight > 0) decoder->in_flight--;
            pthread_mutex_unlock(&decoder->mutex);
        }
        if (status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr) {
            end_session(decoder);  // iOS ended it in the background, or its decoder broke: the next unit makes one
            decoder->parameters_changed = YES;
        }
        CFRelease(sample);
    }
    CFRelease(block);
}

static int create(void *context, const struct apkrt_video_decoder_description *description, uint64_t *handle) {
    (void)context;
    if (description->type != APKRT_VIDEO_DECODER_AVC && description->type != APKRT_VIDEO_DECODER_HEVC) {
        return -linux_einval;
    }
    @autoreleasepool {
        MinatoVideoDecoder *decoder = [MinatoVideoDecoder new];
        if (decoder == nil) return -linux_enomem;
        decoder->type = description->type;
        if (description->codec_data != NULL && description->codec_data_size > 0) {
            NSMutableData *ignored = [NSMutableData data];
            read_units(decoder, description->codec_data, description->codec_data_size, ignored);
        }
        pthread_mutex_lock(&state_lock);
        if (decoders == nil) decoders = [NSMutableDictionary dictionary];
        const uint64_t made = ++last_handle;
        decoders[@(made)] = decoder;
        pthread_mutex_unlock(&state_lock);
        *handle = made;
        return 0;
    }
}

static int queue(void *context, uint64_t handle, const void *data, size_t size, int64_t time_us, uint32_t flags) {
    (void)context;
    @autoreleasepool {
        MinatoVideoDecoder *decoder = find_decoder(handle);
        if (decoder == nil) return -linux_einval;
        pthread_mutex_lock(&decoder->mutex);
        const BOOL full = decoder->frames.count >= max_frames || decoder->in_flight >= max_frames;
        pthread_mutex_unlock(&decoder->mutex);
        if (full) return -linux_eagain;
        if (size > 0) {
            NSMutableData *units = [NSMutableData data];
            read_units(decoder, data, size, units);
            if (units.length > 0 && ensure_session(decoder)) {
                decode(decoder, units, time_us);
            }
        }
        if ((flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0) {
            if (decoder->session != NULL) {
                VTDecompressionSessionFinishDelayedFrames(decoder->session);
                VTDecompressionSessionWaitForAsynchronousFrames(decoder->session);
            }
            MinatoDecodedFrame *end = [MinatoDecodedFrame new];
            end->frame = (struct apkrt_video_frame){.time_us = time_us, .flags = APKRT_VIDEO_DECODER_END_OF_STREAM};
            pthread_mutex_lock(&decoder->mutex);
            release_held(decoder, 0);  // every frame has come: the session waited for them
            [decoder->frames addObject:end];
            pthread_mutex_unlock(&decoder->mutex);
        }
        return 0;
    }
}

static int peek(void *context, uint64_t handle, struct apkrt_video_frame *frame) {
    (void)context;
    @autoreleasepool {
        MinatoVideoDecoder *decoder = find_decoder(handle);
        if (decoder == nil) return -linux_einval;
        pthread_mutex_lock(&decoder->mutex);
        int result = -linux_eagain;
        if (decoder->frames.count > 0) {
            MinatoDecodedFrame *first = decoder->frames[0];
            decoder->peeked = first;
            *frame = first->frame;
            frame->data = first->bytes.bytes;
            result = 0;
        }
        pthread_mutex_unlock(&decoder->mutex);
        return result;
    }
}

static void pop(void *context, uint64_t handle) {
    (void)context;
    @autoreleasepool {
        MinatoVideoDecoder *decoder = find_decoder(handle);
        if (decoder == nil) return;
        pthread_mutex_lock(&decoder->mutex);
        if (decoder->peeked != nil) {
            [decoder->frames removeObjectIdenticalTo:decoder->peeked];
            decoder->peeked = nil;
        }
        pthread_mutex_unlock(&decoder->mutex);
    }
}

/* Moves the generation and drops every frame; the session ends after its frames in flight. */
static void drop_all(MinatoVideoDecoder *decoder) {
    pthread_mutex_lock(&decoder->mutex);
    decoder->generation++;
    pthread_mutex_unlock(&decoder->mutex);
    end_session(decoder);
    pthread_mutex_lock(&decoder->mutex);
    [decoder->frames removeAllObjects];
    [decoder->held removeAllObjects];
    decoder->in_flight = 0;
    decoder->peeked = nil;
    pthread_mutex_unlock(&decoder->mutex);
}

static int flush(void *context, uint64_t handle) {
    (void)context;
    @autoreleasepool {
        MinatoVideoDecoder *decoder = find_decoder(handle);
        if (decoder == nil) return -linux_einval;
        drop_all(decoder);
        decoder->parameters_changed = YES;  // a new session at the next unit, with the sets the stream had
        return 0;
    }
}

static void destroy(void *context, uint64_t handle) {
    (void)context;
    @autoreleasepool {
        MinatoVideoDecoder *decoder = find_decoder(handle);
        if (decoder == nil) return;
        drop_all(decoder);
        pthread_mutex_lock(&state_lock);
        [decoders removeObjectForKey:@(handle)];
        pthread_mutex_unlock(&state_lock);
    }
}

static const struct apkrt_video_decoders functions = {create, queue, peek, pop, flush, destroy, NULL};

const struct apkrt_video_decoders *minato_graphics_video_decoders(void) {
    return &functions;
}

int minato_graphics_set_video_decoders(void) {
    return apkrt_set_video_decoders(&functions) == 0 ? 0 : -1;
}
