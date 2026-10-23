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
     time, while there are no more of them than the reorder depth of the active SPS, the one the access unit's first
     slice names through its PPS (video_parameters.c: 16 where it cannot be read), and the earliest becomes ready when
     one more comes. Once eight frames are ready, a queue answers -EAGAIN until frames are popped, as it does while
     VideoToolbox has eight units and the reorder depth's (which its temporal processing may hold), but never for the
     end of the stream alone, which makes VideoToolbox hand them out: a decoder has at most those units' frames, the
     held ones and eight ready ones more. A decoded image beyond the
     runtime's limits (APKRT_VIDEO_DECODER_MAX_DIMENSION) or not 4:2:0 in two planes of 8 bits is dropped, so that no
     frame reaches the runtime that it would refuse. The crop is the picture's whole size, which VideoToolbox's
     buffers have.
   - The end of the stream: the delayed frames, then a frame of size 0 with APKRT_VIDEO_DECODER_END_OF_STREAM.
   - A decoder for a surface (APKRT_VIDEO_DECODER_SURFACE, ADR-0060 e2c) keeps VideoToolbox's image instead of a copy:
     peek gives the frame a token of the decoder's, pop puts it among the waiting ones (at most
     APKRT_VIDEO_DECODER_MAX_WAITING, peek -EAGAIN past them), and render draws it into the guest's hardware buffer, a
     slot of the surface's queue, by one copy on the GPU (decision 3), scaled to its size: VTPixelTransferSession into
     a YUV 420 buffer ('420v', an ImageReader's), and our Metal pass into an RGBA one (a SurfaceTexture's, a
     TextureView's, an RGBA reader's; RGBX and PRIVATE are 'RGBA' too): the image's planes as R8 and RG8 textures,
     chroma filtered linearly, through the matrix of its attachments (BT.601, BT.709 or BT.2020; without one BT.601
     below 720 rows and BT.709 from there, as Android takes an unspecified stream) and its range (video for '420v',
     full for '420f'), alpha 1. The pass ends before render returns, as Minato's queues have no fences.
   - Each unit in VideoToolbox has a token of its own, which its call back or its refusal ends once. A flush or a
     destroy ends every token, so that no frame of its earlier input comes after it, waits for VideoToolbox's frames in
     flight and ends the session; a flush's next input makes a new one, as the next unit does after VideoToolbox ended
     a session (iOS in the background) or found its decoder broken. Whenever a session ends, its units' tokens end (no
     call back may come for them), and the frames it held are ready, before any frame of the next session, whose
     reorder depth applies from then on.
   VideoToolbox calls back on threads of its own; a decoder's mutex guards its frames, tokens and reorder depth and is
   never held across a call into VideoToolbox. The runtime calls a decoder's functions one at a time, from the guest's
   syscall: no Objective-C exception leaves them (an allocation that fails answers -ENOMEM, anything else thrown
   -EIO), and the mutex is let go on the way out of one. ARC, with -fobjc-arc-exceptions. */
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include <pthread.h>
#include <simd/simd.h>
#include <string.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "minato_graphics.h"
#include "video_decoder_testing.h"
#include "video_parameters.h"

/* Linux's errno values, which the guest gets. */
enum { linux_eio = 5, linux_eagain = 11, linux_enomem = 12, linux_einval = 22 };

/* The decoded frames a decoder holds before a queue waits for pops. */
static const NSUInteger max_frames = 8;

/* The standard's IDs: H.264's SPSs and PPSs (HEVC's are fewer). */
enum { max_sps_ids = 32, max_pps_ids = 256 };

@interface MinatoDecodedFrame : NSObject {
@public
    NSMutableData *bytes;     // a ByteBuffer decoder's frame
    CVPixelBufferRef image;   // a surface decoder's, retained
    struct apkrt_video_frame frame;
}
@end

@implementation MinatoDecodedFrame
- (void)dealloc {
    if (image != NULL) CVPixelBufferRelease(image);
}
@end

