/* For test 6 of ADR-0030 in graphics-run: the host's window as the app gives it (ADR-0028, decision 6).
   - With MINATO_WINDOW=<width>x<height>, before the program starts, it adds a window of that many pixels on
     a CAMetalLayer of as many points at scale 1 and opens its gate: the program's first window, ID 1.
   - With MINATO_WINDOW_SCRIPT=background:<n> too, a thread of its own waits until the window has <n>
     presents and then keeps the app's order at background (ADR-0028, decision 6): the background flag,
     "scheduled" with 1 s to go, the window's removal; 100 ms later the foreground. With background:<n>:<ms>, it
     stays <ms> in the background between "scheduled" and the removal, which the count of guest commits covers. It prints each step to
     stderr, how many command buffers other threads than the guest's committed during "scheduled" (one per
     queue: graphics/host's), and how many guest threads committed between "scheduled" and the foreground,
     which must be none (the Metal hook counts both, test_host.h).
   - With MINATO_WINDOW_SCRIPT=glue instead (test 7 of ADR-0029), it begins a session before the program
     starts, whose end ends the process with the program's status, and a thread of its own plays the app for
     a program that plays activity-host: the events of a start with the window, until the window has 10
     presents; the focus lost, until the app stops drawing (the NDK sample stops at APP_CMD_LOST_FOCUS); the
     gate closed; the order at background of ADR-0029's decision 15 (the background flag, "scheduled", the
     window's removal, only then onPause, the window's destruction, the saved state and onStop, each answer
     within 500 ms of its event); the foreground and onDestroy. It prints each step to stderr, and the command
     buffers guest threads committed from "scheduled" to the foreground, which must be none.
   - With MINATO_WINDOW_BUFFERS=fake too (ADR-0043 b1, graphics.window_lock), graphics_run.c puts a fake in the place
     of graphics/host's buffers (minato_test_use_fake_buffers), one that
     prints to stderr what reaches it: lock takes RGBA_8888, RGBX_8888 and RGB_565 and refuses other formats with
     -EINVAL; its buffer starts cleared, with the width rounded up to 16 pixels as its stride; the dirty rectangle it
     answers is the whole buffer for another geometry than the last posted one's, else the app's within the buffer.
     post prints the buffer's first pixel. The window's gate stays closed until 200 ms after the first lock, so that
     the first post waits at it. */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
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

/* The fake buffers of MINATO_WINDOW_BUFFERS=fake, on the program's one thread. */
static struct {
    int locks;
    int32_t width, height, format;  // the locked buffer's
    int32_t posted_width, posted_height, posted_format;
    void *pixels;
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
    if (fake.locks++ == 0) {
        pthread_t thread;
        pthread_create(&thread, NULL, open_gate_later, NULL);
        pthread_detach(thread);
    }
    const int32_t stride = (buffer->width + 15) / 16 * 16;
    fake.pixels = calloc((size_t)stride * (size_t)buffer->height, (size_t)bytes);
    buffer->stride = stride;
    buffer->bits = fake.pixels;
    int32_t *const dirty = buffer->dirty;
    if (buffer->width != fake.posted_width || buffer->height != fake.posted_height ||
        buffer->format != fake.posted_format) {
        dirty[0] = dirty[1] = 0;
        dirty[2] = buffer->width;
        dirty[3] = buffer->height;
    } else {
        dirty[0] = dirty[0] < 0 ? 0 : dirty[0];
        dirty[1] = dirty[1] < 0 ? 0 : dirty[1];
        dirty[2] = dirty[2] > buffer->width ? buffer->width : dirty[2];
        dirty[3] = dirty[3] > buffer->height ? buffer->height : dirty[3];
    }
    fake.width = buffer->width;
    fake.height = buffer->height;
    fake.format = buffer->format;
    return 0;
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

static const struct apkrt_window_buffers fake_buffers = {fake_lock, fake_post, fake_removed, NULL};

void minato_test_use_fake_buffers(void) {
    const char *buffers = getenv("MINATO_WINDOW_BUFFERS");
    if (buffers == NULL) {
        return;
    }
    if (strcmp(buffers, "fake") != 0 || getenv("MINATO_WINDOW") == NULL ||
        apkrt_set_window_buffers(&fake_buffers) != 0) {
        fprintf(stderr, "window host: MINATO_WINDOW_BUFFERS=%s is not fake without MINATO_WINDOW, or the runtime "
                        "refused it\n", buffers);
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
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.contentsScale = 1;
        layer.bounds = CGRectMake(0, 0, width, height);
        minato_test_window = apkrt_window_add((__bridge void *)layer, width, height);
    }
    if (minato_test_window == 0 || apkrt_window_set_gate(minato_test_window, 1) != 0) {
        fprintf(stderr, "window host: the runtime refused the window (%llu)\n", (unsigned long long)minato_test_window);
        exit(2);
    }
    if (getenv("MINATO_WINDOW_BUFFERS") != NULL) {
        apkrt_window_set_gate(minato_test_window, 0);  // until 200 ms after the fake's first lock
    }
    const char *script = getenv("MINATO_WINDOW_SCRIPT");
    if (script != NULL) {
        pthread_t thread;
        void *(*play)(void *) = background;
        if (strcmp(script, "glue") == 0) {
            const struct apkrt_session session = {
                .end = ended,
                .configuration = {.scale = 1, .width = width, .height = height, .orientation = 2, .night = 1,
                                  .refresh_rate = 60, .language = "en", .country = "US"}};
            if (apkrt_session_begin(&session) != 0) {
                fprintf(stderr, "window host: the runtime refused the session\n");
                exit(2);
            }
            play = glue;
        } else if (sscanf(script, "background:%lu:%d", &presents, &hold_ms) < 1) {
            fprintf(stderr, "window host: MINATO_WINDOW_SCRIPT=%s is neither background:<presents> nor glue\n", script);
            exit(2);
        }
        pthread_create(&thread, NULL, play, NULL);
        pthread_detach(thread);
    }
}
