/* graphics.camera_model (ADR-0073 decision 10): graphics/host's model cameras (camera_model.m) behind the functions
   minato_graphics_set_camera_model gives the runtime, without the runtime: the two cameras' facts, one open each, a
   repeating capture at its rate with the frame numbers, tokens and times it gives, a single JPEG capture before the
   repeating one's next frame whose picture decodes to the bars and the block, the AF trigger's FOCUSING frames, the
   waiting tokens' discard and flush, and the close.
   Usage: graphics-camera-model-test. Exits 1 after the cases when one failed. */
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "minato_graphics.h"

enum { linux_eagain = 11, linux_ebusy = 16, linux_einval = 22, linux_enospc = 28 };

static int failures;

#define EXPECT(condition)                                                                    \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            fprintf(stderr, "FAILED  %s:%d: %s\n", __func__, __LINE__, #condition);          \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static const struct apkrt_cameras *table;

/* The value of `key` in a facts list, its first I32, or -1. */
static int32_t fact_i32(const uint8_t *bytes, size_t size, uint32_t key) {
    for (size_t at = 0; at + 12 <= size;) {
        uint32_t header[3];
        memcpy(header, bytes + at, sizeof header);
        const size_t length = 12 + (size_t)header[2] * (header[1] == APKRT_CAMERA_TYPE_I64 ? 8 : 4);
        if (header[0] == key && header[1] == APKRT_CAMERA_TYPE_I32 && header[2] > 0) {
            int32_t value;
            memcpy(&value, bytes + at + 12, 4);
            return value;
        }
        at += (length + 7) & ~(size_t)7;
    }
    return -1;
}

static void facts_of_both(void) {
    EXPECT(table->count(table->context) == 2);
    uint8_t bytes[4096];
    size_t size = 0;
    EXPECT(table->facts(table->context, 0, bytes, 8, &size) == -linux_enospc && size > 8);
    EXPECT(table->facts(table->context, 0, bytes, sizeof bytes, &size) == 0);
    EXPECT(fact_i32(bytes, size, APKRT_CAMERA_KEY_FACING) == 1);
    EXPECT(fact_i32(bytes, size, APKRT_CAMERA_KEY_ORIENTATION) == 90);
    EXPECT(fact_i32(bytes, size, APKRT_CAMERA_KEY_FLASH) == 1);
    EXPECT(table->facts(table->context, 1, bytes, sizeof bytes, &size) == 0);
    EXPECT(fact_i32(bytes, size, APKRT_CAMERA_KEY_FACING) == 0);
    EXPECT(fact_i32(bytes, size, APKRT_CAMERA_KEY_ORIENTATION) == 270);
    EXPECT(fact_i32(bytes, size, APKRT_CAMERA_KEY_AF_MODES) == 0);  // fixed focus
    EXPECT(table->facts(table->context, 2, bytes, sizeof bytes, &size) == -linux_einval);
}

static struct apkrt_camera_controls controls(void) {
    struct apkrt_camera_controls c = {0};
    c.af_mode = 4, c.awb_mode = 1, c.ae_mode = 1, c.jpeg_quality = 90, c.zoom_ratio = 1, c.fps_min = 30;
    c.fps_max = 30;
    return c;
}

/* The next frame, waiting up to a second for it. */
static int next_frame(uint64_t handle, struct apkrt_camera_capture *capture) {
    for (int tries = 0; tries < 200; tries++) {
        const int answer = table->peek(table->context, handle, capture);
        if (answer != -linux_eagain) return answer;
        usleep(5000);
    }
    return -linux_eagain;
}