@interface MinatoVideoDecoder : NSObject {
@public
    uint32_t type;
    BOOL surface;                       // frames by tokens, for render
    VTPixelTransferSessionRef transfer; // render's into YUV buffers, made at the first; the runtime's thread alone
    NSMutableDictionary<NSNumber *, NSData *> *sets[3];  // the stream's VPSs, SPSs and PPSs, by their IDs
    int sps_depth[max_sps_ids];                          // each SPS's reorder depth, -1 for none
    int pps_sps[max_pps_ids];                            // the SPS each PPS names, -1 for none
    int pending_depth;                                   // the active SPS's, for the next unit's session; -1 for none
    BOOL parameters_changed;  // since the session's format description
    CMVideoFormatDescriptionRef format;
    VTDecompressionSessionRef session;
    pthread_mutex_t mutex;  // for the fields below, VideoToolbox's threads and the runtime's
    NSMutableArray<MinatoDecodedFrame *> *frames;  // ready, in presentation order
    NSMutableArray<MinatoDecodedFrame *> *held;    // decoded, not yet ready, in presentation order
    NSUInteger reorder_depth;                      // the frames held before the earliest is ready
    NSMutableIndexSet *in_flight;                  // the tokens of units in VideoToolbox, not yet called back
    NSUInteger next_token;
    MinatoDecodedFrame *peeked;
    NSMutableDictionary<NSNumber *, MinatoDecodedFrame *> *waiting;  // a surface decoder's popped frames, by token
    uint64_t next_frame_token;
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
        pending_depth = -1;
        in_flight = [NSMutableIndexSet indexSet];
        next_token = 1;
        waiting = [NSMutableDictionary dictionary];
        next_frame_token = 1;
        for (int kind = 0; kind < 3; kind++) sets[kind] = [NSMutableDictionary dictionary];
        for (int id = 0; id < max_sps_ids; id++) sps_depth[id] = -1;
        for (int id = 0; id < max_pps_ids; id++) pps_sps[id] = -1;
    }
    return self;
}

- (void)dealloc {
    if (session != NULL) {  // ended before, by a destroy; kept as a guard for the call back's context
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
    }
    if (format != NULL) {
        CFRelease(format);
    }
    if (transfer != NULL) {
        VTPixelTransferSessionInvalidate(transfer);
        CFRelease(transfer);
    }
    pthread_mutex_destroy(&mutex);
}
@end

/* VideoToolbox's decode call, or the test's in its place in graphics.video_decoder_asan's own build of this file. */
#ifdef MINATO_VIDEO_DECODER_TESTING
static _Atomic(minato_video_decode_function) decode_function = VTDecompressionSessionDecodeFrame;

void minato_video_decoder_set_decode_for_testing(minato_video_decode_function decode) {
    decode_function = decode != NULL ? decode : VTDecompressionSessionDecodeFrame;
}
#else
static const minato_video_decode_function decode_function = VTDecompressionSessionDecodeFrame;
#endif

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, MinatoVideoDecoder *> *decoders;
static uint64_t last_handle;  // handles count up from 1 and are never used again

/* Runs `work` with the decoder's mutex held, which an exception inside it lets go of too. */
static void locked(MinatoVideoDecoder *decoder, void (^work)(void)) {
    pthread_mutex_lock(&decoder->mutex);
    @try {
        work();
    } @finally {
        pthread_mutex_unlock(&decoder->mutex);
    }
}

/* What was thrown, as the guest's answer: -ENOMEM for memory the host could not get, -EIO for anything else. */
static int exception_errno(id thrown) {
    const BOOL memory =
        [thrown isKindOfClass:[NSException class]] && [((NSException *)thrown).name isEqualToString:NSMallocException];
    return memory ? -linux_enomem : -linux_eio;
}

static MinatoVideoDecoder *find_decoder(uint64_t handle) {
    pthread_mutex_lock(&state_lock);
    MinatoVideoDecoder *decoder = nil;
    @try {
        decoder = decoders[@(handle)];
    } @finally {
        pthread_mutex_unlock(&state_lock);
    }
    return decoder;
}

/* A NAL unit's type, H.264's or HEVC's. */
static unsigned nal_type(uint32_t type, const uint8_t *nal) {
    return type == APKRT_VIDEO_DECODER_HEVC ? (nal[0] >> 1) & 0x3fu : nal[0] & 0x1fu;
}

