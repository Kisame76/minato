/* graphics.video_decoder (ADR-0060 e2b3): graphics/host's video decoders (video_decoder.m), VideoToolbox behind the
   functions minato_graphics_set_video_decoders gives the runtime, without the runtime: Minato's own streams
   (media-codec-test's assets) decoded frame by frame, each frame's time and the CRC32 of its Y, Cb and Cr samples
   inside the crop against Android 15's lines (media-codec-test.logcat, AOSP's c2.android.avc.decoder); the order of
   presentation for B-frames, the end of the stream, a flush and the stream again from its second key frame, a second
   PPS of another ID before every unit, a flood of one-byte units, garbage that decodes to nothing without harm, the
   types refused, a destroy with frames and input still held, parameter sets of every ID, the reorder depth of the
   active SPS, and a new session after the old one's frames. graphics.video_decoder_asan runs the cases with the
   decoders' own sources under AddressSanitizer and VideoToolbox's decode call replaced (MINATO_VIDEO_DECODER_TESTING),
   for three more: units whose call back never comes with a session iOS ended, units VideoToolbox holds with the end
   of the stream after them, exceptions, and surfaces the Metal pass cannot wrap; and a frame drawn into a host
   window (ADR-0060 e2c3), whose drawable's pixels only the testing build reads back.
   Usage: graphics-video-decoder-test <assets directory> <media-codec-test.logcat>. Exits 1 after the cases when one
   failed. */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "minato_graphics.h"
#include "video_decoder_testing.h"

enum { linux_eio = 5, linux_eagain = 11, linux_enomem = 12, linux_enodev = 19, linux_einval = 22 };

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

/* Android's frame lines of one part of the log ("baseline", "main", "baseline reader", ...):
   "pts <t> ... Y <y> U <u> V <v>". */
