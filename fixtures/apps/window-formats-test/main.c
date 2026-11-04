// window-formats-test (ADR-0043 b5, step f): a NativeActivity on native_app_glue that shows its whole window by the
// CPU, for tools/window-contents-check.sh's screenshots. Once it has the focus, it shows RGBA_8888, RGBX_8888 and
// RGB_565 in turn, 5 s each (time for SessionCheck's dump and a screenshot on the iPhone), at the window's own size
// (setBuffersGeometry 0 x 0), one post each, and logs "showing <format>" after each post. The buffer is a 3 x 3 grid
// of plain colors, whose centers are the check's sample points:
// - RGBA_8888: the center premultiplied at half alpha, the others opaque;
// - RGBX_8888: every X byte 0x40, which the window must not take for alpha;
// - RGB_565: colors whose 5 and 6 bits widen to the 8 bits written.
// RGB_565 stays shown until the window goes (HOME); on the return it is shown again on the new window, for 5 s, and
// then ANativeActivity_finish. With the tag "window-formats-test" it logs what Android 15 fixes; the window's size and
// the strides, which differ by device, go to "window-formats-test-device".
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "window-formats-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "window-formats-test-device", __VA_ARGS__)

enum { kFormats = 3, kShownMs = 5000 };

static const struct {
    int32_t format;
    const char *name;
} kFormat[kFormats] = {
    {WINDOW_FORMAT_RGBA_8888, "RGBA_8888"},
    {WINDOW_FORMAT_RGBX_8888, "RGBX_8888"},
    {WINDOW_FORMAT_RGB_565, "RGB_565"},
};

// The grid's colors, row by row, as R, G, B. 0x84 and 0x82 are 565's 16 of 31 and 32 of 63 widened.
static const uint8_t kGrid[9][3] = {
    {0xff, 0x00, 0x00}, {0x00, 0xff, 0x00}, {0x00, 0x00, 0xff}, {0xff, 0xff, 0x00}, {0x84, 0x82, 0x84},
    {0x00, 0xff, 0xff}, {0xff, 0x00, 0xff}, {0xff, 0xff, 0xff}, {0x84, 0x00, 0x00},
};

struct state {
    int shown;         // formats shown so far
    bool waiting;      // RGB_565 shown, waiting for the window to go
    bool returned;     // the window went and came back
    int64_t next_ms;   // when the next step is due, or 0
};

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void fill(const ANativeWindow_Buffer *buffer, int format) {
    for (int y = 0; y < buffer->height; y++) {
        uint8_t *line = (uint8_t *)buffer->bits + (size_t)y * (size_t)buffer->stride * (format == 2 ? 2 : 4);
        const int row = y * 3 / buffer->height;
        for (int x = 0; x < buffer->width; x++) {
            const int cell = row * 3 + x * 3 / buffer->width;
            const uint8_t *rgb = kGrid[cell];
            if (format == 2) {
                const uint16_t value = (uint16_t)((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3);
                memcpy(line + (size_t)x * 2, &value, 2);
            } else if (format == 1) {
                const uint8_t bytes[4] = {rgb[0], rgb[1], rgb[2], 0x40};
                memcpy(line + (size_t)x * 4, bytes, 4);
            } else {
                const bool half = cell == 4;
                const uint8_t bytes[4] = {(uint8_t)(half ? rgb[0] / 2 : rgb[0]), (uint8_t)(half ? rgb[1] / 2 : rgb[1]),
                                          (uint8_t)(half ? rgb[2] / 2 : rgb[2]), (uint8_t)(half ? 0x80 : 0xff)};
                memcpy(line + (size_t)x * 4, bytes, 4);
            }
        }
    }
}

static void show(ANativeWindow *window, int format) {
    const char *name = kFormat[format].name;
    LOG("%s: setBuffersGeometry %d", name, ANativeWindow_setBuffersGeometry(window, 0, 0, kFormat[format].format));
    ANativeWindow_Buffer buffer;
    const int32_t locked = ANativeWindow_lock(window, &buffer, NULL);
    if (locked != 0) {
        LOG("%s: lock %d", name, locked);
        return;
    }
    LOG("%s: lock 0, format %d", name, buffer.format);
    DEVICE("%s: %dx%d stride %d", name, buffer.width, buffer.height, buffer.stride);
    fill(&buffer, format);
    const int32_t posted = ANativeWindow_unlockAndPost(window);
    if (posted != 0) {
        LOG("%s: post %d", name, posted);
        return;
    }
    LOG("showing %s", name);
}

// The next step, when it is due and the window is there.
static void step(struct android_app *app) {
    struct state *state = app->userData;
    if (state->next_ms == 0 || now_ms() < state->next_ms || app->window == NULL) return;
    state->next_ms = 0;
    if (state->returned) {
        LOG("ANativeActivity_finish");
        ANativeActivity_finish(app->activity);
    } else if (state->shown < kFormats) {
        show(app->window, state->shown++);
        if (state->shown < kFormats) {
            state->next_ms = now_ms() + kShownMs;
        } else {
            state->waiting = true;
            LOG("waiting for the window to go");
        }
    }
}

static void on_command(struct android_app *app, int32_t command) {
    struct state *state = app->userData;
    if (command == APP_CMD_GAINED_FOCUS && state->shown == 0 && state->next_ms == 0) {
        state->next_ms = now_ms();
    } else if (command == APP_CMD_TERM_WINDOW && state->waiting) {
        state->waiting = false;
        state->returned = true;
        LOG("the window goes");
    } else if (command == APP_CMD_INIT_WINDOW && state->returned && app->window != NULL) {
        LOG("a new window");
        show(app->window, kFormats - 1);
        state->next_ms = now_ms() + kShownMs;
    }
    step(app);
}

void android_main(struct android_app *app) {
    struct state state = {0};
    app->userData = &state;
    app->onAppCmd = on_command;
    LOG("android_main");
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        int timeout = -1;
        if (state.next_ms != 0) {
            const int64_t left = state.next_ms - now_ms();
            timeout = left > 0 ? (int)left : 0;
        }
        const int result = ALooper_pollOnce(timeout, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        step(app);
    }
    LOG("android_main returns");
}
