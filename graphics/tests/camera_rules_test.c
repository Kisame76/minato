/* graphics.camera_rules (ADR-0073 decision 10): the iPhone cameras' rules (camera_rules.c) on device descriptions
   shaped like an iPhone's back triple camera and its front camera: the facts and their limits, the format a stream
   configuration runs on, the 3A state bits through triggers, and the controls' mapping onto the device.
   Usage: graphics-camera-rules-test. Exits 1 after the cases when one failed. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "camera_rules.h"

static int failures;

#define EXPECT(condition)                                                                    \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            fprintf(stderr, "FAILED  %s:%d: %s\n", __func__, __LINE__, #condition);          \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static struct minato_camera_format format(int32_t width, int32_t height, float fps_max, float field_of_view) {
    struct minato_camera_format made;
    memset(&made, 0, sizeof made);
    made.width = width, made.height = height, made.yuv = 1, made.fps_max = fps_max;
    made.field_of_view = field_of_view;
    made.iso_min = 50, made.iso_max = 6400;
    made.exposure_min_ns = 14000, made.exposure_max_ns = 1000000000;
    made.zoom_max = 123.75f;
    made.photo_count = 1;
    made.photo[0][0] = width, made.photo[0][1] = height;
    return made;
}

/* A back triple camera: ultra wide first (switch-over to the wide at 2), formats as an iPhone lists them. */
static struct minato_camera_device back_camera(void) {
    struct minato_camera_device device;
    memset(&device, 0, sizeof device);
    device.facing = 1, device.autofocus = 1, device.flash = 1, device.aperture = 1.78f;
    device.min_focus_mm = 200, device.bias_min = -8, device.bias_max = 8, device.wide_factor = 2;
    struct minato_camera_format *formats = device.formats;
    formats[0] = format(640, 480, 30, 73.0f);                                           // 0
    formats[1] = format(1280, 720, 60, 69.0f);                                          // 1
    formats[2] = format(1920, 1080, 60, 69.0f);                                         // 2
    formats[2].photo_count = 2, formats[2].photo[1][0] = 4032, formats[2].photo[1][1] = 2268;
    formats[3] = format(1440, 1080, 30, 73.0f);                                         // 3
    formats[3].photo_count = 2, formats[3].photo[1][0] = 4032, formats[3].photo[1][1] = 3024;
    formats[4] = format(1920, 1440, 30, 73.0f);                                         // 4
    formats[4].photo_count = 2, formats[4].photo[1][0] = 4032, formats[4].photo[1][1] = 3024;
    formats[5] = format(3840, 2160, 60, 69.0f);                                         // 5
    formats[6] = format(4032, 3024, 30, 73.0f);                                         // 6
    formats[6].photo_count = 2, formats[6].photo[1][0] = 8064, formats[6].photo[1][1] = 6048;
    formats[7] = format(1920, 1080, 30, 69.0f);                                         // 7: 10-bit, not YUV 8
    formats[7].yuv = 0;
    formats[8] = format(352, 288, 15, 60.0f);                                           // 8: below 30 fps
    device.format_count = 9;
    return device;
}

/* A front camera: fixed focus, no flash, no ultra wide. */
static struct minato_camera_device front_camera(void) {
    struct minato_camera_device device;
    memset(&device, 0, sizeof device);
    device.facing = 0, device.aperture = 1.9f, device.min_focus_mm = -1, device.bias_min = -1, device.bias_max = 1;
    device.wide_factor = 1;
    device.formats[0] = format(1280, 720, 30, 60.0f);
    device.formats[0].zoom_max = 4;
    device.formats[1] = format(1920, 1440, 30, 63.0f);
    device.formats[1].photo_count = 2, device.formats[1].photo[1][0] = 3088, device.formats[1].photo[1][1] = 2316;
    device.formats[1].zoom_max = 4;
    device.format_count = 2;
    return device;
}