static void captures_in_order(void) {
    uint64_t handle = 0, other = 0;
    EXPECT(table->open(table->context, 0, &handle) == 0 && handle != 0);
    EXPECT(table->open(table->context, 0, &other) == -linux_ebusy);
    const struct apkrt_camera_stream streams[2] = {{APKRT_CAMERA_FORMAT_YUV_420_888, 640, 480, 0},
                                                   {APKRT_CAMERA_FORMAT_JPEG, 4032, 3024, 0}};
    EXPECT(table->configure(table->context, handle, streams, 2) == 0);
    struct apkrt_camera_controls repeating = controls();
    EXPECT(table->request(table->context, handle, &repeating, 1, 1, APKRT_CAMERA_REPEATING) == 0);
    struct apkrt_camera_capture capture;
    int64_t last_time = 0;
    uint64_t tokens[3] = {0};
    for (int n = 0; n < 3; n++) {
        EXPECT(next_frame(handle, &capture) == 0);
        EXPECT(capture.frame.number == 1 && capture.frame.frame == (uint64_t)n && capture.frame.streams == 1);
        EXPECT(capture.frame.tokens[0] != 0 && capture.jpeg_size == 0);
        EXPECT((capture.frame.states & APKRT_CAMERA_STATE_FOCUSED) != 0);
        if (n > 0) EXPECT(capture.frame.timestamp_ns - last_time >= 30000000);  // at 30 fps
        last_time = capture.frame.timestamp_ns;
        tokens[n] = capture.frame.tokens[0];
        table->pop(table->context, handle);
    }
    EXPECT(tokens[0] != tokens[1] && tokens[1] != tokens[2]);
    // A single JPEG capture comes before the repeating one's next frame, with an AF trigger's FOCUSING after it.
    struct apkrt_camera_controls still = controls();
    still.af_trigger = 1;
    EXPECT(table->request(table->context, handle, &still, 2, 2, 0) == 0);
    EXPECT(next_frame(handle, &capture) == 0);
    EXPECT(capture.frame.number == 2 && capture.frame.streams == 2 && capture.frame.tokens[1] == 0);
    EXPECT((capture.frame.states & APKRT_CAMERA_STATE_FOCUSING) != 0);
    EXPECT(capture.jpeg_size > 1000 && ((const uint8_t *)capture.jpeg)[0] == 0xff &&
           ((const uint8_t *)capture.jpeg)[1] == 0xd8);
    NSData *jpeg = [NSData dataWithBytes:capture.jpeg length:(NSUInteger)capture.jpeg_size];
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)jpeg, NULL);
    CGImageRef picture = source != NULL ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
    EXPECT(picture != NULL && CGImageGetWidth(picture) == 4032 && CGImageGetHeight(picture) == 3024);
    if (picture != NULL) {
        // The first bar is white, the second yellow (no blue), at mid height.
        uint8_t pixels[4 * 2] = {0};
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context =
            CGBitmapContextCreate(pixels, 2, 1, 8, 8, space, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
        CGContextDrawImage(context, CGRectMake(-252, -1512, 4032, 3024), picture);  // x 252 and 756 to 0 and 1
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        EXPECT(pixels[0] > 200 && pixels[1] > 200 && pixels[2] > 200);
        CGImageRelease(picture);
    }
    if (source != NULL) CFRelease(source);
    table->pop(table->context, handle);
    for (int n = 0; n < 2; n++) {
        EXPECT(next_frame(handle, &capture) == 0 && capture.frame.number == 1);
        EXPECT((capture.frame.states & APKRT_CAMERA_STATE_FOCUSING) != 0);  // the trigger's 3 frames
        table->pop(table->context, handle);
    }
    EXPECT(next_frame(handle, &capture) == 0 && (capture.frame.states & APKRT_CAMERA_STATE_FOCUSED) != 0);
    table->pop(table->context, handle);
    // The waiting tokens: one discarded, then a flush ends the rest, whose render then finds none.
    table->discard(table->context, handle, tokens[0]);
    EXPECT(table->render_window(table->context, handle, tokens[0], 1, 0, 0) == -linux_einval);
    EXPECT(table->flush(table->context, handle) == 0);
    EXPECT(table->render_window(table->context, handle, tokens[1], 1, 0, 0) == -linux_einval);
    table->stop_repeating(table->context, handle);
    table->close(table->context, handle);
    EXPECT(table->open(table->context, 0, &other) == 0);  // free again after the close
    table->close(table->context, other);
}

int main(void) {
    @autoreleasepool {
        table = minato_graphics_camera_model();
        facts_of_both();
        captures_in_order();
        EXPECT(minato_graphics_set_camera_model() == 0);
    }
    if (failures == 0) printf("graphics.camera_model: ok\n");
    return failures == 0 ? 0 : 1;
}
