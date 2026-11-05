/* graphics.window_readback (ADR-0043, the text selection's loupe): graphics/host's buffers (window.m) behind the
   functions minato_graphics_set_window_buffers gives the runtime, without the runtime's windows: a window's last
   posted buffer read back as RGBA_8888 (its rectangle exactly, RGBX_8888's alpha as 255, RGB_565 widened, a scaled
   copy), the destination's padding left alone, and the answers for no buffer yet, an unknown or removed window, a
   trimmed one and rectangles outside the buffer or empty; a lock and post go on after a readback.
   Usage: graphics-window-readback-test. Exits 1 after the cases when one failed. */
#import <Foundation/Foundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "minato_graphics.h"

enum { linux_enodata = 61, linux_einval = 22 };
enum { rgba_8888 = 1, rgbx_8888 = 2, rgb_565 = 4 };

static int failures;

#define EXPECT(condition)                                                                    \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            fprintf(stderr, "FAILED  %s:%d: %s\n", __func__, __LINE__, #condition);          \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static const struct apkrt_window_buffers *table;

/* A pixel of the pattern: x and y in red and green, a fixed blue, an alpha of its own. */
static void pattern(int32_t x, int32_t y, uint8_t out[4]) {
    out[0] = (uint8_t)(x * 4), out[1] = (uint8_t)(y * 5), out[2] = 0x80, out[3] = (uint8_t)(200 + (x % 7));
}

/* Locks window `id` at 64 x 48 in `format`, draws the pattern (565: its five, six and five top bits) and posts it. */
static int draw(uint64_t id, int32_t format) {
    struct apkrt_window_buffer buffer = {64, 48, 0, format, NULL, {0, 0, 64, 48}};
    const int locked = table->lock(table->context, id, &buffer);
    if (locked != 0) return locked;
    for (int32_t y = 0; y < 48; y++) {
        for (int32_t x = 0; x < 64; x++) {
            uint8_t color[4];
            pattern(x, y, color);
            if (format == rgb_565) {
                uint16_t *row = (uint16_t *)buffer.bits + (size_t)y * (size_t)buffer.stride;
                row[x] = (uint16_t)((color[0] >> 3) << 11 | (color[1] >> 2) << 5 | color[2] >> 3);
            } else {
                memcpy((uint8_t *)buffer.bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * 4, color, 4);
            }
        }
    }
    return table->post(table->context, id);
}

static int read_back(uint64_t id, int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t width,
                     uint32_t height, uint32_t stride, uint8_t *pixels) {
    const struct apkrt_window_readback request = {left, top, right, bottom, width, height, stride, pixels};
    return table->readback(table->context, id, &request);
}

static void exact_rectangle(void) {
    EXPECT(read_back(101, 0, 0, 8, 8, 8, 8, 32, (uint8_t[256]){0}) == -linux_enodata);  // no window yet
    EXPECT(draw(101, rgba_8888) == 0);
    // 20 x 20 from (10, 5), rows of 96 bytes: 16 of padding a row stay as they were.
    uint8_t pixels[20 * 96];
    memset(pixels, 0xee, sizeof pixels);
    EXPECT(read_back(101, 10, 5, 30, 25, 20, 20, 96, pixels) == 0);
    int wrong = 0, touched = 0;
    for (int32_t y = 0; y < 20; y++) {
        for (int32_t x = 0; x < 20; x++) {
            uint8_t color[4];
            pattern(10 + x, 5 + y, color);
            wrong += memcmp(pixels + (size_t)y * 96 + (size_t)x * 4, color, 4) != 0;
        }
        for (int n = 80; n < 96; n++) touched += pixels[(size_t)y * 96 + (size_t)n] != 0xee;
    }
    EXPECT(wrong == 0);
    EXPECT(touched == 0);
}

static void other_formats(void) {
    EXPECT(draw(102, rgbx_8888) == 0);
    uint8_t pixels[4 * 4 * 4];
    EXPECT(read_back(102, 60, 44, 64, 48, 4, 4, 16, pixels) == 0);
    int wrong = 0;
    for (int32_t y = 0; y < 4; y++) {
        for (int32_t x = 0; x < 4; x++) {
            uint8_t color[4];
            pattern(60 + x, 44 + y, color);
            color[3] = 255;  // RGBX: no alpha
            wrong += memcmp(pixels + (size_t)(y * 4 + x) * 4, color, 4) != 0;
        }
    }
    EXPECT(wrong == 0);
    EXPECT(draw(103, rgb_565) == 0);
    EXPECT(read_back(103, 3, 2, 7, 6, 4, 4, 16, pixels) == 0);
    wrong = 0;
    for (int32_t y = 0; y < 4; y++) {
        for (int32_t x = 0; x < 4; x++) {
            uint8_t color[4];
            pattern(3 + x, 2 + y, color);
            const uint32_t red = color[0] >> 3, green = color[1] >> 2, blue = color[2] >> 3;
            const uint8_t wide[4] = {(uint8_t)((red * 255 + 15) / 31), (uint8_t)((green * 255 + 31) / 63),
                                     (uint8_t)((blue * 255 + 15) / 31), 255};
            wrong += memcmp(pixels + (size_t)(y * 4 + x) * 4, wide, 4) != 0;
        }
    }
    EXPECT(wrong == 0);
}