/* The entry of `key` in a facts list: its type and count, and its values' bytes. */
static const uint8_t *fact(const uint8_t *bytes, size_t size, uint32_t key, uint32_t *type, uint32_t *count) {
    for (size_t at = 0; at + 12 <= size;) {
        uint32_t header[3];
        memcpy(header, bytes + at, sizeof header);
        const size_t length = 12 + (size_t)header[2] * (header[1] == APKRT_CAMERA_TYPE_I64 ? 8 : 4);
        if (header[0] == key) {
            *type = header[1], *count = header[2];
            return bytes + at + 12;
        }
        at += (length + 7) & ~(size_t)7;
    }
    return NULL;
}

/* Whether the entry of `key` is I32 `values`. */
static int i32s(const uint8_t *bytes, size_t size, uint32_t key, const int32_t *values, uint32_t count) {
    uint32_t type = 0, got = 0;
    const uint8_t *at = fact(bytes, size, key, &type, &got);
    return at != NULL && type == APKRT_CAMERA_TYPE_I32 && got == count && memcmp(at, values, count * 4u) == 0;
}

static int i64s(const uint8_t *bytes, size_t size, uint32_t key, const int64_t *values, uint32_t count) {
    uint32_t type = 0, got = 0;
    const uint8_t *at = fact(bytes, size, key, &type, &got);
    return at != NULL && type == APKRT_CAMERA_TYPE_I64 && got == count && memcmp(at, values, count * 8u) == 0;
}

/* The F32 entry's value n, or NAN. */
static float f32(const uint8_t *bytes, size_t size, uint32_t key, uint32_t n) {
    uint32_t type = 0, count = 0;
    const uint8_t *at = fact(bytes, size, key, &type, &count);
    if (at == NULL || type != APKRT_CAMERA_TYPE_F32 || n >= count) return NAN;
    float value;
    memcpy(&value, at + 4u * n, 4);
    return value;
}

static int near(float a, float b) {
    return fabsf(a - b) < 0.001f;
}

static void back_facts(void) {
    const struct minato_camera_device device = back_camera();
    uint8_t bytes[4096];
    const size_t size = minato_camera_facts(&device, bytes, sizeof bytes);
    EXPECT(size > 0 && size <= sizeof bytes && size % 8 == 0);
    const int32_t facing = 1, orientation = 90, array[2] = {4032, 3024}, active[4] = {0, 0, 4032, 3024};
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_FACING, &facing, 1));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_ORIENTATION, &orientation, 1));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_PIXEL_ARRAY, array, 2));  // 8064 x 6048 is past 12 MP
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_ACTIVE_ARRAY, active, 4));
    // The sensor from the still's format's field of view (1440 x 1080's 73 degrees) at the nominal focal length.
    const float width = 2.0f * MINATO_CAMERA_FOCAL_LENGTH_MM * tanf(73.0f * 3.14159265f / 360.0f);
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_PHYSICAL_SIZE, 0), width));
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_PHYSICAL_SIZE, 1), width * 3024.0f / 4032.0f));
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_FOCAL_LENGTHS, 0), MINATO_CAMERA_FOCAL_LENGTH_MM));
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_APERTURES, 0), 1.78f));
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_MIN_FOCUS_DISTANCE, 0), 5.0f));  // 200 mm
    const int32_t flash = 1, compensation[4] = {-12, 12, 1, 6};
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_FLASH, &flash, 1));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_AE_COMPENSATION, compensation, 4));  // +-8 EV capped at +-2
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_ZOOM, 0), 0.5f));  // the ultra wide
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_ZOOM, 1), MINATO_CAMERA_MAX_ZOOM_RATIO));
    const int32_t fps[4] = {15, 30, 30, 30}, iso[2] = {50, 6400};
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_FPS_RANGES, fps, 4));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_ISO, iso, 2));
    const int64_t exposure[2] = {14000, 1000000000};
    EXPECT(i64s(bytes, size, APKRT_CAMERA_KEY_EXPOSURE_NS, exposure, 2));
    const int32_t af[4] = {0, 1, 3, 4}, awb = 1, faces = 0;
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_AF_MODES, af, 4));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_AWB_MODES, &awb, 1));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_FACES, &faces, 1));
    // YUV at the 8-bit formats' sizes within 1080p's pixels and 30 fps, largest first; a JPEG at the largest still
    // of each aspect within 12 MP.
    const int64_t frame = 33333333, yuv = APKRT_CAMERA_FORMAT_YUV_420_888, jpeg = APKRT_CAMERA_FORMAT_JPEG;
    const int64_t streams[] = {yuv,  1920, 1080, frame, yuv,  1440, 1080, frame, yuv,  1280, 720, frame,
                               yuv,  640,  480,  frame, jpeg, 4032, 3024, frame, jpeg, 4032, 2268, frame};
    EXPECT(i64s(bytes, size, APKRT_CAMERA_KEY_STREAMS, streams, 24));
    const int64_t stalls[] = {jpeg, 4032, 3024, 200000000, jpeg, 4032, 2268, 200000000};
    EXPECT(i64s(bytes, size, APKRT_CAMERA_KEY_STALLS, stalls, 8));

    // Too small a buffer: the size all the same, nothing written past it.
    uint8_t small[16];
    memset(small, 0xa5, sizeof small);
    EXPECT(minato_camera_facts(&device, small, 8) == size);
    EXPECT(small[8] == 0xa5 && small[15] == 0xa5);
}

