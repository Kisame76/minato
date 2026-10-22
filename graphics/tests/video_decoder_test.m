/* graphics.video_decoder (ADR-0060 e2b3): graphics/host's video decoders (video_decoder.m), VideoToolbox behind the
   functions minato_graphics_set_video_decoders gives the runtime, without the runtime: Minato's own streams
   (media-codec-test's assets) decoded frame by frame, each frame's time and the CRC32 of its Y, Cb and Cr samples
   inside the crop against Android 15's lines (media-codec-test.logcat, AOSP's c2.android.avc.decoder); the order of
   presentation for B-frames, the end of the stream, a flush and the stream again from its second key frame, a second
   PPS of another ID before every unit, a flood of one-byte units, garbage that decodes to nothing without harm, the
   types refused, and a destroy with frames and input still held.
   Usage: graphics-video-decoder-test <assets directory> <media-codec-test.logcat>. Exits 1 after the cases when one
   failed. */
#import <Foundation/Foundation.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "minato_graphics.h"

enum { linux_eagain = 11, linux_einval = 22 };

static const struct apkrt_video_decoders *table;
static int failures;

static void fail(const char *test, NSString *what) {
    fprintf(stderr, "FAILED  %s: %s\n", test, what.UTF8String);
    failures++;
}

/* A stream and its access units: offset, size, time, key. */
struct stream {
    NSData *bytes;
    NSMutableArray<NSArray<NSNumber *> *> *units;
};

static struct stream load(NSString *assets, NSString *name) {
    struct stream stream = {[NSData dataWithContentsOfFile:[assets stringByAppendingPathComponent:
                                                                        [name stringByAppendingString:@".h264"]]],
                            [NSMutableArray array]};
    NSString *index = [NSString stringWithContentsOfFile:[assets stringByAppendingPathComponent:
                                                                     [name stringByAppendingString:@".idx"]]
                                                encoding:NSUTF8StringEncoding
                                                   error:NULL];
    for (NSString *line in [index componentsSeparatedByString:@"\n"]) {
        NSArray<NSString *> *parts = [line componentsSeparatedByString:@" "];
        if (parts.count == 4) {
            [stream.units addObject:@[ @(parts[0].longLongValue), @(parts[1].longLongValue),
                                       @(parts[2].longLongValue), @(parts[3].longLongValue) ]];
        }
    }
    return stream;
}

/* Android's frame lines of one part of the log ("baseline", "main", ...): "pts <t> ... Y <y> U <u> V <v>". */
static NSArray<NSString *> *expected_frames(NSString *log, NSString *part) {
    NSMutableArray<NSString *> *frames = [NSMutableArray array];
    NSString *prefix = [NSString stringWithFormat:@"I/media-codec-test: %@: pts ", part];
    for (NSString *line in [log componentsSeparatedByString:@"\n"]) {
        if (![line hasPrefix:prefix]) continue;
        NSArray<NSString *> *words = [[line substringFromIndex:prefix.length] componentsSeparatedByString:@" "];
        // <t>, flags <f>, <w> x <h>, format 35, Y <y> U <u> V <v>
        if (words.count < 14) continue;
        [frames addObject:[NSString stringWithFormat:@"pts %@ Y %@ U %@ V %@",
                                                     [words[0] stringByTrimmingCharactersInSet:
                                                                   [NSCharacterSet punctuationCharacterSet]],
                                                     words[9], words[11], words[13]]];
    }
    return frames;
}

/* A frame's line as expected_frames has Android's: its time and the CRC32 of each plane inside the crop. */
static NSString *frame_line(const struct apkrt_video_frame *frame) {
    const uint8_t *y = frame->data, *chroma = y + (size_t)frame->stride * frame->slice_height;
    uLong crcs[3] = {crc32(0L, Z_NULL, 0), crc32(0L, Z_NULL, 0), crc32(0L, Z_NULL, 0)};
    for (uint32_t row = frame->crop_top; row <= frame->crop_bottom; row++) {
        for (uint32_t column = frame->crop_left; column <= frame->crop_right; column++) {
            crcs[0] = crc32(crcs[0], y + (size_t)row * frame->stride + column, 1);
        }
    }
    for (uint32_t row = frame->crop_top / 2; row <= frame->crop_bottom / 2; row++) {
        for (uint32_t column = frame->crop_left / 2; column <= frame->crop_right / 2; column++) {
            const uint8_t *pair = chroma + (size_t)row * frame->stride + (size_t)column * 2;
            crcs[1] = crc32(crcs[1], pair, 1);
            crcs[2] = crc32(crcs[2], pair + 1, 1);
        }
    }
    return [NSString stringWithFormat:@"pts %lld Y %lx U %lx V %lx", (long long)frame->time_us, crcs[0], crcs[1],
                                      crcs[2]];
}