static NSArray<NSString *> *expected_frames(NSString *log, NSString *part) {
    NSMutableArray<NSString *> *frames = [NSMutableArray array];
    NSString *prefix = [NSString stringWithFormat:@"I/media-codec-test: %@: pts ", part];
    for (NSString *line in [log componentsSeparatedByString:@"\n"]) {
        if (![line hasPrefix:prefix]) continue;
        NSArray<NSString *> *words = [[line substringFromIndex:prefix.length] componentsSeparatedByString:@" "];
        // <t>, [flags <f>,] <w> x <h>, format 35, Y <y> U <u> V <v>
        const NSUInteger y = [words indexOfObject:@"Y"];
        if (y == NSNotFound || y + 5 >= words.count) continue;
        [frames addObject:[NSString stringWithFormat:@"pts %@ Y %@ U %@ V %@",
                                                     [words[0] stringByTrimmingCharactersInSet:
                                                                   [NSCharacterSet punctuationCharacterSet]],
                                                     words[y + 1], words[y + 3], words[y + 5]]];
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

static uint64_t make_with(uint32_t type, uint32_t flags) {
    const struct apkrt_video_decoder_description description = {type, 320, 180, NULL, 0, flags};
    uint64_t handle = 0;
    return table->create(table->context, &description, &handle) == 0 ? handle : 0;
}

static uint64_t make(uint32_t type) {
    const struct apkrt_video_decoder_description description = {type, 320, 180, NULL, 0, 0};
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

/* The stream's first NAL unit of H.264 type `type`, without its start code, or nil. */
static NSData *first_nal(struct stream stream, unsigned type) {
    const uint8_t *bytes = stream.bytes.bytes;
    for (NSUInteger at = 0; at + 4 < stream.bytes.length; at++) {
        if (bytes[at] == 0 && bytes[at + 1] == 0 && bytes[at + 2] == 1 && (bytes[at + 3] & 0x1f) == type) {
            NSUInteger end = at + 3;
            while (end + 3 <= stream.bytes.length && !(bytes[end] == 0 && bytes[end + 1] == 0)) end++;
            return [stream.bytes subdataWithRange:NSMakeRange(at + 3, end - at - 3)];
        }
    }
    return nil;
}

/* A parameter set of ID 0 with its ID's Exp-Golomb code at byte `at` ('1', ID 0) written as `id`'s: the set's bits
   after it move, so that the set is the same but for its ID; nil for a set whose ID is not 0. */
static NSData *with_id(NSData *set, NSUInteger at, uint32_t id) {
    const uint8_t *in = set.bytes;
    if (set.length <= at || (in[at] & 0x80) == 0) return nil;
    NSMutableData *out = [NSMutableData dataWithBytes:in length:at];
    __block uint32_t bits = 0, count = 0;
    void (^put)(uint32_t, uint32_t) = ^(uint32_t value, uint32_t width) {
        for (uint32_t n = width; n > 0; n--) {
            bits = (bits << 1) | ((value >> (n - 1)) & 1u), count++;
            if (count == 8) {
                const uint8_t byte = (uint8_t)bits;
                [out appendBytes:&byte length:1];
                bits = 0, count = 0;
            }
        }
    };
    uint32_t width = 0;
    while (((id + 1) >> width) > 1) width++;
    put(0, width), put(id + 1, width + 1);  // ue(id) for ID 0's '1'
    for (NSUInteger index = at; index < set.length; index++) {
        for (int bit = index == at ? 6 : 7; bit >= 0; bit--) put((in[index] >> bit) & 1u, 1);
    }
    if (count > 0) {
        const uint8_t byte = (uint8_t)(bits << (8 - count));
        [out appendBytes:&byte length:1];
    }
    return out;
}

/* `plain` with `extra` before (or after) every access unit, as one stream. */
static struct stream with_units(struct stream plain, NSData *extra, BOOL after) {
    struct stream joined_stream = {nil, [NSMutableArray array]};
    NSMutableData *joined = [NSMutableData data];
    static const uint8_t start[4] = {0, 0, 0, 1};
    const uint8_t *bytes = plain.bytes.bytes;
    for (NSArray<NSNumber *> *unit in plain.units) {
        const NSUInteger offset = joined.length;
        if (!after) [joined appendBytes:start length:4], [joined appendData:extra];
        [joined appendBytes:bytes + unit[0].longLongValue length:(NSUInteger)unit[1].longLongValue];
        if (after) [joined appendBytes:start length:4], [joined appendData:extra];
        [joined_stream.units addObject:@[ @(offset), @(joined.length - offset), unit[2], unit[3] ]];
    }
    joined_stream.bytes = joined;
    return joined_stream;
}

/* Baseline's stream with a second PPS, ID 1, before every access unit: its bytes are the stream's PPS with its ID
   written as 1, which no slice names. The frames must stay Android's, so the decoder keeps both sets by their IDs. */
static void aSecondPpsOfAnotherIdChangesNothing(NSString *assets, NSString *log) {
    const char *test = "a second PPS of another ID changes nothing";
    const struct stream plain = load(assets, @"baseline");
    NSData *other = with_id(first_nal(plain, 8), 1, 1);
    if (other == nil) {
        fail(test, @"no PPS of ID 0 in the stream");
        return;
    }
    const struct stream doubled = with_units(plain, other, NO);
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

/* Baseline's stream after a unit of its SPS and PPS under every ID up to 63 and 299: those of the standard's IDs
   (SPSs to 31, PPSs to 255) are kept, the others refused, so that the format description's list of them never runs
   over. VideoToolbox takes at most 31 SPSs and 255 PPSs in one [verified: kCMFormatDescriptionError_InvalidParameter
   for 32 SPSs or 256 PPSs], so the stream decodes to nothing, and its end comes. */
static void parameterSetsOfEveryIdAreKeptOrRefused(NSString *assets) {
    const char *test = "parameter sets of every ID are kept or refused";
    const struct stream plain = load(assets, @"baseline");
    NSData *sps = first_nal(plain, 7), *pps = first_nal(plain, 8);
    NSMutableData *sets = [NSMutableData data];
    static const uint8_t start[4] = {0, 0, 0, 1};
    for (uint32_t id = 1; id < 64; id++) {
        NSData *one = with_id(sps, 4, id);
        if (one == nil) break;
        [sets appendBytes:start length:4], [sets appendData:one];
    }
    for (uint32_t id = 1; id < 300; id++) {
        NSData *one = with_id(pps, 1, id);
        if (one == nil) break;
        [sets appendBytes:start length:4], [sets appendData:one];
    }
    if (sets.length == 0) {
        fail(test, @"no SPS or PPS of ID 0 in the stream");
        return;
    }
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    const int queued = table->queue(table->context, handle, sets.bytes, sets.length, 0, 0);
    if (queued != 0) fail(test, [NSString stringWithFormat:@"the sets' unit: queue %d", queued]);
    BOOL ended = NO;
    NSArray<NSString *> *lines = decode_units(handle, plain, 0, plain.units.count, YES, &ended);
    if (!ended || lines.count != 0) fail(test, [NSString stringWithFormat:@"%@, the end %d", lines, ended]);
    table->destroy(table->context, handle);
}

/* Main's stream with Baseline's SPS as ID 1 after every access unit, which no PPS names: the order of presentation is
   still Main's (its SPS's reorder depth of 9), not that of the SPS read last (Baseline's 0). */
static void theReorderDepthIsTheActiveSpss(NSString *assets, NSString *log) {
    const char *test = "the reorder depth is the active SPS's";
    NSData *baseline_sps = with_id(first_nal(load(assets, @"baseline"), 7), 4, 1);
    if (baseline_sps == nil) {
        fail(test, @"no SPS of ID 0 in Baseline's stream");
        return;
    }
    const struct stream main_stream = with_units(load(assets, @"main"), baseline_sps, YES);
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    BOOL ended = NO;
    NSArray<NSString *> *lines = decode_units(handle, main_stream, 0, main_stream.units.count, YES, &ended);
    if (!ended) fail(test, @"no end of the stream");
    compare(test, lines, expected_frames(log, @"main"));
    table->destroy(table->context, handle);
}

/* Main's first 15 units, then Baseline's stream in the same decoder, whose SPS of ID 0 has other bytes: a new session,
   before which every frame of the old one comes, in its order, and then Baseline's from its time 0 on. */
static void aNewSessionHandsOutTheOldOnesFramesFirst(NSString *assets, NSString *log) {
    const char *test = "a new session hands out the old one's frames first";
    const struct stream main_stream = load(assets, @"main"), baseline = load(assets, @"baseline");
    NSMutableSet<NSNumber *> *times = [NSMutableSet set];
    for (NSUInteger index = 0; index < 15; index++) [times addObject:main_stream.units[index][2]];
    NSMutableArray<NSString *> *want = [NSMutableArray array];
    for (NSString *line in expected_frames(log, @"main")) {
        if ([times containsObject:@([line componentsSeparatedByString:@" "][1].longLongValue)]) [want addObject:line];
    }
    [want addObjectsFromArray:expected_frames(log, @"baseline")];
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    // Main's units queued at once, nothing taken, so that VideoToolbox still has some when the session changes: they
    // come at Main's depth, not Baseline's.
    const uint8_t *bytes = main_stream.bytes.bytes;
    for (NSUInteger index = 0; index < 15; index++) {
        NSArray<NSNumber *> *unit = main_stream.units[index];
        const int queued = table->queue(table->context, handle, bytes + unit[0].longLongValue,
                                        (size_t)unit[1].longLongValue, unit[2].longLongValue, 0);
        if (queued != 0) fail(test, [NSString stringWithFormat:@"Main's unit %lu: queue %d", (unsigned long)index,
                                                               queued]);
    }
    BOOL ended = NO;
    NSArray<NSString *> *lines = decode_units(handle, baseline, 0, baseline.units.count, YES, &ended);
    if (!ended) fail(test, @"no end of the stream");
    if (want.count != 15 + expected_frames(log, @"baseline").count) fail(test, @"not 15 of Main's frames in the log");
    compare(test, lines, want);
    table->destroy(table->context, handle);
}

#ifdef MINATO_VIDEO_DECODER_TESTING
/* What the decode functions of the tests below answer, and how often they were called. */
static atomic_int decodes;

/* Four units whose call back never comes, then a session iOS ended: twice that, and the decoder's queue is not full of
   units it lost. */
static OSStatus lose_then_end(VTDecompressionSessionRef session, CMSampleBufferRef sample, VTDecodeFrameFlags flags,
                              void *frame_context, VTDecodeInfoFlags *info) {
    (void)session, (void)sample, (void)flags, (void)frame_context, (void)info;
    return atomic_fetch_add(&decodes, 1) % 5 == 4 ? kVTInvalidSessionErr : noErr;
}

static OSStatus run_out_of_memory(VTDecompressionSessionRef session, CMSampleBufferRef sample,
                                  VTDecodeFrameFlags flags, void *frame_context, VTDecodeInfoFlags *info) {
    (void)session, (void)sample, (void)flags, (void)frame_context, (void)info;
    @throw [NSException exceptionWithName:atomic_fetch_add(&decodes, 1) == 0 ? NSMallocException
                                                                               : NSGenericException
                                   reason:@"graphics.video_decoder's"
                                 userInfo:nil];
}

/* VideoToolbox holding every unit, as its temporal processing may for a deep reorder. */
static OSStatus hold_every_unit(VTDecompressionSessionRef session, CMSampleBufferRef sample,
                                VTDecodeFrameFlags flags, void *frame_context, VTDecodeInfoFlags *info) {
    (void)session, (void)sample, (void)flags, (void)frame_context, (void)info;
    atomic_fetch_add(&decodes, 1);
    return noErr;
}

/* Main's stream (a reorder depth of 9) while VideoToolbox holds its units: as many as eight and the depth are taken,
   and the end of the stream after them, which makes VideoToolbox hand them out. */
static void unitsVideoToolboxHoldsAndTheEndAreTaken(NSString *assets, NSString *log) {
    const char *test = "units VideoToolbox holds and the end are taken";
    const struct stream stream = load(assets, @"main");
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    atomic_store(&decodes, 0);
    minato_video_decoder_set_decode_for_testing(hold_every_unit);
    const uint8_t *bytes = stream.bytes.bytes;
    for (NSUInteger index = 0; index < 17; index++) {
        NSArray<NSNumber *> *unit = stream.units[index];
        const int queued = table->queue(table->context, handle, bytes + unit[0].longLongValue,
                                        (size_t)unit[1].longLongValue, unit[2].longLongValue, 0);
        if (queued != 0) fail(test, [NSString stringWithFormat:@"unit %lu: queue %d", (unsigned long)index, queued]);
    }
    const int ended_queue = table->queue(table->context, handle, NULL, 0, 0, APKRT_VIDEO_DECODER_END_OF_STREAM);
    minato_video_decoder_set_decode_for_testing(NULL);
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    if (ended_queue != 0 || !drain(handle, lines) || atomic_load(&decodes) != 17) {
        fail(test, [NSString stringWithFormat:@"the end: queue %d, %d decodes", ended_queue, atomic_load(&decodes)]);
    }
    if (table->flush(table->context, handle) != 0) fail(test, @"flush refused");
    BOOL ended = NO;
    NSArray<NSString *> *decoded = decode_units(handle, stream, 0, stream.units.count, YES, &ended);
    if (!ended) fail(test, @"no end of the stream");
    compare(test, decoded, expected_frames(log, @"main"));
    table->destroy(table->context, handle);
}

static void lostUnitsAndAnEndedSessionLeaveTheDecoderWorking(NSString *assets, NSString *log) {
    const char *test = "lost units and an ended session leave the decoder working";
    const struct stream stream = load(assets, @"baseline");
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    atomic_store(&decodes, 0);
    minato_video_decoder_set_decode_for_testing(lose_then_end);
    BOOL ended = NO;
    NSArray<NSString *> *lost = decode_units(handle, stream, 0, 10, NO, &ended);
    minato_video_decoder_set_decode_for_testing(NULL);
    if (atomic_load(&decodes) != 10 || lost.count != 0) {
        fail(test, [NSString stringWithFormat:@"%d decodes, %@", atomic_load(&decodes), lost]);
    }
    NSArray<NSString *> *lines = decode_units(handle, stream, 15, stream.units.count, YES, &ended);
    if (!ended) fail(test, @"no end of the stream");
    compare(test, lines, expected_frames(log, @"baseline flushed after"));
    table->destroy(table->context, handle);
}

static void anExceptionInTheHostAnswersAnErrno(NSString *assets, NSString *log) {
    const char *test = "an exception in the host answers an errno";
    const struct stream stream = load(assets, @"baseline");
    const uint64_t handle = make(APKRT_VIDEO_DECODER_AVC);
    atomic_store(&decodes, 0);
    minato_video_decoder_set_decode_for_testing(run_out_of_memory);
    const uint8_t *bytes = stream.bytes.bytes;
    NSArray<NSNumber *> *unit = stream.units[0];
    // Eight of them, as many units as VideoToolbox may have: none of them stays in flight.
    for (int round = 0; round < 8; round++) {
        const int queued = table->queue(table->context, handle, bytes + unit[0].longLongValue,
                                        (size_t)unit[1].longLongValue, 0, 0);
        if (queued != (round == 0 ? -linux_enomem : -linux_eio)) {
            fail(test, [NSString stringWithFormat:@"round %d: queue %d", round, queued]);
        }
    }
    minato_video_decoder_set_decode_for_testing(NULL);
    BOOL ended = NO;
    NSArray<NSString *> *lines = decode_units(handle, stream, 0, stream.units.count, YES, &ended);
    if (!ended) fail(test, @"no end of the stream");
    compare(test, lines, expected_frames(log, @"baseline"));
    table->destroy(table->context, handle);
}

/* An 'RGBA' IOSurface of `width` x `height` with rows of `row` bytes. */
static IOSurfaceRef rgba_surface(size_t width, size_t height, size_t row) {
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth: @(width),
        (id)kIOSurfaceHeight: @(height),
        (id)kIOSurfaceBytesPerElement: @4,
        (id)kIOSurfaceBytesPerRow: @(row),
        (id)kIOSurfacePixelFormat: @((uint32_t)'RGBA'),
    });
}

/* ADR-0060 e2c2 hardening: the Metal pass refuses, with -EIO, surfaces Metal would end the process at (a side past
   16384, rows not of the device's linear texture alignment), and draws into one it can wrap. */
static void theMetalPassRefusesSurfacesMetalCannotWrap(void) {
    const char *test = "the Metal pass refuses surfaces Metal cannot wrap";
    CVPixelBufferRef image = NULL;
    NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    if (CVPixelBufferCreate(NULL, 64, 48, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                            (__bridge CFDictionaryRef)attributes, &image) != kCVReturnSuccess) {
        fail(test, @"no image");
        return;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    const size_t alignment = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA8Unorm];
    const size_t aligned = (64 * 4 + alignment - 1) / alignment * alignment;
    IOSurfaceRef surfaces[] = {rgba_surface(64, 48, aligned),
                               rgba_surface(16385, 1, (16385 * 4 + alignment - 1) / alignment * alignment),
                               rgba_surface(1, 16385, alignment > 4 ? alignment : 4),
                               alignment > 4 ? rgba_surface(64, 48, aligned + 4) : NULL};
    const char *const names[] = {"an aligned surface", "16385 x 1", "1 x 16385", "rows past the alignment"};
    const int wanted[] = {0, -linux_eio, -linux_eio, -linux_eio};
    for (int index = 0; index < 4; index++) {
        if (surfaces[index] == NULL) {
            if (index < 3) fail(test, [NSString stringWithFormat:@"%s: no surface", names[index]]);
            continue;
        }
        const int answer = minato_video_decoder_convert_for_testing(image, surfaces[index]);
        if (answer != wanted[index]) fail(test, [NSString stringWithFormat:@"%s: %d", names[index], answer]);
        CFRelease(surfaces[index]);
    }
    CVPixelBufferRelease(image);
}

#endif  // MINATO_VIDEO_DECODER_TESTING

/* A hardware buffer of graphics/host's own of the format (AHardwareBuffer's), as the guest's queue makes one, as a
   render's target: 0 with it, its handle and IOSurface. */
static int target_buffer(uint32_t format, uint32_t width, uint32_t height, struct apkrt_video_target *target,
                         IOSurfaceRef *surface) {
    const struct apkrt_hardware_buffers *buffers = minato_graphics_hardware_buffers();
    *target = (struct apkrt_video_target){.buffers = buffers->context,
                                          .description = {width, height, 1, format, 0x333}};  // GPU, CPU often
    if (buffers->allocate(buffers->context, &target->description, &target->handle, &target->stride) != 0) return -1;
    *surface = (IOSurfaceRef)minato_graphics_hardware_buffer_surface(target->handle);
    return *surface != NULL ? 0 : -1;
}

/* The mean absolute difference of an RGBA buffer of 320 x 180 from the YUV 420 one's samples converted by BT.601 or
   BT.709 in video range (kr, kb). */
static double rgba_difference(IOSurfaceRef rgba, IOSurfaceRef yuv, double kr, double kb) {
    IOSurfaceLock(rgba, kIOSurfaceLockReadOnly, NULL);
    IOSurfaceLock(yuv, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *pixels = IOSurfaceGetBaseAddress(rgba), *y = IOSurfaceGetBaseAddressOfPlane(yuv, 0);
    const uint8_t *chroma = IOSurfaceGetBaseAddressOfPlane(yuv, 1);
    const size_t row = IOSurfaceGetBytesPerRow(rgba), y_row = IOSurfaceGetBytesPerRowOfPlane(yuv, 0);
    const size_t c_row = IOSurfaceGetBytesPerRowOfPlane(yuv, 1);
    const double kg = 1 - kr - kb;
    double sum = 0;
    for (size_t line = 0; line < 180; line++) {
        for (size_t column = 0; column < 320; column++) {
            const double luma = (y[line * y_row + column] - 16) * 255.0 / 219;
            const double cb = (chroma[(line / 2) * c_row + (column / 2) * 2] - 128) * 255.0 / 224;
            const double cr = (chroma[(line / 2) * c_row + (column / 2) * 2 + 1] - 128) * 255.0 / 224;
            const double r = luma + 2 * (1 - kr) * cr, b = luma + 2 * (1 - kb) * cb, g = (luma - kr * r - kb * b) / kg;
            const double want[3] = {r, g, b};
            for (int c = 0; c < 3; c++) {
                const double clamped = want[c] < 0 ? 0 : want[c] > 255 ? 255 : want[c];
                sum += fabs(clamped - pixels[line * row + column * 4 + (size_t)c]);
            }
        }
    }
    IOSurfaceUnlock(yuv, kIOSurfaceLockReadOnly, NULL);
    IOSurfaceUnlock(rgba, kIOSurfaceLockReadOnly, NULL);
    return sum / (320.0 * 180 * 3);
}

/* ADR-0060 e2c2: each of Baseline's frames into a YUV buffer and, from a second decoder, into an RGBA one (a
   SurfaceTexture's slot): the RGBA one is the YUV one's samples through BT.601 in video range (the stream has no
   matrix of its own and 180 rows) within a mean of 3, clearly closer than BT.709's, alpha 255. */
static void rgbaFramesAreBt601VideoOfTheYuvOnes(struct stream stream, const struct apkrt_video_target *yuv_target,
                                                IOSurfaceRef yuv, const struct apkrt_video_target *rgba_target,
                                                IOSurfaceRef rgba) {
    const char *test = "RGBA frames are BT.601 video of the YUV ones";
    const uint64_t to_yuv = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
    const uint64_t to_rgba = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
    const uint8_t *bytes = stream.bytes.bytes;
    int compared = 0;
    for (NSUInteger index = 0; index < stream.units.count; index++) {
        NSArray<NSNumber *> *unit = stream.units[index];
        struct apkrt_video_frame frames[2];
        BOOL rendered = YES;
        for (int which = 0; which < 2; which++) {
            const uint64_t handle = which == 0 ? to_yuv : to_rgba;
            table->queue(table->context, handle, bytes + unit[0].longLongValue, (size_t)unit[1].longLongValue,
                         unit[2].longLongValue, 0);
            for (int tries = 0; table->peek(table->context, handle, &frames[which]) != 0 && tries < 2000; tries++) {
                usleep(1000);
            }
            table->pop(table->context, handle);
            const int answer = table->render(table->context, handle, frames[which].token,
                                             which == 0 ? yuv_target : rgba_target);
            if (answer != 0) {
                fail(test, [NSString stringWithFormat:@"unit %lu: render %d into %s", (unsigned long)index, answer,
                                                      which == 0 ? "YUV" : "RGBA"]);
                rendered = NO;
            }
        }
        if (!rendered || frames[0].time_us != frames[1].time_us) continue;
        const double bt601 = rgba_difference(rgba, yuv, 0.299, 0.114);
        const double bt709 = rgba_difference(rgba, yuv, 0.2126, 0.0722);
        IOSurfaceLock(rgba, kIOSurfaceLockReadOnly, NULL);
        const uint8_t alpha = ((const uint8_t *)IOSurfaceGetBaseAddress(rgba))[3];
        IOSurfaceUnlock(rgba, kIOSurfaceLockReadOnly, NULL);
        if (bt601 > 3 || bt709 < bt601 + 2 || alpha != 255) {
            fail(test, [NSString stringWithFormat:@"pts %lld: BT.601 %.2f, BT.709 %.2f, alpha %u",
                                                  (long long)frames[0].time_us, bt601, bt709, alpha]);
        }
        compared++;
    }
    if (compared != (int)stream.units.count) fail(test, [NSString stringWithFormat:@"%d frames compared", compared]);
    table->destroy(table->context, to_yuv);
    table->destroy(table->context, to_rgba);
}

/* A YUV 420 buffer's line as frame_line has a frame's: its CRC32s inside 320 x 180 at `time_us`. */
static NSString *surface_line(IOSurfaceRef surface, int64_t time_us) {
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *y = IOSurfaceGetBaseAddressOfPlane(surface, 0), *chroma = IOSurfaceGetBaseAddressOfPlane(surface, 1);
    const size_t y_row = IOSurfaceGetBytesPerRowOfPlane(surface, 0), c_row = IOSurfaceGetBytesPerRowOfPlane(surface, 1);
    uLong crcs[3] = {crc32(0L, Z_NULL, 0), crc32(0L, Z_NULL, 0), crc32(0L, Z_NULL, 0)};
    for (size_t row = 0; row < 180; row++) {
        for (size_t column = 0; column < 320; column++) crcs[0] = crc32(crcs[0], y + row * y_row + column, 1);
    }
    for (size_t row = 0; row < 90; row++) {
        for (size_t column = 0; column < 160; column++) {
            crcs[1] = crc32(crcs[1], chroma + row * c_row + column * 2, 1);
            crcs[2] = crc32(crcs[2], chroma + row * c_row + column * 2 + 1, 1);
        }
    }
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    return [NSString stringWithFormat:@"pts %lld Y %lx U %lx V %lx", (long long)time_us, crcs[0], crcs[1], crcs[2]];
}

/* ADR-0060 e2c: Baseline's stream through a surface decoder, each frame rendered into a YUV 420 buffer of the
   guest's kind (an ImageReader's slot): the buffer's samples are Android's frame's; tokens it does not have, a
   target of another table and one whose IOSurface is not the '420v' its description says are refused, a discarded
   token ends, at most eight frames wait, a flush ends a waiting one, and a render in the background draws nothing. */
static void aSurfaceDecoderDrawsIntoAYuvBufferAsAndroidDecodes(NSString *assets, NSString *log) {
    const char *test = "a surface decoder draws into a YUV buffer as Android decodes";
    const struct stream stream = load(assets, @"baseline");
    const uint64_t handle = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
    struct apkrt_video_target target, rgba;
    IOSurfaceRef surface = NULL, rgba_surface = NULL;
    if (handle == 0 || target_buffer(0x23, 320, 180, &target, &surface) != 0 ||
        target_buffer(1, 320, 180, &rgba, &rgba_surface) != 0) {
        fail(test, @"no decoder or buffers");
        return;
    }
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    const uint8_t *bytes = stream.bytes.bytes;
    BOOL ended = NO;
    BOOL (^take)(void) = ^BOOL {
        struct apkrt_video_frame frame;
        while (table->peek(table->context, handle, &frame) == 0) {
            if ((frame.flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0) {
                table->pop(table->context, handle);
                return YES;
            }
            if (frame.token == 0 || frame.data != NULL || frame.size != 0) {
                fail(test, @"a frame with bytes or without a token");
                table->pop(table->context, handle);
                continue;
            }
            const uint64_t token = frame.token;
            table->pop(table->context, handle);
            const int rendered = table->render(table->context, handle, token, &target);
            if (rendered != 0) fail(test, [NSString stringWithFormat:@"render %d", rendered]);
            [lines addObject:surface_line(surface, frame.time_us)];
        }
        return NO;
    };
    for (NSUInteger index = 0; index < stream.units.count; index++) {
        NSArray<NSNumber *> *unit = stream.units[index];
        int queued;
        for (int tries = 0; (queued = table->queue(table->context, handle, bytes + unit[0].longLongValue,
                                                   (size_t)unit[1].longLongValue, unit[2].longLongValue, 0)) ==
                                -linux_eagain && tries < 1000;
             tries++) {
            take();
            usleep(1000);
        }
        if (queued != 0) fail(test, [NSString stringWithFormat:@"queue %d", queued]);
        take();
    }
    table->queue(table->context, handle, NULL, 0, 0, APKRT_VIDEO_DECODER_END_OF_STREAM);
    for (int tries = 0; !ended && tries < 5000; tries++) {
        ended = take();
        if (!ended) usleep(1000);
    }
    if (!ended) fail(test, @"no end of the stream");
    compare(test, lines, expected_frames(log, @"baseline reader"));
    // The refusals, on a decoder with one frame waiting.
    const uint64_t other = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
    NSArray<NSNumber *> *first = stream.units[0];
    table->queue(table->context, other, bytes + first[0].longLongValue, (size_t)first[1].longLongValue, 0, 0);
    struct apkrt_video_frame frame;
    for (int tries = 0; table->peek(table->context, other, &frame) != 0 && tries < 2000; tries++) usleep(1000);
    const uint64_t token = frame.token;
    table->pop(table->context, other);
    struct apkrt_video_target foreign = target;
    foreign.buffers = (void *)&foreign;
    if (table->render(table->context, other, token + 1000, &target) != -linux_einval) fail(test, @"a token not there");
    if (table->render(table->context, other, token, &foreign) != -linux_einval) fail(test, @"another table's buffer");
    table->pop(table->context, other);
    // A buffer whose description says YUV 420 but whose IOSurface is an RGBA one.
    const uint64_t lying_handle = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
    table->queue(table->context, lying_handle, bytes + first[0].longLongValue, (size_t)first[1].longLongValue, 0, 0);
    for (int tries = 0; table->peek(table->context, lying_handle, &frame) != 0 && tries < 2000; tries++) usleep(1000);
    table->pop(table->context, lying_handle);
    struct apkrt_video_target lying = rgba;
    lying.description.format = 0x23;
    if (table->render(table->context, lying_handle, frame.token, &lying) != -linux_einval) {
        fail(test, @"an RGBA surface described as YUV 420");
    }
    table->destroy(table->context, lying_handle);
    const uint64_t rgba_handle = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
    table->queue(table->context, rgba_handle, bytes + first[0].longLongValue, (size_t)first[1].longLongValue, 0, 0);
    for (int tries = 0; table->peek(table->context, rgba_handle, &frame) != 0 && tries < 2000; tries++) usleep(1000);
    table->pop(table->context, rgba_handle);
    table->discard(table->context, rgba_handle, frame.token);
    if (table->render(table->context, rgba_handle, frame.token, &target) != -linux_einval) {
        fail(test, @"a token rendered after its end");
    }
    // At most eight frames wait: a ninth decoded one stays unpeeked until one of them ends.
    const uint64_t many = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
    uint64_t tokens[APKRT_VIDEO_DECODER_MAX_WAITING] = {0};
    uint32_t popped = 0;
    for (NSUInteger index = 0; index < 15 && popped < APKRT_VIDEO_DECODER_MAX_WAITING; index++) {
        NSArray<NSNumber *> *unit = stream.units[index];
        table->queue(table->context, many, bytes + unit[0].longLongValue, (size_t)unit[1].longLongValue,
                     unit[2].longLongValue, 0);
        for (int tries = 0; tries < 2000 && table->peek(table->context, many, &frame) != 0; tries++) usleep(1000);
        tokens[popped++] = frame.token;
        table->pop(table->context, many);
    }
    NSArray<NSNumber *> *ninth = stream.units[popped];
    table->queue(table->context, many, bytes + ninth[0].longLongValue, (size_t)ninth[1].longLongValue,
                 ninth[2].longLongValue, 0);
    usleep(200000);
    if (popped != APKRT_VIDEO_DECODER_MAX_WAITING || table->peek(table->context, many, &frame) != -linux_eagain) {
        fail(test, @"a ninth frame peeked while eight wait");
    }
    table->discard(table->context, many, tokens[0]);
    if (table->peek(table->context, many, &frame) != 0) fail(test, @"no frame after a discard");
    table->destroy(table->context, many);
    // A flush drops the waiting frames: their tokens draw nothing after it.
    table->queue(table->context, other, bytes + first[0].longLongValue, (size_t)first[1].longLongValue, 0, 0);
    for (int tries = 0; table->peek(table->context, other, &frame) != 0 && tries < 2000; tries++) usleep(1000);
    table->pop(table->context, other);
    table->flush(table->context, other);
    if (table->render(table->context, other, frame.token, &target) != -linux_einval) {
        fail(test, @"a token after a flush");
    }
    table->destroy(table->context, other);
    table->destroy(table->context, rgba_handle);
    // In the background (ADR-0032, decision 6) a render draws nothing, neither by the Metal pass nor by
    // VTPixelTransferSession, and its token ends; back in the foreground the next one draws.
    const struct apkrt_video_target *const targets[] = {&rgba, &target, &rgba};
    for (int index = 0; index < 3; index++) {
        const uint64_t one = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
        table->queue(table->context, one, bytes + first[0].longLongValue, (size_t)first[1].longLongValue, 0, 0);
        int peeked = -1;
        for (int tries = 0; (peeked = table->peek(table->context, one, &frame)) != 0 && tries < 2000; tries++) {
            usleep(1000);
        }
        if (peeked != 0) {
            fail(test, @"no frame for a render in the background");
            table->destroy(table->context, one);
            continue;
        }
        table->pop(table->context, one);
        apkrt_set_background(index < 2);
        const int rendered = table->render(table->context, one, frame.token, targets[index]);
        apkrt_set_background(0);
        if (rendered != (index < 2 ? -linux_eio : 0)) {
            fail(test, [NSString stringWithFormat:@"a render %s the background: %d", index < 2 ? "in" : "after",
                                                  rendered]);
        }
        if (index < 2 && table->render(table->context, one, frame.token, targets[index]) != -linux_einval) {
            fail(test, @"a token left after a render in the background");
        }
        table->destroy(table->context, one);
    }
#ifdef MINATO_VIDEO_DECODER_TESTING
    // No render left counted in flight, refused ones included: the scheduled provider answers yes at once.
    if (minato_video_decoder_scheduled_for_testing(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 50000000) != 1) {
        fail(test, @"renders still in flight for the scheduled provider");
    }
#endif
    rgbaFramesAreBt601VideoOfTheYuvOnes(stream, &target, surface, &rgba, rgba_surface);
    table->destroy(table->context, handle);
    CFRelease(surface);
    CFRelease(rgba_surface);
    const struct apkrt_hardware_buffers *buffers = minato_graphics_hardware_buffers();
    buffers->free(buffers->context, target.handle);
    buffers->free(buffers->context, rgba.handle);
}

/* The first frame of Baseline's stream from a new surface decoder: its handle, and the frame's token in `token`;
   0 without one. */
static uint64_t first_frame(struct stream stream, uint64_t *token, int64_t *time_us) {
    const uint64_t handle = make_with(APKRT_VIDEO_DECODER_AVC, APKRT_VIDEO_DECODER_SURFACE);
    NSArray<NSNumber *> *first = stream.units[0];
    const uint8_t *bytes = stream.bytes.bytes;
    table->queue(table->context, handle, bytes + first[0].longLongValue, (size_t)first[1].longLongValue, 0, 0);
    struct apkrt_video_frame frame;
    int peeked = -1;
    for (int tries = 0; (peeked = table->peek(table->context, handle, &frame)) != 0 && tries < 2000; tries++) {
        usleep(1000);
    }
    if (peeked != 0) {
        table->destroy(table->context, handle);
        return 0;
    }
    table->pop(table->context, handle);
    *token = frame.token;
    if (time_us != NULL) *time_us = frame.time_us;
    return handle;
}

/* The token of the stream's second frame on a decoder of first_frame's, or 0. */
static uint64_t next_token(struct stream stream, uint64_t handle) {
    NSArray<NSNumber *> *second = stream.units[1];
    const uint8_t *bytes = stream.bytes.bytes;
    table->queue(table->context, handle, bytes + second[0].longLongValue, (size_t)second[1].longLongValue,
                 second[2].longLongValue, 0);
    struct apkrt_video_frame frame;
    int peeked = -1;
    for (int tries = 0; (peeked = table->peek(table->context, handle, &frame)) != 0 && tries < 2000; tries++) {
        usleep(1000);
    }
    if (peeked != 0) return 0;
    table->pop(table->context, handle);
    return frame.token;
}

/* ADR-0060 e2c3: a surface decoder's frame into a host window (a SurfaceView's): drawn into the next drawable of the
   window's layer, of the frame's size, and presented, at once or at the time asked, the layer's gravity by the
   scaling mode (SCALE_TO_FIT stretches, WITH_CROPPING fills), the present counted for the window. The drawable's
   pixels, which only the testing build hands back, are the YUV render's samples through BT.601 in video range, as
   the RGBA render's are, in BGRA. Refused: a token that does not wait or has ended, a decoder for buffers, unknown
   flags (-EINVAL), a window that is not there or was removed (-ENODEV); in the background nothing is drawn (-EIO)
   and the token ends. */
static void aSurfaceDecoderDrawsIntoAWindow(NSString *assets) {
    const char *test = "a surface decoder draws into a window";
    const struct stream stream = load(assets, @"baseline");
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.framebufferOnly = NO;  // so that the testing build can read the drawable back
    const uint64_t window = apkrt_window_add((__bridge void *)layer, 1290, 2796);
    struct apkrt_video_target yuv;
    IOSurfaceRef yuv_surface = NULL;
    if (window == 0 || target_buffer(0x23, 320, 180, &yuv, &yuv_surface) != 0) {
        fail(test, @"no window or buffer");
        return;
    }
    uint64_t token = 0;
    int64_t time_us = 0;
    // The reference: the same first frame through a YUV render.
    uint64_t handle = first_frame(stream, &token, &time_us);
    if (handle == 0 || table->render(table->context, handle, token, &yuv) != 0) fail(test, @"no YUV render");
    table->destroy(table->context, handle);
    // At once, scaled to fit.
    handle = first_frame(stream, &token, NULL);
    const uint64_t presents = apkrt_window_presents(window);
    int answer = table->render_window(table->context, handle, token, window, 0, 0);
    if (answer != 0) fail(test, [NSString stringWithFormat:@"render at once %d", answer]);
    if (!CGSizeEqualToSize(layer.drawableSize, CGSizeMake(320, 180))) fail(test, @"the drawable is not the frame's");
    if (![layer.contentsGravity isEqualToString:kCAGravityResize]) fail(test, @"SCALE_TO_FIT does not stretch");
    if (apkrt_window_presents(window) != presents + 1) fail(test, @"the present was not counted");
#ifdef MINATO_VIDEO_DECODER_TESTING
    id<MTLTexture> drawn = minato_video_decoder_window_texture_for_testing();
    if (drawn.pixelFormat != MTLPixelFormatBGRA8Unorm || drawn.width != 320 || drawn.height != 180) {
        fail(test, @"the drawable's texture");
    } else {
        IOSurfaceRef rgba = rgba_surface(320, 180, 320 * 4);
        IOSurfaceLock(rgba, 0, NULL);
        uint8_t *pixels = IOSurfaceGetBaseAddress(rgba);
        [drawn getBytes:pixels bytesPerRow:320 * 4 fromRegion:MTLRegionMake2D(0, 0, 320, 180) mipmapLevel:0];
        for (size_t at = 0; at < 320 * 180 * 4; at += 4) {  // BGRA to RGBA
            const uint8_t blue = pixels[at];
            pixels[at] = pixels[at + 2];
            pixels[at + 2] = blue;
        }
        IOSurfaceUnlock(rgba, 0, NULL);
        const double bt601 = rgba_difference(rgba, yuv_surface, 0.299, 0.114);
        const double bt709 = rgba_difference(rgba, yuv_surface, 0.2126, 0.0722);
        if (bt601 > 3 || bt709 < bt601 + 2) {
            fail(test, [NSString stringWithFormat:@"the window's pixels: BT.601 %.2f, BT.709 %.2f", bt601, bt709]);
        }
        CFRelease(rgba);
    }
#endif
    if (table->render_window(table->context, handle, token, window, 0, 0) != -linux_einval) {
        fail(test, @"a token rendered twice");
    }
    table->destroy(table->context, handle);
    // At a time, cropped to fill.
    handle = first_frame(stream, &token, NULL);
    const int64_t later = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 50000000;
    answer = table->render_window(table->context, handle, token, window, later, APKRT_VIDEO_DECODER_CROP);
    if (answer != 0) fail(test, [NSString stringWithFormat:@"render at a time %d", answer]);
    if (![layer.contentsGravity isEqualToString:kCAGravityResizeAspectFill]) fail(test, @"WITH_CROPPING does not fill");
#ifdef MINATO_VIDEO_DECODER_TESTING
    if (minato_video_decoder_window_time_for_testing() != later) fail(test, @"not presented at the time asked");
#endif
    table->destroy(table->context, handle);
    // The decoder's end gives the window back stretched, Core Animation's default, which the other producers take:
    // their buffers bring no scaling mode of their own.
    if (![layer.contentsGravity isEqualToString:kCAGravityResize]) fail(test, @"a cropping decoder's gravity stayed");
    // So does its switch to another window.
    {
        CAMetalLayer *other = [CAMetalLayer layer];
        const uint64_t second = apkrt_window_add((__bridge void *)other, 1290, 2796);
        handle = first_frame(stream, &token, NULL);
        table->render_window(table->context, handle, token, window, 0, APKRT_VIDEO_DECODER_CROP);
        const uint64_t next = next_token(stream, handle);
        if (next == 0 || table->render_window(table->context, handle, next, second, 0, 0) != 0 ||
            ![layer.contentsGravity isEqualToString:kCAGravityResize]) {
            fail(test, @"a cropping decoder's gravity stayed after its switch");
        }
        table->destroy(table->context, handle);
        apkrt_window_remove(second);
    }
    // More than a second ahead, at once, as Android's consumer takes such a time for a wrong one (MAX_REASONABLE_NSEC).
    handle = first_frame(stream, &token, NULL);
    const int64_t far = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 10000000000;
    answer = table->render_window(table->context, handle, token, window, far, 0);
    if (answer != 0) fail(test, [NSString stringWithFormat:@"render far ahead %d", answer]);
#ifdef MINATO_VIDEO_DECODER_TESTING
    if (minato_video_decoder_window_time_for_testing() != 0) fail(test, @"a time far ahead was kept");
#endif
    table->destroy(table->context, handle);
    // A layer window.m's hide rule hid is shown again with the frame.
    handle = first_frame(stream, &token, NULL);
    layer.hidden = YES;
    const uint64_t shown = apkrt_window_presents(window);
    answer = table->render_window(table->context, handle, token, window, 0, 0);
    if (answer != 0 || layer.hidden || apkrt_window_presents(window) != shown + 1) {
        fail(test, [NSString stringWithFormat:@"a render into a hidden layer: %d, hidden %d", answer, layer.hidden]);
    }
    table->destroy(table->context, handle);
    // The refusals.
    handle = first_frame(stream, &token, NULL);
    if (table->render_window(table->context, handle, token + 1000, window, 0, 0) != -linux_einval) {
        fail(test, @"a token not there");
    }
    if (table->render_window(table->context, handle, token, window, 0, 0x80) != -linux_einval) {
        fail(test, @"unknown flags");
    }
    table->destroy(table->context, handle);
    const uint64_t buffers = make(APKRT_VIDEO_DECODER_AVC);
    if (table->render_window(table->context, buffers, 1, window, 0, 0) != -linux_einval) {
        fail(test, @"a decoder for buffers");
    }
    table->destroy(table->context, buffers);
    handle = first_frame(stream, &token, NULL);
    if (table->render_window(table->context, handle, token, window + 1000, 0, 0) != -linux_enodev) {
        fail(test, @"a window that is not there");
    }
    if (table->render_window(table->context, handle, token, window, 0, 0) != -linux_einval) {
        fail(test, @"a token left after a refused window");
    }
    table->destroy(table->context, handle);
    // A window there without a Metal layer (apk-run's have none) drops the frame, without -ENODEV's "gone".
    const uint64_t plain = apkrt_window_add((__bridge void *)[CALayer layer], 1290, 2796);
    const uint64_t layerless = apkrt_window_add(NULL, 1290, 2796);
    handle = first_frame(stream, &token, NULL);
    if (table->render_window(table->context, handle, token, plain, 0, 0) != -linux_einval) {
        fail(test, @"a window of a plain CALayer");
    }
    if (table->render_window(table->context, handle, token, plain, 0, 0) != -linux_einval) {
        fail(test, @"a token left after a plain CALayer");
    }
    table->destroy(table->context, handle);
    handle = first_frame(stream, &token, NULL);
    if (table->render_window(table->context, handle, token, layerless, 0, 0) != -linux_einval) {
        fail(test, @"a window without a layer");
    }
    table->destroy(table->context, handle);
    apkrt_window_remove(plain);
    apkrt_window_remove(layerless);
    // In the background nothing is drawn and the token ends.
    handle = first_frame(stream, &token, NULL);
    const uint64_t before = apkrt_window_presents(window);
    apkrt_set_background(1);
    answer = table->render_window(table->context, handle, token, window, 0, 0);
    apkrt_set_background(0);
    if (answer != -linux_eio || apkrt_window_presents(window) != before) {
        fail(test, [NSString stringWithFormat:@"a render in the background: %d", answer]);
    }
    if (table->render_window(table->context, handle, token, window, 0, 0) != -linux_einval) {
        fail(test, @"a token left after a render in the background");
    }
    table->destroy(table->context, handle);
#ifdef MINATO_VIDEO_DECODER_TESTING
    // A render that waits for a drawable (Core Animation's wait of up to a second) is not in flight yet, so that the
    // provider of "scheduled" answers meanwhile, as window.m's wait for a drawable is not counted either.
    {
        NSMutableArray<id<CAMetalDrawable>> *held = [NSMutableArray array];
        for (int taken = 0; taken < 3; taken++) {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (drawable == nil) break;
            [held addObject:drawable];
        }
        handle = first_frame(stream, &token, NULL);
        __block int waited = 0;
        const uint64_t waiting = handle, waiting_token = token;
        dispatch_group_t group = dispatch_group_create();
        dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            waited = table->render_window(table->context, waiting, waiting_token, window, 0, 0);
        });
        usleep(200000);
        if (minato_video_decoder_scheduled_for_testing(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 100000000) != 1) {
            fail(test, @"a render waiting for a drawable held the provider of \"scheduled\"");
        }
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
        (void)waited;
        [held removeAllObjects];
        table->destroy(table->context, handle);
    }
#endif
    // A removed window.
    handle = first_frame(stream, &token, NULL);
    apkrt_window_remove(window);
    if (table->render_window(table->context, handle, token, window, 0, 0) != -linux_enodev) {
        fail(test, @"a removed window");
    }
    table->destroy(table->context, handle);
#ifdef MINATO_VIDEO_DECODER_TESTING
    if (minato_video_decoder_scheduled_for_testing(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 50000000) != 1) {
        fail(test, @"window renders still in flight for the scheduled provider");
    }
#endif
    CFRelease(yuv_surface);
    const struct apkrt_hardware_buffers *hardware = minato_graphics_hardware_buffers();
    hardware->free(hardware->context, yuv.handle);
}

static void typesItDoesNotDecodeAreRefused(void) {
    const char *test = "types it does not decode are refused";
    const struct apkrt_video_decoder_description description = {99, 320, 180, NULL, 0, 0};
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
        parameterSetsOfEveryIdAreKeptOrRefused(assets);
        theReorderDepthIsTheActiveSpss(assets, log);
        aNewSessionHandsOutTheOldOnesFramesFirst(assets, log);
        aSurfaceDecoderDrawsIntoAYuvBufferAsAndroidDecodes(assets, log);
        aSurfaceDecoderDrawsIntoAWindow(assets);
        int cases = 12;
#ifdef MINATO_VIDEO_DECODER_TESTING
        lostUnitsAndAnEndedSessionLeaveTheDecoderWorking(assets, log);
        unitsVideoToolboxHoldsAndTheEndAreTaken(assets, log);
        anExceptionInTheHostAnswersAnErrno(assets, log);
        theMetalPassRefusesSurfacesMetalCannotWrap();
        cases += 4;
#endif
        printf("graphics.video_decoder: %d cases, %d failures\n", cases, failures);
        return failures == 0 ? 0 : 1;
    }
}