static void front_facts(void) {
    const struct minato_camera_device device = front_camera();
    uint8_t bytes[4096];
    const size_t size = minato_camera_facts(&device, bytes, sizeof bytes);
    EXPECT(size > 0 && size <= sizeof bytes);
    const int32_t facing = 0, orientation = 270, array[2] = {3088, 2316}, flash = 0, af = 0;
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_FACING, &facing, 1));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_ORIENTATION, &orientation, 1));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_PIXEL_ARRAY, array, 2));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_FLASH, &flash, 1));
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_AF_MODES, &af, 1));
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_MIN_FOCUS_DISTANCE, 0), 0.0f));  // fixed focus
    const int32_t compensation[4] = {-6, 6, 1, 6};
    EXPECT(i32s(bytes, size, APKRT_CAMERA_KEY_AE_COMPENSATION, compensation, 4));
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_ZOOM, 0), 1.0f));
    EXPECT(near(f32(bytes, size, APKRT_CAMERA_KEY_ZOOM, 1), 4.0f));

    // Focus unknown on a camera that focuses: 10 cm.
    struct minato_camera_device focusing = device;
    focusing.autofocus = 1;
    const size_t again = minato_camera_facts(&focusing, bytes, sizeof bytes);
    EXPECT(near(f32(bytes, again, APKRT_CAMERA_KEY_MIN_FOCUS_DISTANCE, 0), 10.0f));

    // No 8-bit YUV format: no facts.
    struct minato_camera_device none = device;
    none.formats[0].yuv = 0, none.formats[1].yuv = 0;
    EXPECT(minato_camera_facts(&none, bytes, sizeof bytes) == 0);
    int32_t width = 0, height = 0;
    EXPECT(minato_camera_still(&none, &width, &height) == -1);
}

static void still(void) {
    const struct minato_camera_device device = back_camera();
    int32_t width = 0, height = 0;
    EXPECT(minato_camera_still(&device, &width, &height) == 0 && width == 4032 && height == 3024);
}

static int choose(const struct minato_camera_device *device, uint32_t f0, uint32_t w0, uint32_t h0, uint32_t f1,
                  uint32_t w1, uint32_t h1, int32_t fps) {
    const struct apkrt_camera_stream streams[2] = {{f0, w0, h0, 0}, {f1, w1, h1, 0}};
    return minato_camera_choose_format(device, streams, f1 == 0 ? 1 : 2, fps);
}

