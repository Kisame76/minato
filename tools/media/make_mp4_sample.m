/* ADR-0060 e2d's test files: Minato's own H.264 streams (make_h264_sample.m's, committed in media-codec-test's assets)
   in ISO BMFF, written by AVAssetWriter without re-encoding, so that a sample read from the mp4 is the stream's access
   unit and the decoder's frames are media-codec-test's:
   - baseline.mp4: baseline's 30 frames (avc1, each access unit's parameter sets in its avcC instead) and a track of
     AAC of our own, a 440 Hz sine at 44.1 kHz, mono, 1 s, which AVAssetWriter's encoder makes;
   - main.mp4: main's 30 frames alone, decoded in an order other than shown (B-frames), so its times need composition
     offsets.
   Each access unit's index line gives its time; its decoding time is its place in the stream, moved back so that no
   frame is shown before it is decoded.
   Usage: make-mp4-sample <h264 directory> <output directory> */
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

static const int32_t kTimeScale = 30000, kFrameTicks = 1000;  // 30 frames per second

/* The NAL units of an Annex B access unit (start codes of 3 or 4 bytes), each without its start code. */
static NSArray<NSData *> *nal_units(NSData *unit) {
    NSMutableArray<NSData *> *units = [NSMutableArray array];
    const uint8_t *bytes = unit.bytes;
    const NSUInteger length = unit.length;
    NSUInteger start = NSNotFound;
    for (NSUInteger at = 0; at + 3 <= length;) {
        if (bytes[at] == 0 && bytes[at + 1] == 0 && bytes[at + 2] == 1) {
            if (start != NSNotFound) {
                NSUInteger end = at;
                while (end > start && bytes[end - 1] == 0) end--;
                [units addObject:[unit subdataWithRange:NSMakeRange(start, end - start)]];
            }
            at += 3;
            start = at;
        } else {
            at++;
        }
    }
    if (start != NSNotFound && start < length) {
        [units addObject:[unit subdataWithRange:NSMakeRange(start, length - start)]];
    }
    return units;
}

/* The access units of `name`.h264 by `name`.idx: each as {NSData, pts in us, key}. */
static NSArray<NSArray *> *access_units(NSString *directory, NSString *name) {
    NSData *stream = [NSData dataWithContentsOfFile:[directory stringByAppendingFormat:@"/%@.h264", name]];
    NSString *index = [NSString stringWithContentsOfFile:[directory stringByAppendingFormat:@"/%@.idx", name]
                                                encoding:NSUTF8StringEncoding
                                                   error:NULL];
    if (stream == nil || index == nil) return nil;
    NSMutableArray<NSArray *> *units = [NSMutableArray array];
    for (NSString *line in [index componentsSeparatedByString:@"\n"]) {
        NSArray<NSString *> *fields = [line componentsSeparatedByString:@" "];
        if (fields.count != 4) continue;
        const NSRange range = NSMakeRange((NSUInteger)fields[0].longLongValue, (NSUInteger)fields[1].longLongValue);
        if (NSMaxRange(range) > stream.length) return nil;
        [units addObject:@[ [stream subdataWithRange:range], @(fields[2].longLongValue), @(fields[3].intValue) ]];
    }
    return units;
}

/* Appends once the input takes more. */
static BOOL append(AVAssetWriterInput *input, CMSampleBufferRef sample) {
    while (!input.readyForMoreMediaData) usleep(1000);
    return [input appendSampleBuffer:sample];
}

/* The video track of `units`: each access unit without its parameter sets, its NAL units 4-byte length prefixed. */
static BOOL write_video(AVAssetWriterInput *input, NSArray<NSArray *> *units, CMVideoFormatDescriptionRef format) {
    // Decoding times: a frame's place in the stream, moved back by the most any frame is shown before its place.
    int64_t shift = 0;
    for (NSUInteger place = 0; place < units.count; place++) {
        const int64_t frame = ([units[place][1] longLongValue] * 30 + 500000) / 1000000;
        shift = MAX(shift, (int64_t)place - frame);
    }
    for (NSUInteger place = 0; place < units.count; place++) {
        NSMutableData *avcc = [NSMutableData data];
        for (NSData *nal in nal_units(units[place][0])) {
            const uint8_t type = ((const uint8_t *)nal.bytes)[0] & 0x1f;
            if (type == 7 || type == 8 || type == 9) continue;  // SPS and PPS in the avcC, no access unit delimiter
            const uint32_t size = CFSwapInt32HostToBig((uint32_t)nal.length);
            [avcc appendBytes:&size length:4];
            [avcc appendData:nal];
        }
        CMBlockBufferRef block = NULL;
        if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, avcc.length, NULL, NULL, 0, avcc.length, 0, &block) !=
                noErr ||
            CMBlockBufferReplaceDataBytes(avcc.bytes, block, 0, avcc.length) != noErr) {
            return NO;
        }
        const int64_t frame = ([units[place][1] longLongValue] * 30 + 500000) / 1000000;
        const CMSampleTimingInfo timing = {CMTimeMake(kFrameTicks, kTimeScale),
                                           CMTimeMake(frame * kFrameTicks, kTimeScale),
                                           CMTimeMake(((int64_t)place - shift) * kFrameTicks, kTimeScale)};
        const size_t size = avcc.length;
        CMSampleBufferRef sample = NULL;
        const OSStatus made = CMSampleBufferCreateReady(NULL, block, format, 1, 1, &timing, 1, &size, &sample);
        CFRelease(block);
        if (made != noErr) return NO;
        if (![units[place][2] intValue]) {
            CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, YES);
            CFMutableDictionaryRef first = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
            CFDictionarySetValue(first, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
        }
        const BOOL appended = append(input, sample);
        CFRelease(sample);
        if (!appended) return NO;
    }
    [input markAsFinished];
    return YES;
}

