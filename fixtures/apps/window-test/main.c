// window-test (ADR-0043 test 3): a NativeActivity on native_app_glue that draws on its window by the CPU, with
// ANativeWindow_lock and ANativeWindow_unlockAndPost, once it has the focus. For RGBA_8888, RGBX_8888 and RGB_565 in
// turn, at 64 x 48 pixels (setBuffersGeometry):
// - the first lock's answer, whose dirty rectangle is the whole buffer, since nothing can be copied into it yet;
// - five locks with a dirty rectangle, each one's pixels outside it the last posted ones (libgui's copy-back), and
//   which of the window's buffers each lock got;
// - a second lock while one is held.
// Then a post without a lock, and ANativeActivity_finish. With the tag "window-test" it logs what Android 15 fixes;
// the strides, which differ by device, go to "window-test-device". 18 posts in all.
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "window-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "window-test-device", __VA_ARGS__)

enum { kWidth = 64, kHeight = 48, kLocks = 6 };

static uint8_t model[kHeight][kWidth][4];  // the last posted pixels, in the format's bytes

static const struct {
    int32_t format;
    const char *name;
    int bytes;
} kFormats[] = {
    {WINDOW_FORMAT_RGBA_8888, "RGBA_8888", 4},
    {WINDOW_FORMAT_RGBX_8888, "RGBX_8888", 4},
    {WINDOW_FORMAT_RGB_565, "RGB_565", 2},
};

// The dirty rectangles of the locks after the first.
static const ARect kDirty[kLocks - 1] = {
    {.left = 16, .top = 8, .right = 48, .bottom = 40}, {.left = 0, .top = 0, .right = 8, .bottom = 8},
    {.left = 40, .top = 30, .right = 70, .bottom = 60}, {.left = 8, .top = 8, .right = 24, .bottom = 24},
    {.left = 0, .top = 40, .right = 64, .bottom = 48},
};

static bool inside(const ARect *rect, int x, int y) {
    return x >= rect->left && x < rect->right && y >= rect->top && y < rect->bottom;
}

// The bytes of lock `round`'s pixel at x, y: different in every round.
static void pixel(int round, int bytes, int x, int y, uint8_t *out) {
    const uint8_t values[4] = {(uint8_t)(x * 4 + round), (uint8_t)(y * 5 + round * 7), (uint8_t)(round * 40),
                               (uint8_t)(round * 30 + 15)};
    memcpy(out, values, (size_t)bytes);
}

static void draw(ANativeWindow *window, int format) {
    const int bytes = kFormats[format].bytes;
    const char *name = kFormats[format].name;
    LOG("%s: setBuffersGeometry %d", name,
        ANativeWindow_setBuffersGeometry(window, kWidth, kHeight, kFormats[format].format));
    const void *seen[kLocks] = {NULL};
    char turns[64] = "";
    for (int round = 0; round < kLocks; round++) {
        ARect dirty = round == 0 ? (ARect){.left = 16, .top = 8, .right = 48, .bottom = 40} : kDirty[round - 1];
        ANativeWindow_Buffer buffer;
        const int32_t locked = ANativeWindow_lock(window, &buffer, &dirty);
        if (locked != 0) {
            LOG("%s: lock %d", name, locked);
            return;
        }
        int turn = 0;
        while (turn < kLocks && seen[turn] != NULL && seen[turn] != buffer.bits) turn++;
        if (turn < kLocks) seen[turn] = buffer.bits;
        snprintf(turns + strlen(turns), sizeof turns - strlen(turns), " %d", turn);
        uint8_t *const bits = buffer.bits;
        if (round == 0) {
            LOG("%s: lock 0, %dx%d format %d, dirty %d %d %d %d", name, buffer.width, buffer.height, buffer.format,
                dirty.left, dirty.top, dirty.right, dirty.bottom);
            DEVICE("%s: stride %d", name, buffer.stride);
            ANativeWindow_Buffer again;
            LOG("%s: a second lock while one is held: %d", name, ANativeWindow_lock(window, &again, NULL));
        } else {
            int outside = 0, kept = 0;
            for (int y = 0; y < kHeight; y++) {
                for (int x = 0; x < kWidth; x++) {
                    if (!inside(&dirty, x, y)) {
                        outside++;
                        kept += memcmp(bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * (size_t)bytes,
                                       model[y][x], (size_t)bytes) == 0;
                    }
                }
            }
            LOG("%s: lock 0, dirty %d %d %d %d, copy-back: %d of the %d pixels outside it are the last posted ones",
                name, dirty.left, dirty.top, dirty.right, dirty.bottom, kept, outside);
        }
        for (int y = 0; y < kHeight; y++) {
            for (int x = 0; x < kWidth; x++) {
                if (inside(&dirty, x, y)) {
                    pixel(round, bytes, x, y, model[y][x]);
                    memcpy(bits + ((size_t)y * (size_t)buffer.stride + (size_t)x) * (size_t)bytes, model[y][x],
                           (size_t)bytes);
                }
            }
        }
        LOG("%s: post %d", name, ANativeWindow_unlockAndPost(window));
    }
    LOG("%s: buffers in turn:%s", name, turns);
}

static void on_command(struct android_app *app, int32_t command) {
    bool *done = app->userData;
    if (command != APP_CMD_GAINED_FOCUS || *done || app->window == NULL) return;
    *done = true;
    for (int format = 0; format < 3; format++) draw(app->window, format);
    LOG("post without a lock: %d", ANativeWindow_unlockAndPost(app->window));
    LOG("ANativeActivity_finish");
    ANativeActivity_finish(app->activity);
}

void android_main(struct android_app *app) {
    bool done = false;
    app->userData = &done;
    app->onAppCmd = on_command;
    LOG("android_main");
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(-1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
    }
    LOG("android_main returns");
}