/* Whether a NAL unit of `kind` is a slice: H.264's types 1 to 5, HEVC's slice segments 0 to 9 and 16 to 21. */
static BOOL is_slice(BOOL hevc, unsigned kind) {
    return hevc ? kind <= 9 || (kind >= 16 && kind <= 21) : kind >= 1 && kind <= 5;
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

/* Takes the stream's parameter sets in by their IDs; appends its other units, each with a 4-byte length, to `units`.
   The first slice's PPS names the active SPS, whose reorder depth the unit's session takes. */
static void read_units(MinatoVideoDecoder *decoder, const uint8_t *bytes, size_t size, NSMutableData *units) {
    __block BOOL sliced = NO;
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
            }
            if (which == 1 && id < max_sps_ids) {
                const int depth = hevc ? minato_hevc_reorder_depth(nal, length) : minato_avc_reorder_depth(nal, length);
                decoder->sps_depth[id] = depth >= 0 ? depth : 16;
            } else if (which == 2 && id < max_pps_ids) {
                decoder->pps_sps[id] = hevc ? minato_hevc_pps_sps_id(nal, length) : minato_avc_pps_sps_id(nal, length);
            }
            return;
        }
        if (hevc ? kind == 35 : kind == 9) {
            return;  // an access unit delimiter
        }
        if (!sliced && is_slice(hevc, kind)) {
            sliced = YES;
            const int pps = hevc ? minato_hevc_slice_pps_id(nal, length) : minato_avc_slice_pps_id(nal, length);
            const int sps = pps >= 0 && pps < max_pps_ids ? decoder->pps_sps[pps] : -1;
            const int depth = sps >= 0 && sps < max_sps_ids ? decoder->sps_depth[sps] : -1;
            if (depth >= 0) decoder->pending_depth = depth;  // after the old session's frames
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
    @try {
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
    } @catch (id thrown) {
        made = nil;  // the frame's memory could not be had: it is dropped
    } @finally {
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    }
    return made;
}