static void choose_format(void) {
    const struct minato_camera_device device = back_camera();
    const uint32_t yuv = APKRT_CAMERA_FORMAT_YUV_420_888, private = APKRT_CAMERA_FORMAT_PRIVATE;
    const uint32_t jpeg = APKRT_CAMERA_FORMAT_JPEG;
    EXPECT(choose(&device, yuv, 1920, 1080, 0, 0, 0, 0) == 2);       // exact
    EXPECT(choose(&device, private, 640, 480, yuv, 1280, 720, 0) == 1);  // the larger stream's
    EXPECT(choose(&device, private, 1440, 1080, jpeg, 4032, 3024, 0) == 3);
    EXPECT(choose(&device, private, 1920, 1080, jpeg, 4032, 2268, 0) == 2);
    // 16:9 with the 4:3 still: no 16:9 format takes it, the smallest larger one does.
    EXPECT(choose(&device, private, 1920, 1080, jpeg, 4032, 3024, 0) == 4);
    // 640 x 480 with the 4:3 still: the smallest larger 4:3 format with it.
    EXPECT(choose(&device, yuv, 640, 480, jpeg, 4032, 3024, 0) == 3);
    EXPECT(choose(&device, jpeg, 4032, 3024, 0, 0, 0, 0) == 3);      // a still alone: the smallest with it
    EXPECT(choose(&device, yuv, 1280, 720, 0, 0, 0, 60) == 1);
    EXPECT(choose(&device, yuv, 640, 480, 0, 0, 0, 60) == 1);       // 640 x 480 runs at 30 only
    EXPECT(choose(&device, yuv, 352, 288, 0, 0, 0, 0) == 0);         // the 15 fps format cannot run 30
    EXPECT(choose(&device, jpeg, 8064, 6048, 0, 0, 0, 0) == 6);
    EXPECT(choose(&device, yuv, 5000, 4000, 0, 0, 0, 0) == -1);
    EXPECT(minato_camera_choose_format(&device, NULL, 0, 0) == -1);
}

static uint32_t frame(struct minato_camera_3a *state, uint8_t focus, uint8_t exposure) {
    const struct minato_camera_adjusting now = {1, focus, exposure, 0, 0};
    return minato_camera_3a_frame(state, &now);
}

static void focus_states(void) {
    const uint32_t focusing = APKRT_CAMERA_STATE_FOCUSING, focused = APKRT_CAMERA_STATE_FOCUSED;
    struct minato_camera_3a state;
    memset(&state, 0, sizeof state);
    // Continuous: as the device adjusts.
    EXPECT(frame(&state, 0, 0) == focused);
    EXPECT(frame(&state, 1, 0) == focusing);
    EXPECT(frame(&state, 0, 0) == focused);
    // A trigger: FOCUSING from its frame on, through the device's adjusting, FOCUSED once it stopped.
    minato_camera_3a_trigger(&state, 1, 0);
    EXPECT(frame(&state, 0, 0) == focusing);
    EXPECT(frame(&state, 0, 0) == focusing);
    EXPECT(frame(&state, 1, 0) == focusing);
    EXPECT(frame(&state, 1, 0) == focusing);
    EXPECT(frame(&state, 0, 0) == focused);
    EXPECT(state.focus_phase == 0);
    // A trigger the device does not start on: FOCUSED after the start frames.
    minato_camera_3a_trigger(&state, 1, 0);
    for (uint32_t n = 0; n < MINATO_CAMERA_TRIGGER_START_FRAMES; n++) EXPECT(frame(&state, 0, 0) == focusing);
    EXPECT(frame(&state, 0, 0) == focused);
    // A device that adjusts for ever: FOCUSED once after the most frames, then as the device adjusts.
    minato_camera_3a_trigger(&state, 1, 0);
    for (uint32_t n = 0; n < MINATO_CAMERA_TRIGGER_MOST_FRAMES; n++) EXPECT(frame(&state, 1, 0) == focusing);
    EXPECT(frame(&state, 1, 0) == focused);
    EXPECT(frame(&state, 1, 0) == focusing);
    // A cancel ends the trigger.
    minato_camera_3a_trigger(&state, 1, 0);
    EXPECT(frame(&state, 0, 0) == focusing);
    minato_camera_3a_trigger(&state, 2, 0);
    EXPECT(frame(&state, 0, 0) == focused);
    // Fixed focus: always FOCUSED.
    struct minato_camera_3a fixed;
    memset(&fixed, 0, sizeof fixed);
    const struct minato_camera_adjusting none = {0, 1, 0, 0, 0};
    minato_camera_3a_trigger(&fixed, 1, 0);
    EXPECT(minato_camera_3a_frame(&fixed, &none) == focused);
}