/* Takes every frame the decoder has into `lines`; true once the end of the stream came. */
static BOOL drain(uint64_t handle, NSMutableArray<NSString *> *lines) {
    struct apkrt_video_frame frame;
    while (table->peek(table->context, handle, &frame) == 0) {
        const BOOL end = (frame.flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0;
        if (frame.size > 0) {
            [lines addObject:frame_line(&frame)];
        }
        table->pop(table->context, handle);
        if (end) {
            return YES;
        }
    }
    return NO;
}

/* Queues units [first, last) of the stream, taking frames whenever the decoder asks for it; with `end`, the end of
   the stream after them and every frame up to it. */
static NSMutableArray<NSString *> *decode_units(uint64_t handle, struct stream stream, NSUInteger first,
                                                NSUInteger last, BOOL end, BOOL *ended) {
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    *ended = NO;
    for (NSUInteger index = first; index < last; index++) {
        NSArray<NSNumber *> *unit = stream.units[index];
        const uint8_t *bytes = (const uint8_t *)stream.bytes.bytes + unit[0].longLongValue;
        int queued;
        for (int tries = 0; (queued = table->queue(table->context, handle, bytes, (size_t)unit[1].longLongValue,
                                                   unit[2].longLongValue, 0)) == -linux_eagain && tries < 1000;
             tries++) {
            drain(handle, lines);
            usleep(1000);
        }
        if (queued != 0) {
            [lines addObject:[NSString stringWithFormat:@"queue %d", queued]];
            return lines;
        }
        drain(handle, lines);
    }
    if (end) {
        const int queued = table->queue(table->context, handle, NULL, 0, 0, APKRT_VIDEO_DECODER_END_OF_STREAM);
        if (queued != 0) {
            [lines addObject:[NSString stringWithFormat:@"end %d", queued]];
            return lines;
        }
        for (int tries = 0; !*ended && tries < 5000; tries++) {
            *ended = drain(handle, lines);
            if (!*ended) usleep(1000);
        }
    }
    return lines;
}

static uint64_t make(uint32_t type) {
    const struct apkrt_video_decoder_description description = {type, 320, 180, NULL, 0};
    uint64_t handle = 0;
    return table->create(table->context, &description, &handle) == 0 ? handle : 0;
}

static void compare(const char *test, NSArray<NSString *> *got, NSArray<NSString *> *want) {
    if (want.count == 0) {
        fail(test, @"no lines of Android's");
        return;
    }
    if (![got isEqualToArray:want]) {
        for (NSUInteger index = 0; index < (got.count > want.count ? got.count : want.count); index++) {
            NSString *ours = index < got.count ? got[index] : @"(none)";
            NSString *theirs = index < want.count ? want[index] : @"(none)";
            if (![ours isEqualToString:theirs]) {
                fail(test, [NSString stringWithFormat:@"line %lu: ours \"%@\", Android's \"%@\" (%lu and %lu lines)",
                                                      (unsigned long)index, ours, theirs, (unsigned long)got.count,
                                                      (unsigned long)want.count]);
                return;
            }
        }
    }
}

static void bothStreamsDecodeAsAndroidDecodesThem(NSString *assets, NSString *log) {
    for (NSString *name in @[ @"baseline", @"main" ]) {
        const char *test = name.UTF8String;
        const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
        if (handle == 0) {
            fail(test, @"no decoder");
            continue;
        }
        const struct stream stream = load(assets, name);
        BOOL ended = NO;
        NSArray<NSString *> *lines = decode_units(handle, stream, 0, stream.units.count, YES, &ended);
        if (!ended) fail(test, @"no end of the stream");
        compare(test, lines, expected_frames(log, name));
        table->destroy(table->context, handle);
    }
}

static void aFlushDropsWhatCameAndTheStreamGoesOnFromAKeyFrame(NSString *assets, NSString *log) {
    const char *test = "a flush drops what came, and the stream goes on from a key frame";
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    const struct stream stream = load(assets, @"baseline");
    BOOL ended = NO;
    (void)decode_units(handle, stream, 0, 15, NO, &ended);
    if (table->flush(table->context, handle) != 0) fail(test, @"flush refused");
    struct apkrt_video_frame frame;
    if (table->peek(table->context, handle, &frame) != -linux_eagain) fail(test, @"a frame after the flush");
    NSArray<NSString *> *lines = decode_units(handle, stream, 15, stream.units.count, YES, &ended);
    if (!ended) fail(test, @"no end of the stream");
    compare(test, lines, expected_frames(log, @"baseline flushed after"));
    table->destroy(table->context, handle);
}

/* Baseline's stream with a second PPS, ID 1, before every access unit: its bytes are the stream's PPS with its ID's
   Exp-Golomb code 1 ('1') written as 010 (ID 1), which no slice names. The frames must stay Android's, so the decoder
   keeps both sets by their IDs. */
static void aSecondPpsOfAnotherIdChangesNothing(NSString *assets, NSString *log) {
    const char *test = "a second PPS of another ID changes nothing";
    const struct stream plain = load(assets, @"baseline");
    const uint8_t *bytes = plain.bytes.bytes;
    // The stream's PPS: the NAL unit of type 8 after the first start code that holds one.
    NSData *pps = nil;
    for (NSUInteger at = 0; at + 4 < plain.bytes.length && pps == nil; at++) {
        if (bytes[at] == 0 && bytes[at + 1] == 0 && bytes[at + 2] == 1 && (bytes[at + 3] & 0x1f) == 8) {
            NSUInteger end = at + 3;
            while (end + 3 <= plain.bytes.length && !(bytes[end] == 0 && bytes[end + 1] == 0)) end++;
            pps = [plain.bytes subdataWithRange:NSMakeRange(at + 3, end - at - 3)];
        }
    }
    if (pps == nil || pps.length < 2 || (((const uint8_t *)pps.bytes)[1] & 0x80) == 0) {
        fail(test, @"no PPS of ID 0 in the stream");
        return;
    }
    // The payload's bits with '01' before them: ue(0) '1' becomes ue(1) '010'.
    const uint8_t *in = pps.bytes;
    NSMutableData *other = [NSMutableData dataWithBytes:in length:1];
    uint32_t bits = 1, count = 2;  // '01'
    for (NSUInteger index = 1; index < pps.length; index++) {
        for (int bit = 7; bit >= 0; bit--) {
            bits = (bits << 1) | ((in[index] >> bit) & 1u), count++;
            if (count == 8) {
                const uint8_t byte = (uint8_t)bits;
                [other appendBytes:&byte length:1];
                bits = 0, count = 0;
            }
        }
    }
    if (count > 0) {
        const uint8_t byte = (uint8_t)(bits << (8 - count));
        [other appendBytes:&byte length:1];
    }
    struct stream doubled = {nil, [NSMutableArray array]};
    NSMutableData *joined = [NSMutableData data];
    static const uint8_t start[4] = {0, 0, 0, 1};
    for (NSArray<NSNumber *> *unit in plain.units) {
        const NSUInteger offset = joined.length;
        [joined appendBytes:start length:4];
        [joined appendData:other];
        [joined appendBytes:bytes + unit[0].longLongValue length:(NSUInteger)unit[1].longLongValue];
        [doubled.units addObject:@[ @(offset), @(joined.length - offset), unit[2], unit[3] ]];
    }
    doubled.bytes = joined;
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    BOOL ended = NO;
    NSArray<NSString *> *lines = decode_units(handle, doubled, 0, doubled.units.count, YES, &ended);
    if (!ended) fail(test, @"no end of the stream");
    compare(test, lines, expected_frames(log, @"baseline"));
    table->destroy(table->context, handle);
}

/* A unit of 4 MiB of one-byte NAL units, each its own start code and an access unit delimiter: read through without
   harm or a pile of objects. */
static void aFloodOfTinyUnitsDecodesToNothing(void) {
    const char *test = "a flood of tiny units decodes to nothing";
    const size_t size = (size_t)4 << 20;
    uint8_t *flood = malloc(size);
    for (size_t at = 0; at + 4 <= size; at += 4) flood[at] = 0, flood[at + 1] = 0, flood[at + 2] = 1, flood[at + 3] = 9;
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    const int queued = table->queue(table->context, handle, flood, size, 0, 0);
    free(flood);
    struct apkrt_video_frame frame;
    if (queued != 0 || table->peek(table->context, handle, &frame) != -linux_eagain) {
        fail(test, [NSString stringWithFormat:@"queue %d, or a frame", queued]);
    }
    table->destroy(table->context, handle);
}

static void garbageDecodesToNothingWithoutHarm(void) {
    const char *test = "garbage decodes to nothing without harm";
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    uint8_t junk[4096];
    uint32_t seed = 12345;
    for (int round = 0; round < 50; round++) {
        for (size_t at = 0; at < sizeof junk; at++) {
            seed = seed * 1103515245u + 12345u;
            junk[at] = (uint8_t)(seed >> 16);
        }
        if (round % 2 == 0) junk[0] = 0, junk[1] = 0, junk[2] = 0, junk[3] = 1;  // a start code, then noise
        const int queued = table->queue(table->context, handle, junk, (size_t)(seed % sizeof junk), round, 0);
        if (queued != 0 && queued != -linux_einval) {
            fail(test, [NSString stringWithFormat:@"round %d: queue %d", round, queued]);
        }
    }
    BOOL ended = NO;
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    if (table->queue(table->context, handle, NULL, 0, 0, APKRT_VIDEO_DECODER_END_OF_STREAM) != 0) {
        fail(test, @"the end refused");
    }
    for (int tries = 0; !ended && tries < 5000; tries++) {
        ended = drain(handle, lines);
        if (!ended) usleep(1000);
    }
    if (!ended) fail(test, @"no end of the stream");
    table->destroy(table->context, handle);
}

static void typesItDoesNotDecodeAreRefused(void) {
    const char *test = "types it does not decode are refused";
    const struct apkrt_video_decoder_description description = {99, 320, 180, NULL, 0};
    uint64_t handle = 0;
    if (table->create(table->context, &description, &handle) != -linux_einval || handle != 0) {
        fail(test, @"type 99 made");
    }
    const uint64_t hevc = make(APKRT_VIDEO_DECODER_HEVC);
    if (hevc == 0) {
        fail(test, @"no HEVC decoder");
    } else {
        table->destroy(table->context, hevc);
    }
}

static void aDestroyWithFramesAndInputHeldFreesThem(NSString *assets) {
    const char *test = "a destroy with frames and input held frees them";
    const struct stream stream = load(assets, @"main");
    for (int round = 0; round < 20; round++) {
        const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
        for (NSUInteger index = 0; index < (NSUInteger)(round % 30) + 1; index++) {
            NSArray<NSNumber *> *unit = stream.units[index];
            if (table->queue(table->context, handle, (const uint8_t *)stream.bytes.bytes + unit[0].longLongValue,
                             (size_t)unit[1].longLongValue, unit[2].longLongValue, 0) == -linux_eagain) {
                break;
            }
        }
        table->destroy(table->context, handle);
    }
    (void)test;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "Usage: graphics-video-decoder-test <assets directory> <media-codec-test.logcat>\n");
            return 2;
        }
        table = minato_graphics_video_decoders();
        if (table == NULL) {
            fprintf(stderr, "FAILED  no video decoders\n");
            return 1;
        }
        NSString *assets = @(argv[1]);
        NSString *log = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:NULL];
        bothStreamsDecodeAsAndroidDecodesThem(assets, log);
        aFlushDropsWhatCameAndTheStreamGoesOnFromAKeyFrame(assets, log);
        aSecondPpsOfAnotherIdChangesNothing(assets, log);
        aFloodOfTinyUnitsDecodesToNothing();
        garbageDecodesToNothingWithoutHarm();
        typesItDoesNotDecodeAreRefused();
        aDestroyWithFramesAndInputHeldFreesThem(assets);
        printf("graphics.video_decoder: 7 cases, %d failures\n", failures);
        return failures == 0 ? 0 : 1;
    }
}