/* A surface decoder's frame of a decoded image, which it keeps, or nil: the same images copy_image takes. */
static MinatoDecodedFrame *keep_image(CVImageBufferRef image, int64_t time_us) {
    const OSType pixels = CVPixelBufferGetPixelFormatType(image);
    const size_t width = CVPixelBufferGetWidth(image), height = CVPixelBufferGetHeight(image);
    const size_t limit = APKRT_VIDEO_DECODER_MAX_DIMENSION;
    if (CVPixelBufferGetPlaneCount(image) != 2 ||
        (pixels != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
         pixels != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
        width == 0 || height == 0 || width > limit || height > limit) {
        return nil;
    }
    MinatoDecodedFrame *made = nil;
    @try {
        made = [MinatoDecodedFrame new];
    } @catch (id thrown) {
        return nil;
    }
    made->image = CVPixelBufferRetain(image);
    made->frame = (struct apkrt_video_frame){.width = (uint32_t)width, .height = (uint32_t)height,
                                             .crop_right = (uint16_t)(width - 1), .crop_bottom = (uint16_t)(height - 1),
                                             .time_us = time_us};
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
        MinatoDecodedFrame *made = !kept ? nil : decoder->surface ? keep_image(image, time_us)
                                                                  : copy_image(image, time_us);
        @try {
            locked(decoder, ^{
                if ([decoder->in_flight containsIndex:(NSUInteger)(uintptr_t)frame_context]) {
                    [decoder->in_flight removeIndex:(NSUInteger)(uintptr_t)frame_context];
                    if (made != nil) {
                        insert_by_time(decoder->held, made);
                        release_held(decoder, decoder->reorder_depth);
                    }
                }
            });
        } @catch (id thrown) {
            // On VideoToolbox's thread, nothing to answer: the frame is lost, as one VideoToolbox drops.
        }
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

/* After a session ended: its units' tokens end, so that they count no more and a late call back of one is dropped,
   and the frames it held are ready, before any frame of the next session. */
static void session_gone(MinatoVideoDecoder *decoder) {
    locked(decoder, ^{
        [decoder->in_flight removeAllIndexes];
        release_held(decoder, 0);
    });
}

/* A session for the stream's parameter sets, the old one's frames first; NO while they are incomplete or refused. */
static BOOL ensure_session(MinatoVideoDecoder *decoder) {
    if (decoder->session != NULL && !decoder->parameters_changed) return YES;
    const BOOL hevc = decoder->type == APKRT_VIDEO_DECODER_HEVC;
    if (decoder->sets[1].count == 0 || decoder->sets[2].count == 0 || (hevc && decoder->sets[0].count == 0)) return NO;
    if (decoder->session != NULL) {
        VTDecompressionSessionFinishDelayedFrames(decoder->session);
        end_session(decoder);
        session_gone(decoder);
    }
    decoder->parameters_changed = NO;
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status;
    // Every set, VPSs first (HEVC), then SPSs and PPSs, each kind by its IDs: at most 16 + 32 + 256, which the
    // parser's limits on the IDs keep to; more are refused here as well.
    const uint8_t *sets[16 + 32 + 256];
    size_t sizes[16 + 32 + 256], count = 0;
    for (int which = hevc ? 0 : 1; which < 3; which++) {
        NSArray<NSNumber *> *ids = [decoder->sets[which].allKeys sortedArrayUsingSelector:@selector(compare:)];
        for (NSNumber *id in ids) {
            if (count == sizeof sets / sizeof *sets) return NO;
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

/* Decodes the units of one access unit with its time, under a token of its own. An exception of the decode call goes
   on to the caller, with the unit no more in flight. */
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
        __block NSUInteger token = 0;
        @try {
            locked(decoder, ^{
                token = decoder->next_token++;
                [decoder->in_flight addIndex:token];
            });
        } @catch (id thrown) {
            CFRelease(sample);
            CFRelease(block);
            @throw;
        }
        const VTDecodeFrameFlags flags =
            kVTDecodeFrame_EnableAsynchronousDecompression | kVTDecodeFrame_EnableTemporalProcessing;
        OSStatus status = noErr;
        @try {
            status = decode_function(decoder->session, sample, flags, (void *)(uintptr_t)token, NULL);
        } @catch (id thrown) {
            status = kVTVideoDecoderBadDataErr;
            @throw;
        } @finally {
            if (status != noErr) {  // no call back comes for it, or one came already: the token ends once
                locked(decoder, ^{
                    [decoder->in_flight removeIndex:token];
                });
            }
            CFRelease(sample);
            CFRelease(block);
        }
        if (status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr) {
            end_session(decoder);  // iOS ended it in the background, or its decoder broke: the next unit makes one
            session_gone(decoder);
            decoder->parameters_changed = YES;
        }
        return;
    }
    CFRelease(block);
}

static int create(void *context, const struct apkrt_video_decoder_description *description, uint64_t *handle) {
    (void)context;
    if (description->type != APKRT_VIDEO_DECODER_AVC && description->type != APKRT_VIDEO_DECODER_HEVC) {
        return -linux_einval;
    }
    @try {
        @autoreleasepool {
            MinatoVideoDecoder *decoder = [MinatoVideoDecoder new];
            if (decoder == nil) return -linux_enomem;
            decoder->type = description->type;
            decoder->surface = (description->flags & APKRT_VIDEO_DECODER_SURFACE) != 0;
            if (description->codec_data != NULL && description->codec_data_size > 0) {
                NSMutableData *ignored = [NSMutableData data];
                read_units(decoder, description->codec_data, description->codec_data_size, ignored);
            }
            uint64_t made = 0;
            pthread_mutex_lock(&state_lock);
            @try {
                if (decoders == nil) decoders = [NSMutableDictionary dictionary];
                decoders[@(last_handle + 1)] = decoder;
                made = ++last_handle;
            } @finally {
                pthread_mutex_unlock(&state_lock);
            }
            *handle = made;
            return 0;
        }
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static int queue(void *context, uint64_t handle, const void *data, size_t size, int64_t time_us, uint32_t flags) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoVideoDecoder *decoder = find_decoder(handle);
            if (decoder == nil) return -linux_einval;
            __block BOOL full = NO;
            locked(decoder, ^{
                full = decoder->frames.count >= max_frames ||
                       (size > 0 && decoder->in_flight.count >= max_frames + decoder->reorder_depth);
            });
            if (full) return -linux_eagain;
            if (size > 0) {
                NSMutableData *units = [NSMutableData data];
                read_units(decoder, data, size, units);
                if (units.length > 0 && ensure_session(decoder)) {
                    if (decoder->pending_depth >= 0) {
                        const NSUInteger depth = (NSUInteger)decoder->pending_depth;
                        decoder->pending_depth = -1;
                        locked(decoder, ^{
                            decoder->reorder_depth = depth;
                        });
                    }
                    decode(decoder, units, time_us);
                }
            }
            if ((flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0) {
                if (decoder->session != NULL) {
                    VTDecompressionSessionFinishDelayedFrames(decoder->session);
                    VTDecompressionSessionWaitForAsynchronousFrames(decoder->session);
                }
                MinatoDecodedFrame *end = [MinatoDecodedFrame new];
                end->frame =
                    (struct apkrt_video_frame){.time_us = time_us, .flags = APKRT_VIDEO_DECODER_END_OF_STREAM};
                locked(decoder, ^{
                    release_held(decoder, 0);  // every frame has come: the session waited for them
                    [decoder->frames addObject:end];
                });
            }
            return 0;
        }
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static int peek(void *context, uint64_t handle, struct apkrt_video_frame *frame) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoVideoDecoder *decoder = find_decoder(handle);
            if (decoder == nil) return -linux_einval;
            __block int result = -linux_eagain;
            locked(decoder, ^{
                if (decoder->frames.count > 0 && decoder->waiting.count < APKRT_VIDEO_DECODER_MAX_WAITING) {
                    MinatoDecodedFrame *first = decoder->frames[0];
                    if (first->image != NULL && first->frame.token == 0) {
                        first->frame.token = decoder->next_frame_token++;
                    }
                    decoder->peeked = first;
                    *frame = first->frame;
                    frame->data = first->bytes.bytes;
                    result = 0;
                }
            });
            return result;
        }
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static void pop(void *context, uint64_t handle) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoVideoDecoder *decoder = find_decoder(handle);
            if (decoder == nil) return;
            locked(decoder, ^{
                if (decoder->peeked != nil) {
                    MinatoDecodedFrame *popped = decoder->peeked;
                    if (popped->image != NULL) decoder->waiting[@(popped->frame.token)] = popped;
                    [decoder->frames removeObjectIdenticalTo:popped];
                    decoder->peeked = nil;
                }
            });
        }
    } @catch (id thrown) {
        // Nothing to answer: the frame stays for the next peek.
    }
}

/* Ends every token and drops every frame; the session ends after its frames in flight, which are dropped too. The
   frames go also when something on the way throws, so that no waiting image stays pinned past a flush whose failure
   the runtime hears (it keeps its tokens then, whose render answers -EINVAL and whose discard nothing). */
static void drop_all(MinatoVideoDecoder *decoder) {
    @try {
        locked(decoder, ^{
            [decoder->in_flight removeAllIndexes];
        });
        end_session(decoder);
    } @finally {
        locked(decoder, ^{
            [decoder->frames removeAllObjects];
            [decoder->held removeAllObjects];
            [decoder->in_flight removeAllIndexes];
            [decoder->waiting removeAllObjects];
            decoder->peeked = nil;
        });
    }
}

static int flush(void *context, uint64_t handle) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoVideoDecoder *decoder = find_decoder(handle);
            if (decoder == nil) return -linux_einval;
            drop_all(decoder);
            decoder->parameters_changed = YES;  // a new session at the next unit, with the sets the stream had
            return 0;
        }
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static void destroy(void *context, uint64_t handle) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoVideoDecoder *decoder = find_decoder(handle);
            if (decoder == nil) return;
            @try {
                drop_all(decoder);
            } @finally {
                pthread_mutex_lock(&state_lock);
                @try {
                    [decoders removeObjectForKey:@(handle)];
                } @finally {
                    pthread_mutex_unlock(&state_lock);
                }
            }
        }
    } @catch (id thrown) {
        // Nothing to answer: the runtime has let go of the decoder either way.
    }
}