/* A window of four flat quarters, read back at half its size: each quarter's middle keeps its color. */
static void scaled(void) {
    struct apkrt_window_buffer buffer = {64, 48, 0, rgba_8888, NULL, {0, 0, 64, 48}};
    EXPECT(table->lock(table->context, 104, &buffer) == 0);
    if (buffer.bits == NULL) return;
    static const uint8_t quarters[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
    for (int32_t y = 0; y < 48; y++) {
        for (int32_t x = 0; x < 64; x++) {
            memcpy((uint8_t *)buffer.bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * 4,
                   quarters[(y >= 24) * 2 + (x >= 32)], 4);
        }
    }
    EXPECT(table->post(table->context, 104) == 0);
    uint8_t pixels[32 * 24 * 4];
    EXPECT(read_back(104, 0, 0, 64, 48, 32, 24, 32 * 4, pixels) == 0);
    const int32_t middles[4][2] = {{8, 6}, {24, 6}, {8, 18}, {24, 18}};
    for (int n = 0; n < 4; n++) {
        EXPECT(memcmp(pixels + (size_t)(middles[n][1] * 32 + middles[n][0]) * 4, quarters[n], 4) == 0);
    }
    // Twice its size: the same.
    uint8_t *large = malloc(128 * 96 * 4);
    EXPECT(large != NULL && read_back(104, 0, 0, 64, 48, 128, 96, 128 * 4, large) == 0);
    if (large != NULL) {
        EXPECT(memcmp(large + (size_t)(20 * 128 + 20) * 4, quarters[0], 4) == 0);
        EXPECT(memcmp(large + (size_t)(80 * 128 + 110) * 4, quarters[3], 4) == 0);
    }
    free(large);
}

static void refused(void) {
    uint8_t pixels[64 * 48 * 4];
    EXPECT(read_back(101, 0, 0, 65, 48, 65, 48, 65 * 4, (uint8_t[65 * 48 * 4]){0}) == -linux_einval);  // past right
    EXPECT(read_back(101, -1, 0, 10, 10, 11, 10, 44, pixels) == -linux_einval);
    EXPECT(read_back(101, 0, 40, 10, 49, 10, 9, 40, pixels) == -linux_einval);
    EXPECT(read_back(101, 5, 5, 5, 10, 1, 5, 4, pixels) == -linux_einval);  // empty
    EXPECT(read_back(101, 10, 10, 5, 20, 5, 10, 20, pixels) == -linux_einval);  // right before left
    EXPECT(read_back(999, 0, 0, 8, 8, 8, 8, 32, pixels) == -linux_enodata);  // not a window of the buffers'
    // A lock and post after the readbacks: the window goes on, and its new pixels read back.
    EXPECT(draw(101, rgba_8888) == 0);
    EXPECT(read_back(101, 0, 0, 64, 48, 64, 48, 64 * 4, pixels) == 0);
    uint8_t color[4];
    pattern(63, 47, color);
    EXPECT(memcmp(pixels + (size_t)(47 * 64 + 63) * 4, color, 4) == 0);
    // Removed: no buffer.
    table->removed(table->context, 102);
    EXPECT(read_back(102, 0, 0, 4, 4, 4, 4, 16, pixels) == -linux_enodata);
    // Trimmed: the buffers went; the next post gives one again.
    table->trim(table->context);
    EXPECT(read_back(101, 0, 0, 8, 8, 8, 8, 32, pixels) == -linux_enodata);
    EXPECT(draw(101, rgba_8888) == 0);
    EXPECT(read_back(101, 0, 0, 8, 8, 8, 8, 32, pixels) == 0);
}

int main(void) {
    @autoreleasepool {
        if (minato_graphics_set_window_buffers() != 0) {
            fprintf(stderr, "FAILED  the window buffers were not set\n");
            return 1;
        }
        table = minato_graphics_window_buffers();
        if (table == NULL || table->readback == NULL) {
            fprintf(stderr, "FAILED  no readback\n");
            return 1;
        }
        exact_rectangle();
        other_formats();
        scaled();
        refused();
    }
    if (failures != 0) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("graphics.window_readback: all passed\n");
    return 0;
}
