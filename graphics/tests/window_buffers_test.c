/* ADR-0043 b2: graphics/host's buffers for drawing by the CPU (window.m) behind libnativewindow's lock and post, on
   graphics-run's window, ID 1, of 64 x 48 pixels, which shows them as a sublayer's contents: the window host prints
   each buffer shown (MINATO_WINDOW_WATCH=contents), and the Metal hook (MINATO_METAL_HOOK=present:6) would print any
   drawable presented, of which there are none.
   - RGBA_8888 at the window's size: four posts. The first lock answers the whole buffer as dirty, as nothing can be
     copied into it yet; then each lock's pixels outside its dirty rectangle must be the last posted ones (the
     copy-back), and the four locks take the three buffers in turn. The fourth lock waits for its buffer, which the
     window host holds in use until 500 ms after its replacement, as Core Animation holds a buffer it shows.
   - RGBX_8888, its pixels' alpha 0: shown as opaque, on black, as alpha 255.
   - RGB_565 at 32 x 24 (setBuffersGeometry): pure colors in four quadrants, which convert without rounding.
   - A geometry of 16385 on a side: the lock answers -EINVAL (Metal's largest texture side is 16384).
   For each post it prints the CRC-32 the buffer shown must have, of its BGRA bytes row by row as the window host
   reads them, which the host's line (stderr) must show. */
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { width = 64, height = 48, small_width = 32, small_height = 24 };

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

static uint8_t model[height][width][4];  // RGBA, as the last posts left the window's pixels
static const void *seen[4];              // the buffers the locks got, in the order seen
static long long lock_ms;                // how long the last lock took

static long long now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t crc32_of(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < length; index++) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* The CRC-32 of the buffer shown for the model: BGRA, with alpha 255 for an opaque format. */
static uint32_t expected_crc(int opaque) {
    static uint8_t bgra[height][width][4];
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const uint8_t *rgba = model[y][x];
            const uint8_t pixel[4] = {rgba[2], rgba[1], rgba[0], opaque ? 255 : rgba[3]};
            memcpy(bgra[y][x], pixel, 4);
        }
    }
    return crc32_of(&bgra[0][0][0], sizeof bgra);
}

static int inside(const ARect *rect, int x, int y) {
    return x >= rect->left && x < rect->right && y >= rect->top && y < rect->bottom;
}

/* The first pattern: a gradient, and premultiplied translucent pixels in the left 8 columns. */
static void first_pattern(int x, int y, uint8_t rgba[4]) {
    const uint8_t opaque[4] = {(uint8_t)(x * 4), (uint8_t)(y * 5), 128, 255};
    const uint8_t translucent[4] = {(uint8_t)(x * 2), (uint8_t)(y * 2), 64, 128};
    memcpy(rgba, x < 8 ? translucent : opaque, 4);
}

static int index_of(const void *bits) {
    for (int index = 0; index < 4; index++) {
        if (seen[index] == bits || seen[index] == NULL) {
            seen[index] = bits;
            return index;
        }
    }
    return -1;
}

/* One lock of the RGBA_8888 buffer: checks the copy-back outside the answered rectangle, draws `color` in it (the
   first pattern for NULL) and posts. */
static void draw(int number, ANativeWindow *window, ARect asked, const uint8_t *color, int *turn) {
    ANativeWindow_Buffer buffer;
    ARect dirty = asked;
    const long long asked_at = now_ms();
    const int32_t result = ANativeWindow_lock(window, &buffer, &dirty);
    lock_ms = now_ms() - asked_at;
    if (result != 0) {
        printf("lock %d: %d\n", number, result);
        return;
    }
    *turn = index_of(buffer.bits);
    printf("lock %d: %dx%d stride %d format %d, dirty %d %d %d %d", number, buffer.width, buffer.height, buffer.stride,
           buffer.format, dirty.left, dirty.top, dirty.right, dirty.bottom);
    uint8_t *const bits = buffer.bits;
    if (number > 1) {
        int outside = 0, kept = 0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                if (!inside(&dirty, x, y)) {
                    outside++;
                    kept += memcmp(bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * 4, model[y][x], 4) == 0;
                }
            }
        }
        printf(", copy-back: %d of the %d pixels outside it are the last posted ones", kept, outside);
    }
    printf("\n");
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (inside(&dirty, x, y)) {
                if (color != NULL) {
                    memcpy(model[y][x], color, 4);
                } else {
                    first_pattern(x, y, model[y][x]);
                }
                memcpy(bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * 4, model[y][x], 4);
            }
        }
    }
    printf("post %d: %d, contents CRC-32 %08x\n", number, ANativeWindow_unlockAndPost(window), expected_crc(0));
}