/* The waiting frame of the token, which leaves the waiting ones, or nil. */
static MinatoDecodedFrame *take_waiting(MinatoVideoDecoder *decoder, uint64_t token) {
    __block MinatoDecodedFrame *frame = nil;
    locked(decoder, ^{
        frame = decoder->waiting[@(token)];
        [decoder->waiting removeObjectForKey:@(token)];
    });
    return frame;
}

/* AHardwareBuffer's formats the render draws into: Y8Cb8Cr8_420 (an ImageReader's YUV_420_888, hardware_buffer.m's
   '420v'), and R8G8B8A8, R8G8B8X8 and PRIVATE, which hardware_buffer.m makes as 'RGBA'. */
enum { rgba_format = 1, rgbx_format = 2, private_format = 0x22, yuv_420_format = 0x23 };

/* The Metal pass's conversion: rgb = matrix * (ycbcr - offset), all in 0 to 1, as the kernel's struct. */
struct conversion {
    simd_float3x3 matrix;
    simd_float3 offset;
};

static NSString *const conversion_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct Conversion { float3x3 matrix; float3 offset; };\n"
     "kernel void minato_nv12_to_rgba(texture2d<float, access::sample> luma [[texture(0)]],\n"
     "                                texture2d<float, access::sample> chroma [[texture(1)]],\n"
     "                                texture2d<float, access::write> out [[texture(2)]],\n"
     "                                constant Conversion &conversion [[buffer(0)]],\n"
     "                                uint2 at [[thread_position_in_grid]]) {\n"
     "    if (at.x >= out.get_width() || at.y >= out.get_height()) return;\n"
     "    constexpr sampler linear(coord::normalized, filter::linear, address::clamp_to_edge);\n"
     "    const float2 place = (float2(at) + 0.5) / float2(out.get_width(), out.get_height());\n"
     "    const float3 ycbcr = float3(luma.sample(linear, place).r, chroma.sample(linear, place).rg);\n"
     "    out.write(float4(clamp(conversion.matrix * (ycbcr - conversion.offset), 0.0, 1.0), 1.0), at);\n"
     "}\n";

