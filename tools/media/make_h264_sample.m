/* make-h264-sample (ADR-0060 e2b): Minato's own H.264 test streams, made with VideoToolbox's encoder on the Mac, so
   that the emulator and Minato decode the same bytes and no third-party media is needed. 320 x 180 pixels (coded
   192 rows, so the decoder crops), 30 frames at 30 per second: a gradient with a white block that moves 8 pixels a
   frame and a block whose color changes each frame. Two streams:
   - baseline: Baseline profile, no reordering, a key frame at frames 0 and 15;
   - main: Main profile with frame reordering allowed (B-frames), key frames as above.
   Each is written as Annex B (<name>.h264: every access unit in decode order, the parameter sets before each key
   frame, 4-byte start codes) and an index (<name>.idx: one line per access unit, "<offset> <size> <pts in us>
   <1 for a key frame, else 0>"), which media-codec-test reads to queue one access unit per input buffer.
   Usage: make-h264-sample <output directory> */
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

static const int kWidth = 320, kHeight = 180, kFrames = 30, kRate = 30;

/* The frame's pixels, BGRA: a gradient, a white 32 x 32 block at (8 x frame, 40), and a 48 x 48 block at (240, 100)
   whose color steps with the frame. */
static CVPixelBufferRef frame_pixels(int frame) {
    NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    CVPixelBufferRef pixels = NULL;
    if (CVPixelBufferCreate(NULL, kWidth, kHeight, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attributes,
                            &pixels) != kCVReturnSuccess) {
        return NULL;
    }
    CVPixelBufferLockBaseAddress(pixels, 0);
    uint8_t *base = CVPixelBufferGetBaseAddress(pixels);
    const size_t row = CVPixelBufferGetBytesPerRow(pixels);
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) {
            uint8_t *pixel = base + (size_t)y * row + (size_t)x * 4;
            int r = x * 255 / kWidth, g = y * 255 / kHeight, b = 128;
            if (x >= 8 * frame && x < 8 * frame + 32 && y >= 40 && y < 72) {
                r = g = b = 255;
            } else if (x >= 240 && x < 288 && y >= 100 && y < 148) {
                r = (frame * 37) % 256, g = (frame * 91) % 256, b = (255 - frame * 8) % 256;
            }
            pixel[0] = (uint8_t)b, pixel[1] = (uint8_t)g, pixel[2] = (uint8_t)r, pixel[3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(pixels, 0);
    return pixels;
}

@interface Stream : NSObject
@property NSMutableData *bytes;
@property NSMutableString *index;
@property OSStatus status;
@end
@implementation Stream
@end

static void append_start_code(NSMutableData *bytes) {
    static const uint8_t start[4] = {0, 0, 0, 1};
    [bytes appendBytes:start length:sizeof start];
}

/* Appends one encoded frame as an Annex B access unit and its index line. */
static void encoded(void *context, void *frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    (void)frame, (void)flags;
    Stream *stream = (__bridge Stream *)context;
    if (status != noErr || sample == NULL) {
        stream.status = status != noErr ? status : -1;
        return;
    }
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    const BOOL key = attachments == NULL || CFArrayGetCount(attachments) == 0 ||
                     !CFDictionaryContainsKey(CFArrayGetValueAtIndex(attachments, 0), kCMSampleAttachmentKey_NotSync);
    const NSUInteger offset = stream.bytes.length;
    if (key) {
        CMFormatDescriptionRef description = CMSampleBufferGetFormatDescription(sample);
        size_t count = 0;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, 0, NULL, NULL, &count, NULL);
        for (size_t index = 0; index < count; index++) {
            const uint8_t *set = NULL;
            size_t size = 0;
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, index, &set, &size, NULL, NULL);
            append_start_code(stream.bytes);
            [stream.bytes appendBytes:set length:size];
        }
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t length = 0;
    char *data = NULL;
    if (CMBlockBufferGetDataPointer(block, 0, NULL, &length, &data) != kCMBlockBufferNoErr) {
        stream.status = -2;
        return;
    }
    // The encoder's NAL units carry 4-byte big-endian lengths, which become start codes.
    for (size_t at = 0; at + 4 <= length;) {
        const uint32_t size = ((uint32_t)(uint8_t)data[at] << 24) | ((uint32_t)(uint8_t)data[at + 1] << 16) |
                              ((uint32_t)(uint8_t)data[at + 2] << 8) | (uint32_t)(uint8_t)data[at + 3];
        if (size > length - at - 4) {
            stream.status = -3;
            return;
        }
        append_start_code(stream.bytes);
        [stream.bytes appendBytes:data + at + 4 length:size];
        at += 4 + size;
    }
    const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
    [stream.index appendFormat:@"%lu %lu %lld %d\n", (unsigned long)offset,
                               (unsigned long)(stream.bytes.length - offset),
                               (long long)(CMTimeGetSeconds(pts) * 1000000.0 + 0.5), key ? 1 : 0];
}

static int make(NSString *directory, NSString *name, CFStringRef profile, BOOL reorder) {
    Stream *stream = [Stream new];
    stream.bytes = [NSMutableData data];
    stream.index = [NSMutableString string];
    VTCompressionSessionRef session = NULL;
    OSStatus status = VTCompressionSessionCreate(NULL, kWidth, kHeight, kCMVideoCodecType_H264, NULL, NULL, NULL,
                                                 encoded, (__bridge void *)stream, &session);
    if (status != noErr) {
        fprintf(stderr, "make-h264-sample: no H.264 encoder (%d)\n", (int)status);
        return 1;
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, profile);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, reorder ? kCFBooleanTrue
                                                                                          : kCFBooleanFalse);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFTypeRef) @15);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef) @400000);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFTypeRef) @(kRate));
    for (int frame = 0; frame < kFrames && stream.status == noErr; frame++) {
        CVPixelBufferRef pixels = frame_pixels(frame);
        if (pixels == NULL) {
            stream.status = -4;
            break;
        }
        NSDictionary *properties = frame % 15 == 0 ? @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES} : nil;
        status = VTCompressionSessionEncodeFrame(session, pixels, CMTimeMake(frame, kRate), CMTimeMake(1, kRate),
                                                 (__bridge CFDictionaryRef)properties, NULL, NULL);
        CVPixelBufferRelease(pixels);
        if (status != noErr) {
            stream.status = status;
        }
    }
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    if (stream.status != noErr) {
        fprintf(stderr, "make-h264-sample: %s failed (%d)\n", name.UTF8String, (int)stream.status);
        return 1;
    }
    NSString *base = [directory stringByAppendingPathComponent:name];
    if (![stream.bytes writeToFile:[base stringByAppendingString:@".h264"] atomically:YES] ||
        ![stream.index writeToFile:[base stringByAppendingString:@".idx"] atomically:YES
                          encoding:NSUTF8StringEncoding
                             error:NULL]) {
        fprintf(stderr, "make-h264-sample: cannot write %s\n", base.UTF8String);
        return 1;
    }
    printf("%s: %lu bytes, %lu access units\n", name.UTF8String, (unsigned long)stream.bytes.length,
           (unsigned long)[stream.index componentsSeparatedByString:@"\n"].count - 1);
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "Usage: make-h264-sample <output directory>\n");
            return 2;
        }
        NSString *directory = @(argv[1]);
        return make(directory, @"baseline", kVTProfileLevel_H264_Baseline_AutoLevel, NO) ||
               make(directory, @"main", kVTProfileLevel_H264_Main_AutoLevel, YES);
    }
}