/* A second of a 440 Hz sine, 16-bit mono PCM at 44.1 kHz, in buffers of 1024 frames, for the writer's AAC encoder. */
static BOOL write_tone(AVAssetWriterInput *input) {
    const double rate = 44100;
    AudioStreamBasicDescription pcm = {rate, kAudioFormatLinearPCM,
                                       kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, 2, 1, 2, 1,
                                       16, 0};
    CMAudioFormatDescriptionRef format = NULL;
    if (CMAudioFormatDescriptionCreate(NULL, &pcm, 0, NULL, 0, NULL, NULL, &format) != noErr) return NO;
    BOOL fine = YES;
    for (int64_t first = 0; first < (int64_t)rate && fine; first += 1024) {
        const int64_t count = MIN(1024, (int64_t)rate - first);
        NSMutableData *samples = [NSMutableData dataWithLength:(NSUInteger)count * 2];
        int16_t *each = samples.mutableBytes;
        for (int64_t frame = 0; frame < count; frame++) {
            each[frame] = (int16_t)(sin(2 * M_PI * 440 * (double)(first + frame) / rate) * 12000);
        }
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        fine = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, samples.length, NULL, NULL, 0, samples.length, 0,
                                                  &block) == noErr &&
               CMBlockBufferReplaceDataBytes(samples.bytes, block, 0, samples.length) == noErr &&
               CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, block, format, (CMItemCount)count,
                                                                    CMTimeMake(first, (int32_t)rate), NULL,
                                                                    &sample) == noErr &&
               append(input, sample);
        if (block != NULL) CFRelease(block);
        if (sample != NULL) CFRelease(sample);
    }
    CFRelease(format);
    [input markAsFinished];
    return fine;
}

static int make(NSString *from, NSString *to, NSString *name, BOOL tone) {
    NSArray<NSArray *> *units = access_units(from, name);
    if (units.count == 0) {
        fprintf(stderr, "make-mp4-sample: no access units of %s\n", name.UTF8String);
        return 1;
    }
    NSData *sps = nil, *pps = nil;
    for (NSData *nal in nal_units(units[0][0])) {
        const uint8_t type = ((const uint8_t *)nal.bytes)[0] & 0x1f;
        if (type == 7 && sps == nil) sps = nal;
        if (type == 8 && pps == nil) pps = nal;
    }
    const uint8_t *sets[] = {sps.bytes, pps.bytes};
    const size_t sizes[] = {sps.length, pps.length};
    CMVideoFormatDescriptionRef format = NULL;
    if (sps == nil || pps == nil ||
        CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &format) != noErr) {
        fprintf(stderr, "make-mp4-sample: no parameter sets in %s\n", name.UTF8String);
        return 1;
    }
    NSURL *url = [NSURL fileURLWithPath:[to stringByAppendingFormat:@"/%@.mp4", name]];
    [[NSFileManager defaultManager] removeItemAtURL:url error:NULL];
    NSError *error = nil;
    AVAssetWriter *writer = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeMPEG4 error:&error];
    AVAssetWriterInput *video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                   outputSettings:nil
                                                                 sourceFormatHint:format];
    video.mediaTimeScale = kTimeScale;
    video.expectsMediaDataInRealTime = NO;
    AVAssetWriterInput *audio = nil;
    if (writer == nil || ![writer canAddInput:video]) {
        fprintf(stderr, "make-mp4-sample: no writer: %s\n", error.localizedDescription.UTF8String);
        return 1;
    }
    [writer addInput:video];
    if (tone) {
        audio = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                   outputSettings:@{
                                                       AVFormatIDKey : @(kAudioFormatMPEG4AAC),
                                                       AVSampleRateKey : @44100,
                                                       AVNumberOfChannelsKey : @1,
                                                       AVEncoderBitRateKey : @64000
                                                   }];
        audio.expectsMediaDataInRealTime = NO;
        [writer addInput:audio];
    }
    if (![writer startWriting]) {
        fprintf(stderr, "make-mp4-sample: %s\n", writer.error.localizedDescription.UTF8String);
        return 1;
    }
    [writer startSessionAtSourceTime:kCMTimeZero];
    BOOL fine = write_video(video, units, format);
    if (fine && audio != nil) fine = write_tone(audio);
    CFRelease(format);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (!fine || writer.status != AVAssetWriterStatusCompleted) {
        fprintf(stderr, "make-mp4-sample: %s failed: %s\n", name.UTF8String,
                writer.error.localizedDescription.UTF8String);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "Usage: make-mp4-sample <h264 directory> <output directory>\n");
            return 2;
        }
        NSString *from = @(argv[1]), *to = @(argv[2]);
        return make(from, to, @"baseline", YES) || make(from, to, @"main", NO);
    }
}