static id<MTLDevice> metal_device;
static id<MTLCommandQueue> metal_queue;
static id<MTLComputePipelineState> conversion_pipeline;

/* The device, its queue and the pass's pipeline, once; NO without them. */
static BOOL metal_ready(void) {
    static dispatch_once_t once;
    static BOOL ready;
    dispatch_once(&once, ^{
        metal_device = MTLCreateSystemDefaultDevice();
        metal_queue = [metal_device newCommandQueue];
        NSError *error = nil;
        id<MTLLibrary> library = [metal_device newLibraryWithSource:conversion_source options:nil error:&error];
        id<MTLFunction> kernel = [library newFunctionWithName:@"minato_nv12_to_rgba"];
        conversion_pipeline = kernel != nil ? [metal_device newComputePipelineStateWithFunction:kernel error:&error]
                                            : nil;
        ready = metal_queue != nil && conversion_pipeline != nil;
        if (!ready) {
            fprintf(stderr, "minato graphics: no video frames into RGBA buffers: %s\n",
                    error != nil ? error.localizedDescription.UTF8String : "no Metal device");
        }
    });
    return ready;
}

/* The image's conversion: its matrix by its attachment (or by its height without one) and its range by its format. */
static struct conversion conversion_of(CVPixelBufferRef image) {
    CFTypeRef matrix = CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, NULL);
    float kr = 0.299f, kb = 0.114f;  // BT.601
    if (matrix != NULL ? CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
                       : CVPixelBufferGetHeight(image) >= 720) {
        kr = 0.2126f, kb = 0.0722f;
    } else if (matrix != NULL && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_2020)) {
        kr = 0.2627f, kb = 0.0593f;
    }
    if (matrix != NULL) CFRelease(matrix);
    const BOOL full = CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    const float y_scale = full ? 1.0f : 255.0f / 219.0f, c_scale = full ? 1.0f : 255.0f / 224.0f;
    const float kg = 1.0f - kr - kb;
    struct conversion made;
    made.matrix = simd_matrix(simd_make_float3(y_scale, y_scale, y_scale),
                              simd_make_float3(0, -2 * kb * (1 - kb) / kg * c_scale, 2 * (1 - kb) * c_scale),
                              simd_make_float3(2 * (1 - kr) * c_scale, -2 * kr * (1 - kr) / kg * c_scale, 0));
    made.offset = simd_make_float3(full ? 0 : 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f);
    return made;
}

/* A texture of the IOSurface's plane, or nil. */
static id<MTLTexture> plane_texture(IOSurfaceRef surface, size_t plane, MTLPixelFormat format, MTLTextureUsage usage) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                           width:IOSurfaceGetWidthOfPlane(surface, plane)
                                                          height:IOSurfaceGetHeightOfPlane(surface, plane)
                                                       mipmapped:NO];
    descriptor.usage = usage;
    descriptor.storageMode = MTLStorageModeShared;
    return [metal_device newTextureWithDescriptor:descriptor iosurface:surface plane:plane];
}

