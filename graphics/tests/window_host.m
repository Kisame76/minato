/* For test 6 of ADR-0030 in graphics-run: the host's window as the app gives it (ADR-0028, decision 6).
   - With MINATO_WINDOW=<width>x<height>, before the program starts, it adds a window of that many pixels on
     graphics/host's MinatoMetalLayer, as the app's game view has, of as many points at scale 1 and opens its gate:
     the program's first window, ID 1.
   - With MINATO_WINDOW_SCRIPT=background:<n> too, a thread of its own waits until the window has <n>
     presents and then keeps the app's order at background (ADR-0028, decision 6): the background flag,
     "scheduled" with 1 s to go, the window's removal; 100 ms later the foreground. With background:<n>:<ms>, it
     stays <ms> in the background between "scheduled" and the removal, which the count of guest commits covers.
     It prints each step to stderr, how many command buffers other threads than the guest's committed during
     "scheduled" (one per queue: graphics/host's), and how many guest threads committed between "scheduled" and
     the foreground, which must be none (the Metal hook counts both, test_host.h).
   - With MINATO_WINDOW_SCRIPT=glue instead (test 7 of ADR-0029), it begins a session before the program
     starts, whose end ends the process with the program's status, and a thread of its own plays the app for
     a program that plays activity-host: the events of a start with the window, until the window has 10
     presents; the focus lost, until the app stops drawing (the NDK sample stops at APP_CMD_LOST_FOCUS); the
     gate closed; the order at background of ADR-0029's decision 15 (the background flag, "scheduled", the
     window's removal, only then onPause, the window's destruction, the saved state and onStop, each answer
     within 500 ms of its event); the foreground and onDestroy. It prints each step to stderr, and the command
     buffers guest threads committed from "scheduled" to the foreground, which must be none.
   - With MINATO_WINDOW_SCRIPT=trim:<n> instead, it begins a session as for glue, and a thread of its own waits until
     the window has <n> presents and then posts APKRT_EVENT_TRIM_MEMORY at TRIM_MEMORY_UI_HIDDEN (20), as the App does
     when the scene goes to the background, which the program, no activity-host, never reads.
   - With MINATO_WINDOW_TICKS=<hz>:<n> too, a thread of its own ticks the display link (apkrt_vsync_post) at <hz>
     until the window has <n> presents, and then stops, which it prints to stderr (ADR-0043 b5's back-pressure).
   - With MINATO_WINDOW_RECORD=<capacity>:<n> too, the layer records from the start (ADR-0043 b5, detail 5), and
     100 ms after the window's <n>th present the window host prints whether each record is a post by contents,
     tick-timed, and each one with a presented time is on the grid of the last tick it posted before the post's
     commit: the target of the first refresh after the commit.
   - With MINATO_WINDOW_OPAQUE=1 too, the layer is opaque; with MINATO_WINDOW_WATCH=hidden, it prints each change of
     the layer's hidden to stderr (ADR-0043 b3, the hide rule), with MINATO_WINDOW_WATCH=contents each buffer that
     graphics/host's window shows by contents (session_layers.h), and with "hidden,contents" both.
   - With MINATO_WINDOW_BUFFERS=fake too (ADR-0043 b1, graphics.window_lock), graphics_run.c puts a fake in the place
     of graphics/host's buffers (minato_test_use_fake_buffers), one that
     prints to stderr what reaches it: lock takes RGBA_8888, RGBX_8888 and RGB_565 and refuses other formats with
     -EINVAL; its buffer starts cleared, with the width rounded up to 16 pixels as its stride; the dirty rectangle it
     answers is the whole buffer for another window or geometry than the last posted one's, else the app's cut to the
     buffer, and 0 0 0 0 when that leaves nothing. post prints the buffer's first pixel. The window's gate stays
     closed until 200 ms after the first lock, so that the first post waits at it.
   - With MINATO_WINDOW_DUMP=<folder> too (ADR-0043 c3), which needs MINATO_WINDOW_BUFFERS=fake, the fake buffers also
     serve the session's window, which apk-run --lifecycle adds, without MINATO_WINDOW, and after each post they
     write the window's contents so far: each post's dirty rectangle over what the window's posts before it left, as
     the layer shows them, in <folder>/post-<n>.rgba (width x height x 4 bytes, the buffer's bytes, whose fourth byte
     a reader takes as 255 for RGBX_8888; RGB_565 is left out), with the line
     "<n> window <id> <width>x<height> format <format> dirty <left> <top> <right> <bottom>" in <folder>/posts.txt,
     which the first post starts anew. A new window or geometry starts cleared. graphics/host's buffers copy the
     last post's pixels outside the dirty rectangle back into the buffer, as libgui does, where the dump takes only
     the rectangle from the fake's cleared buffer: the same contents for an app that draws within the rectangle, as
     Surface.lockCanvas's canvas, cut to it, does.
   - With MINATO_SESSION_LAYERS=1, apk-run's --lifecycle gets session_layers.m's layers (apk_run_layers.h). */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "minato_layer.h"