static void exposure_states(void) {
    const uint32_t exposing = APKRT_CAMERA_STATE_EXPOSING, focused = APKRT_CAMERA_STATE_FOCUSED;
    struct minato_camera_3a state;
    memset(&state, 0, sizeof state);
    EXPECT(frame(&state, 0, 1) == (focused | exposing));
    EXPECT(frame(&state, 0, 0) == focused);
    minato_camera_3a_trigger(&state, 0, 1);
    EXPECT(frame(&state, 0, 0) == (focused | exposing));
    EXPECT(frame(&state, 0, 1) == (focused | exposing));
    EXPECT(frame(&state, 0, 0) == focused);
    minato_camera_3a_trigger(&state, 0, 1);
    for (uint32_t n = 0; n < MINATO_CAMERA_TRIGGER_START_FRAMES; n++) {
        EXPECT(frame(&state, 0, 0) == (focused | exposing));
    }
    EXPECT(frame(&state, 0, 0) == focused);
    minato_camera_3a_trigger(&state, 0, 1);
    minato_camera_3a_trigger(&state, 0, 2);
    EXPECT(frame(&state, 0, 0) == focused);
    // White balance and the flash as the device has them.
    const struct minato_camera_adjusting balancing = {1, 0, 0, 1, 1};
    EXPECT(minato_camera_3a_frame(&state, &balancing) ==
           (focused | APKRT_CAMERA_STATE_BALANCING | APKRT_CAMERA_STATE_FLASH_READY));
}

static void controls(void) {
    const struct minato_camera_device back = back_camera(), front = front_camera();
    EXPECT(near(minato_camera_zoom_factor(&back, 1.0f, 123.75f), 2.0f));
    EXPECT(near(minato_camera_zoom_factor(&back, 0.5f, 123.75f), 1.0f));
    EXPECT(near(minato_camera_zoom_factor(&back, 10.0f, 15.0f), 15.0f));
    EXPECT(near(minato_camera_zoom_factor(&back, 0.25f, 15.0f), 1.0f));
    EXPECT(near(minato_camera_zoom_factor(&front, 2.0f, 4.0f), 2.0f));
    EXPECT(near(minato_camera_bias(&back, 3), 0.5f));
    EXPECT(near(minato_camera_bias(&back, -12), -2.0f));
    EXPECT(near(minato_camera_bias(&front, 12), 1.0f));  // within the device's range
    float x = 0, y = 0;
    const int32_t whole[5] = {0, 0, 4032, 3024, 1}, corner[5] = {0, 0, 1008, 756, 1}, off[5] = {0, 0, 10, 10, 0};
    EXPECT(minato_camera_point(1, whole, 4032, 3024, &x, &y) == 1 && near(x, 0.5f) && near(y, 0.5f));
    EXPECT(minato_camera_point(1, corner, 4032, 3024, &x, &y) == 1 && near(x, 0.125f) && near(y, 0.125f));
    EXPECT(minato_camera_point(0, corner, 4032, 3024, &x, &y) == 1 && near(x, 0.875f) && near(y, 0.875f));
    EXPECT(minato_camera_point(1, off, 4032, 3024, &x, &y) == 0);
    EXPECT(minato_camera_frame_rotation(1) == 0);
    EXPECT(minato_camera_frame_rotation(0) == 180);
    EXPECT(minato_camera_still_rotation(1, 90) == 90);
    EXPECT(minato_camera_still_rotation(1, 0) == 0);
    EXPECT(minato_camera_still_rotation(0, 270) == 90);
    EXPECT(minato_camera_still_rotation(0, 0) == 180);
}

int main(void) {
    back_facts();
    front_facts();
    still();
    choose_format();
    focus_states();
    exposure_states();
    controls();
    if (failures != 0) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("graphics.camera_rules: all passed\n");
    return 0;
}