/* The image drawn into the RGBA IOSurface by the Metal pass, scaled to its size: 0, or -EIO. */
static int convert_into(CVPixelBufferRef image, IOSurfaceRef surface) {
    IOSurfaceRef source = CVPixelBufferGetIOSurface(image);
    if (!metal_ready() || source == NULL || IOSurfaceGetPlaneCount(source) != 2) return -linux_eio;
    id<MTLTexture> luma = plane_texture(source, 0, MTLPixelFormatR8Unorm, MTLTextureUsageShaderRead);
    id<MTLTexture> chroma = plane_texture(source, 1, MTLPixelFormatRG8Unorm, MTLTextureUsageShaderRead);
    id<MTLTexture> out = plane_texture(surface, 0, MTLPixelFormatRGBA8Unorm, MTLTextureUsageShaderWrite);
    id<MTLCommandBuffer> commands = [metal_queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (luma == nil || chroma == nil || out == nil || encoder == nil) {
        [encoder endEncoding];
        return -linux_eio;
    }
    const struct conversion conversion = conversion_of(image);
    [encoder setComputePipelineState:conversion_pipeline];
    [encoder setTexture:luma atIndex:0];
    [encoder setTexture:chroma atIndex:1];
    [encoder setTexture:out atIndex:2];
    [encoder setBytes:&conversion length:sizeof conversion atIndex:0];
    const MTLSize group = MTLSizeMake(16, 16, 1);
    [encoder dispatchThreadgroups:MTLSizeMake((out.width + 15) / 16, (out.height + 15) / 16, 1)
            threadsPerThreadgroup:group];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    return commands.status == MTLCommandBufferStatusCompleted ? 0 : -linux_eio;
}

static int render(void *context, uint64_t handle, uint64_t token, const struct apkrt_video_target *target) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoVideoDecoder *decoder = find_decoder(handle);
            if (decoder == nil || !decoder->surface) return -linux_einval;
            MinatoDecodedFrame *frame = take_waiting(decoder, token);
            const uint32_t format = target != NULL ? target->description.format : 0;
            const BOOL yuv = format == yuv_420_format;
            if (frame == nil || target == NULL || target->buffers != minato_graphics_hardware_buffers()->context ||
                target->description.layers != 1 ||
                (!yuv && format != rgba_format && format != rgbx_format && format != private_format)) {
                return -linux_einval;
            }
            IOSurfaceRef surface = (IOSurfaceRef)minato_graphics_hardware_buffer_surface(target->handle);
            if (surface == NULL) return -linux_einval;
            int result = -linux_eio;
            if (!yuv) {
                result = IOSurfaceGetPixelFormat(surface) == 'RGBA' ? convert_into(frame->image, surface)
                                                                    : -linux_einval;
            } else if (IOSurfaceGetPixelFormat(surface) != '420v' || IOSurfaceGetPlaneCount(surface) != 2) {
                result = -linux_einval;  // the description's YUV 420 is graphics/host's NV12 in video range
            } else {
                CVPixelBufferRef into = NULL;
                if (decoder->transfer == NULL) VTPixelTransferSessionCreate(NULL, &decoder->transfer);
                if (decoder->transfer != NULL &&
                    CVPixelBufferCreateWithIOSurface(NULL, surface, NULL, &into) == kCVReturnSuccess) {
                    result = VTPixelTransferSessionTransferImage(decoder->transfer, frame->image, into) == noErr
                                 ? 0
                                 : -linux_eio;
                    CVPixelBufferRelease(into);
                }
            }
            CFRelease(surface);
            return result;
        }
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static void discard(void *context, uint64_t handle, uint64_t token) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoVideoDecoder *decoder = find_decoder(handle);
            if (decoder != nil) (void)take_waiting(decoder, token);
        }
    } @catch (id thrown) {
        // Nothing to answer: the token has ended for the runtime either way.
    }
}

static const struct apkrt_video_decoders functions = {create, queue, peek, pop, flush, destroy, render, discard, NULL};

const struct apkrt_video_decoders *minato_graphics_video_decoders(void) {
    return &functions;
}

int minato_graphics_set_video_decoders(void) {
    return apkrt_set_video_decoders(&functions) == 0 ? 0 : -1;
}