#include "session_layers.h"
#include "test_host.h"

atomic_int minato_test_counting, minato_test_guest_commits, minato_test_asking, minato_test_asker_commits;

uint64_t minato_test_window;
static unsigned long presents;
static int hold_ms;  /* background:<presents>:<hold>, the time in the background before the window's removal */

/* An event and the milliseconds until its answer, -1 without one within ADR-0032's 2 s (10 s for the first,
   which waits for the program's start). */
static double posted(uint32_t type, uint64_t a0, uint64_t a1, uint64_t a2) {
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const uint32_t serial = apkrt_session_post(type, a0, a1, a2);
    const double seconds = type == APKRT_EVENT_CREATE ? 10.0 : 2.0;
    return apkrt_session_wait(serial, seconds) == 1 ? (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e6
                                                     : -1;
}

static const char *within(double milliseconds) {
    return milliseconds >= 0 && milliseconds < 500 ? "yes" : "no";
}

static void *glue(void *unused) {
    (void)unused;
    int32_t width = 0, height = 0;
    apkrt_window_size(minato_test_window, &width, &height);
    posted(APKRT_EVENT_CREATE, 0, 0, 0);
    posted(APKRT_EVENT_START, 0, 0, 0);
    posted(APKRT_EVENT_RESUME, 0, 0, 0);
    posted(APKRT_EVENT_NATIVE_WINDOW_CREATED, minato_test_window, (uint64_t)width, (uint64_t)height);
    posted(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 1, 0, 0);
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    while (apkrt_window_presents(minato_test_window) < 10 &&
           clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start < 5000000000u) {
        usleep(1000);
    }
    fprintf(stderr, "host: the app drew: %s\n", apkrt_window_presents(minato_test_window) >= 10 ? "yes" : "no");
    posted(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0, 0, 0);
    uint64_t before, after = apkrt_window_presents(minato_test_window);
    do {
        before = after;
        usleep(100000);
        after = apkrt_window_presents(minato_test_window);
    } while (after != before && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start < 5000000000u);
    fprintf(stderr, "host: the app stopped drawing: %s\n", after == before ? "yes" : "no");
    apkrt_window_set_gate(minato_test_window, 0);
    fprintf(stderr, "host: in the background\n");
    apkrt_set_background(1);
    const int scheduled = apkrt_wait_scheduled(1.0);
    atomic_store(&minato_test_guest_commits, 0);
    atomic_store(&minato_test_counting, 1);
    fprintf(stderr, "host: \"scheduled\": %d\n", scheduled);
    fprintf(stderr, "host: the window removed: %d\n", apkrt_window_remove(minato_test_window));
    fprintf(stderr, "host: onPause answered within 500 ms: %s\n", within(posted(APKRT_EVENT_PAUSE, 0, 0, 0)));
    fprintf(stderr, "host: onNativeWindowDestroyed answered within 500 ms: %s\n",
            within(posted(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, minato_test_window, 0, 0)));
    fprintf(stderr, "host: onSaveInstanceState and onStop answered within 500 ms: %s, %s\n",
            within(posted(APKRT_EVENT_SAVE_INSTANCE_STATE, 0, 0, 0)), within(posted(APKRT_EVENT_STOP, 0, 0, 0)));
    atomic_store(&minato_test_counting, 0);
    fprintf(stderr, "host: guest commits in the background: %d\n", atomic_load(&minato_test_guest_commits));
    fprintf(stderr, "host: in the foreground\n");
    apkrt_set_background(0);
    apkrt_session_post(APKRT_EVENT_DESTROY, 0, 0, 0);
    return NULL;
}

static void *trim(void *unused) {
    (void)unused;
    while (apkrt_window_presents(minato_test_window) < presents) {
        usleep(1000);
    }
    fprintf(stderr, "host: trim memory at UI_HIDDEN: %s\n",
            apkrt_session_post(APKRT_EVENT_TRIM_MEMORY, 20, 0, 0) != 0 ? "posted" : "not posted");
    return NULL;
}

/* MINATO_WINDOW_TICKS=<hz>:<presents>: the display link's ticks, until the window has that many presents. */
static int tick_hz;
static unsigned long tick_presents;
enum { tick_capacity = 4096 };
static int64_t tick_times[tick_capacity];
static _Atomic(unsigned) tick_count;

static void *tick(void *unused) {
    (void)unused;
    const int64_t period = 1000000000 / tick_hz;
    while (apkrt_window_presents(minato_test_window) < tick_presents) {
        const int64_t now = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        const unsigned index = atomic_load(&tick_count);
        if (index < tick_capacity) {
            tick_times[index] = now;
            atomic_store(&tick_count, index + 1);
        }
        apkrt_vsync_post(now, now + period);
        usleep((useconds_t)(period / 1000));
    }
    fprintf(stderr, "window host: the display link stops after %lu presents\n", tick_presents);
    return NULL;
}

/* MINATO_WINDOW_RECORD: the window layer's recording, checked after that many presents. */
static MinatoMetalLayer *recorded;
static unsigned long recorded_presents;

static void *check_recording(void *unused) {
    (void)unused;
    while (apkrt_window_presents(minato_test_window) < recorded_presents) {
        usleep(1000);
    }
    usleep(100000);
    MinatoLayerRecording *recording = [recorded stopRecording:0];
    const int64_t period = tick_hz > 0 ? 1000000000 / tick_hz : 0;
    NSUInteger tick_timed = 0, presented = 0, on_grid = 0;
    for (NSUInteger index = 0; index < recording.count; index++) {
        const struct minato_layer_drawable drawable = [recording drawable:index];
        tick_timed += drawable.tick_timed == 1 && drawable.drawable_id == 0;
        if (drawable.presented_ns == 0) {
            continue;
        }
        presented++;
        // The last tick before the commit, the first refresh after the commit on its grid, and that one's target.
        int64_t last = -1;
        for (unsigned at = 0; at < atomic_load(&tick_count); at++) {
            if (tick_times[at] <= drawable.next_drawable_ns) {
                last = tick_times[at];
            }
        }
        const int64_t refresh = last + ((drawable.next_drawable_ns - last) / period + 1) * period;
        on_grid += last >= 0 && drawable.presented_ns == refresh + period;
    }
    fprintf(stderr,
            "window host: the recording: %lu posts, each tick-timed: %s; each presented one at the target of the first "
            "refresh after its commit: %s; at least 25 presented: %s\n",
            (unsigned long)recording.count, recording.count > 0 && tick_timed == recording.count ? "yes" : "no",
            presented > 0 && on_grid == presented ? "yes" : "no", presented >= 25 ? "yes" : "no");
    return NULL;
}

/* The program's end ends the process, with its status. */
static void ended(void *context, int status) {
    (void)context;
    _exit(status);
}

static void *background(void *unused) {
    (void)unused;
    while (apkrt_window_presents(minato_test_window) < presents) {
        usleep(1000);
    }
    fprintf(stderr, "host: in the background\n");
    apkrt_set_background(1);
    atomic_store(&minato_test_asking, 1);
    const int scheduled = apkrt_wait_scheduled(1.0);
    atomic_store(&minato_test_asking, 0);
    fprintf(stderr, "host: \"scheduled\": %d, command buffers committed meanwhile by other threads than the guest's: %d\n",
            scheduled, atomic_load(&minato_test_asker_commits));
    atomic_store(&minato_test_counting, 1);
    usleep((useconds_t)hold_ms * 1000);
    fprintf(stderr, "host: the window removed: %d\n", apkrt_window_remove(minato_test_window));
    usleep(100000);
    atomic_store(&minato_test_counting, 0);
    fprintf(stderr, "host: guest commits after \"scheduled\": %d\n", atomic_load(&minato_test_guest_commits));
    fprintf(stderr, "host: in the foreground\n");
    apkrt_set_background(0);
    return NULL;
}

/* The fake buffers of MINATO_WINDOW_BUFFERS=fake, on the program's one thread, for one window at a time: a lock and
   its post are one window's. */
static struct {
    int locks;
    int32_t width, height, format, stride;  // the locked buffer's
    int32_t dirty[4];                        // the dirty rectangle the lock answered
    uint64_t posted_id;                      // the last post's window, and its geometry
    int32_t posted_width, posted_height, posted_format;
    void *pixels;
    // MINATO_WINDOW_DUMP's: the folder, the window's contents so far, its ID and the posts written.
    const char *dump;
    uint8_t *window_pixels;
    uint64_t window_id;
    int posts;
} fake;

static void *open_gate_later(void *unused) {
    (void)unused;
    usleep(200000);
    fprintf(stderr, "window host: the gate opens\n");
    apkrt_window_set_gate(minato_test_window, 1);
    return NULL;
}

static int fake_lock(void *context, uint64_t id, struct apkrt_window_buffer *buffer) {
    (void)context;
    fprintf(stderr, "fake buffers: lock window %llu, %dx%d, format %d, dirty %d %d %d %d\n", (unsigned long long)id,
            buffer->width, buffer->height, buffer->format, buffer->dirty[0], buffer->dirty[1], buffer->dirty[2],
            buffer->dirty[3]);
    // AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, R8G8B8X8_UNORM and R5G6B5_UNORM.
    const int bytes = buffer->format == 1 || buffer->format == 2 ? 4 : buffer->format == 4 ? 2 : 0;
    if (bytes == 0) {
        return -22;  // EINVAL
    }
    if (fake.locks++ == 0 && minato_test_window != 0) {
        pthread_t thread;
        pthread_create(&thread, NULL, open_gate_later, NULL);
        pthread_detach(thread);
    }
    const int32_t stride = (buffer->width + 15) / 16 * 16;
    fake.pixels = calloc((size_t)stride * (size_t)buffer->height, (size_t)bytes);
    if (fake.pixels == NULL) {
        fprintf(stderr, "fake buffers: no memory for a %dx%d buffer\n", buffer->width, buffer->height);
        exit(2);
    }
    buffer->stride = stride;
    buffer->bits = fake.pixels;
    int32_t *const dirty = buffer->dirty;
    if (id != fake.posted_id || buffer->width != fake.posted_width || buffer->height != fake.posted_height ||
        buffer->format != fake.posted_format) {
        dirty[0] = dirty[1] = 0;
        dirty[2] = buffer->width;
        dirty[3] = buffer->height;
    } else {
        dirty[0] = dirty[0] < 0 ? 0 : dirty[0];
        dirty[1] = dirty[1] < 0 ? 0 : dirty[1];
        dirty[2] = dirty[2] > buffer->width ? buffer->width : dirty[2];
        dirty[3] = dirty[3] > buffer->height ? buffer->height : dirty[3];
        if (dirty[0] >= dirty[2] || dirty[1] >= dirty[3]) {
            memset(dirty, 0, 4 * sizeof *dirty);
        }
    }
    fake.width = buffer->width;
    fake.height = buffer->height;
    fake.format = buffer->format;
    fake.stride = stride;
    memcpy(fake.dirty, dirty, sizeof fake.dirty);
    return 0;
}

/* MINATO_WINDOW_DUMP: the post's dirty rectangle over the window's contents so far, written out. */
static void dump_post(uint64_t id) {
    if (fake.format == 4) {
        return;
    }
    const size_t row = (size_t)fake.width * 4;
    if (fake.window_pixels == NULL || id != fake.window_id || fake.width != fake.posted_width ||
        fake.height != fake.posted_height || fake.format != fake.posted_format) {
        free(fake.window_pixels);
        fake.window_pixels = calloc(row, (size_t)fake.height);
        if (fake.window_pixels == NULL) {
            fprintf(stderr, "fake buffers: no memory for window %llu's contents\n", (unsigned long long)id);
            exit(2);
        }
        fake.window_id = id;
    }
    for (int32_t y = fake.dirty[1]; y < fake.dirty[3]; ++y) {
        memcpy(fake.window_pixels + (size_t)y * row + (size_t)fake.dirty[0] * 4,
               (const uint8_t *)fake.pixels + ((size_t)y * (size_t)fake.stride + (size_t)fake.dirty[0]) * 4,
               (size_t)(fake.dirty[2] - fake.dirty[0]) * 4);
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/post-%d.rgba", fake.dump, ++fake.posts);
    FILE *file = fopen(path, "wb");
    if (file == NULL || fwrite(fake.window_pixels, row, (size_t)fake.height, file) != (size_t)fake.height) {
        fprintf(stderr, "fake buffers: cannot write %s\n", path);
        exit(2);
    }
    fclose(file);
    snprintf(path, sizeof path, "%s/posts.txt", fake.dump);
    file = fopen(path, fake.posts == 1 ? "w" : "a");
    if (file == NULL) {
        fprintf(stderr, "fake buffers: cannot write %s\n", path);
        exit(2);
    }
    fprintf(file, "%d window %llu %dx%d format %d dirty %d %d %d %d\n", fake.posts, (unsigned long long)id,
            fake.width, fake.height, fake.format, fake.dirty[0], fake.dirty[1], fake.dirty[2], fake.dirty[3]);
    fclose(file);
}

static int fake_post(void *context, uint64_t id) {
    (void)context;
    if (fake.format == 4) {
        fprintf(stderr, "fake buffers: post window %llu, first pixel 0x%04x\n", (unsigned long long)id,
                ((const uint16_t *)fake.pixels)[0]);
    } else {
        fprintf(stderr, "fake buffers: post window %llu, first pixel 0x%08x\n", (unsigned long long)id,
                ((const uint32_t *)fake.pixels)[0]);
    }
    if (fake.dump != NULL) {
        dump_post(id);
    }
    fake.posted_id = id;
    fake.posted_width = fake.width;
    fake.posted_height = fake.height;
    fake.posted_format = fake.format;
    free(fake.pixels);
    fake.pixels = NULL;
    return 0;
}

static void fake_removed(void *context, uint64_t id) {
    (void)context;
    fprintf(stderr, "fake buffers: window %llu removed\n", (unsigned long long)id);
}

static const struct apkrt_window_buffers fake_buffers = {fake_lock, fake_post, fake_removed, NULL, NULL};

void minato_test_use_fake_buffers(void) {
    const char *buffers = getenv("MINATO_WINDOW_BUFFERS");
    fake.dump = getenv("MINATO_WINDOW_DUMP");
    if (buffers == NULL && fake.dump != NULL) {
        fprintf(stderr, "window host: MINATO_WINDOW_DUMP needs MINATO_WINDOW_BUFFERS=fake\n");
        exit(2);
    }
    if (buffers == NULL) {
        return;
    }
    if (strcmp(buffers, "fake") != 0 || (getenv("MINATO_WINDOW") == NULL && fake.dump == NULL) ||
        apkrt_set_window_buffers(&fake_buffers) != 0) {
        fprintf(stderr, "window host: MINATO_WINDOW_BUFFERS=%s is not fake without MINATO_WINDOW or "
                        "MINATO_WINDOW_DUMP, or the runtime refused it\n", buffers);
        exit(2);
    }
}

__attribute__((constructor)) static void add_window(void) {
    const char *size = getenv("MINATO_WINDOW");
    int width = 0, height = 0;
    if (size == NULL) {
        return;
    }
    if (sscanf(size, "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
        fprintf(stderr, "window host: MINATO_WINDOW=%s is not <width>x<height>\n", size);
        exit(2);
    }
    @autoreleasepool {
        CAMetalLayer *layer = [MinatoMetalLayer layer];
        layer.contentsScale = 1;
        layer.bounds = CGRectMake(0, 0, width, height);
        const char *opaque = getenv("MINATO_WINDOW_OPAQUE");
        layer.opaque = opaque != NULL && strcmp(opaque, "1") == 0;
        minato_test_watch_hidden(layer);
        minato_test_watch_contents(layer);
        const char *record = getenv("MINATO_WINDOW_RECORD");
        unsigned long capacity = 0;
        if (record != NULL && sscanf(record, "%lu:%lu", &capacity, &recorded_presents) == 2 &&
            [(MinatoMetalLayer *)layer startRecording:capacity]) {
            recorded = (MinatoMetalLayer *)layer;
            pthread_t thread;
            pthread_create(&thread, NULL, check_recording, NULL);
            pthread_detach(thread);
        }
        minato_test_window = apkrt_window_add((__bridge void *)layer, width, height);
    }
    if (minato_test_window == 0 || apkrt_window_set_gate(minato_test_window, 1) != 0) {
        fprintf(stderr, "window host: the runtime refused the window (%llu)\n", (unsigned long long)minato_test_window);
        exit(2);
    }
    if (getenv("MINATO_WINDOW_BUFFERS") != NULL) {
        apkrt_window_set_gate(minato_test_window, 0);  // until 200 ms after the fake's first lock
    }
    const char *ticks = getenv("MINATO_WINDOW_TICKS");
    if (ticks != NULL) {
        if (sscanf(ticks, "%d:%lu", &tick_hz, &tick_presents) != 2 || tick_hz <= 0) {
            fprintf(stderr, "window host: MINATO_WINDOW_TICKS=%s is not <hz>:<presents>\n", ticks);
            exit(2);
        }
        pthread_t thread;
        pthread_create(&thread, NULL, tick, NULL);
        pthread_detach(thread);
    }
    const char *script = getenv("MINATO_WINDOW_SCRIPT");
    if (script != NULL) {
        pthread_t thread;
        void *(*play)(void *) = background;
        const BOOL trims = sscanf(script, "trim:%lu", &presents) == 1;
        if (strcmp(script, "glue") == 0 || trims) {
            const struct apkrt_session session = {
                .end = ended,
                .configuration = {.scale = 1, .width = width, .height = height, .orientation = 2, .night = 1,
                                  .refresh_rate = 60, .language = "en", .country = "US"}};
            if (apkrt_session_begin(&session) != 0) {
                fprintf(stderr, "window host: the runtime refused the session\n");
                exit(2);
            }
            play = trims ? trim : glue;
        } else if (sscanf(script, "background:%lu:%d", &presents, &hold_ms) < 1) {
            fprintf(stderr,
                    "window host: MINATO_WINDOW_SCRIPT=%s is neither background:<presents>, trim:<presents> nor glue\n",
                    script);
            exit(2);
        }
        pthread_create(&thread, NULL, play, NULL);
        pthread_detach(thread);
    }
}