int main(void) {
    create_window create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    ANativeWindow *window = create(1, width, height);
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);

    int turns[4];
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
    draw(1, window, (ARect){.left = 16, .top = 8, .right = 48, .bottom = 40}, NULL, &turns[0]);
    draw(2, window, (ARect){.left = 16, .top = 8, .right = 48, .bottom = 40}, red, &turns[1]);
    draw(3, window, (ARect){.left = 0, .top = 0, .right = 8, .bottom = 8}, green, &turns[2]);
    draw(4, window, (ARect){.left = 40, .top = 30, .right = 70, .bottom = 60}, blue, &turns[3]);
    printf("lock 4 waited for its buffer, still in use: %s\n", lock_ms >= 100 ? "yes" : "no");
    printf("buffers in turn: %d %d %d %d\n", turns[0], turns[1], turns[2], turns[3]);

    // RGBX_8888: alpha 0 in memory, 255 on the screen.
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBX_8888);
    ANativeWindow_Buffer buffer;
    int32_t result = ANativeWindow_lock(window, &buffer, NULL);
    printf("lock 5 RGBX_8888: %d", result);
    if (result == 0) {
        printf(", %dx%d stride %d format %d\n", buffer.width, buffer.height, buffer.stride, buffer.format);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const uint8_t pixel[4] = {(uint8_t)(x * 4), 200, (uint8_t)(y * 5), 0};
                memcpy(model[y][x], pixel, 4);
                memcpy((uint8_t *)buffer.bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * 4, pixel, 4);
            }
        }
        printf("post 5: %d, contents CRC-32 %08x\n", ANativeWindow_unlockAndPost(window), expected_crc(1));
    } else {
        printf("\n");
    }

    // RGB_565 at 32 x 24: red, green, blue and white quadrants.
    ANativeWindow_setBuffersGeometry(window, small_width, small_height, WINDOW_FORMAT_RGB_565);
    result = ANativeWindow_lock(window, &buffer, NULL);
    printf("lock 6 RGB_565: %d", result);
    if (result == 0) {
        printf(", %dx%d stride %d format %d\n", buffer.width, buffer.height, buffer.stride, buffer.format);
        static uint8_t bgra[small_height][small_width][4];
        for (int y = 0; y < small_height; y++) {
            for (int x = 0; x < small_width; x++) {
                const int quadrant = (y >= small_height / 2) * 2 + (x >= small_width / 2);
                const uint16_t colors[4] = {0xf800, 0x07e0, 0x001f, 0xffff};
                const uint8_t expected[4][4] = {{0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255},
                                                {255, 255, 255, 255}};
                ((uint16_t *)buffer.bits)[(size_t)y * (size_t)buffer.stride + (size_t)x] = colors[quadrant];
                memcpy(bgra[y][x], expected[quadrant], 4);
            }
        }
        printf("post 6: %d, contents CRC-32 %08x\n", ANativeWindow_unlockAndPost(window),
               crc32_of(&bgra[0][0][0], sizeof bgra));
    } else {
        printf("\n");
    }
    // A side past 16384, a Metal texture's largest (ADR-0060 e2c2 hardening): refused, where Metal would end the
    // process at the buffer's texture.
    ANativeWindow_setBuffersGeometry(window, 16385, 1, WINDOW_FORMAT_RGBA_8888);
    result = ANativeWindow_lock(window, &buffer, NULL);
    printf("lock 16385 x 1: %d\n", result);
    if (result == 0) ANativeWindow_unlockAndPost(window);
    ANativeWindow_setBuffersGeometry(window, 1, 16385, WINDOW_FORMAT_RGBA_8888);
    result = ANativeWindow_lock(window, &buffer, NULL);
    printf("lock 1 x 16385: %d\n", result);
    if (result == 0) ANativeWindow_unlockAndPost(window);
    ANativeWindow_release(window);
    // Metal presents a drawable from its command buffer's scheduled handler, where the hook reads it back: a moment
    // for the last one before the process ends.
    usleep(300000);
    return 0;
}
