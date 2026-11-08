/* libEGL's EGL_ANDROID_get_frame_timestamps, which Android's libEGL implements itself over the window's frame
   history (opengl/libs/EGL/egl_platform_entries.cpp at android15-release) and ANGLE's Metal display lacks; a frame
   pacer uses it once eglGetProcAddress finds its functions (engine-start-test). As Android's:
   - eglSurfaceAttrib(EGL_TIMESTAMPS_ANDROID) turns the history on or off for a window surface (EGL_BAD_SURFACE for
     any other surface); every other attribute is ANGLE's;
   - eglGetNextFrameIdANDROID answers the ID the next swap's frame gets, from 1, whether or not the history is on;
   - eglGetFrameTimestampsANDROID answers a frame's timestamps with the history on (EGL_BAD_SURFACE while it is
     off), EGL_BAD_ACCESS for a frame not yet swapped or no longer among the last 8, EGL_BAD_PARAMETER for an
     unknown name;
   - every timestamp but the display's present is supported (the window has no present fences, as the emulator's
     has none), and the three compositor timings.
   The values are what the guest knows: the requested present is the frame's eglPresentationTimeANDROID or else its
   swap's time (Android's automatic timestamp); rendering complete is pending until a fence made at the swap is
   signaled, then the time it was first seen signaled, at a later swap or a query; the compositor's own times, the
   dequeue ready and reads done times are EGL_TIMESTAMP_INVALID_ANDROID, which Android answers for a time a frame
   will not have. The compositor
   timings come from the display's vsync channel: the interval is its period, the deadline its next tick and the
   latency 2 periods, as graphics/host presents (ADR-0041 decision 11).
   Errors these functions leave are their thread's own, answered by eglGetError before ANGLE's. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "libegl.h"

MINATO_HOST(eglSurfaceAttrib);
MINATO_HOST(eglGetError);

#define HISTORY 8

struct frame {
    EGLuint64KHR id;    // 0 for none
    int64_t requested;  // the requested present
    EGLSyncKHR sync;    // the swap's fence until it is seen signaled
    int64_t complete;   // rendering complete, 0 while pending
};

struct history {
    struct history *next;
    EGLDisplay display;
    EGLSurface surface;
    int enabled;
    EGLuint64KHR next_id;  // the next swap's frame ID, from 1
    int64_t presentation;  // eglPresentationTimeANDROID's time for the next frame, 0 for none
    struct frame frames[HISTORY];
};
static struct history *histories;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static __thread EGLint own_error;  // this file's error, eglGetError's first answer; 0 for none

static int64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static EGLBoolean fail(EGLint error) {
    own_error = error;
    return EGL_FALSE;
}

static EGLBoolean succeed(void) {
    own_error = EGL_SUCCESS;
    return EGL_TRUE;
}

EGLint eglGetError(void) {
    const EGLint own = own_error;
    if (own != 0) {
        own_error = 0;
        minato_host_eglGetError();  // ANGLE's goes too, as one call answers it
        return own;
    }
    return minato_host_eglGetError();
}

/* The history of a window surface, made at its first use; with the lock held. NULL for any other surface. */
static struct history *find(EGLDisplay display, EGLSurface surface) {
    for (struct history *entry = histories; entry != NULL; entry = entry->next) {
        if (entry->display == display && entry->surface == surface) {
            return entry;
        }
    }
    if (!minato_window_surface(display, surface)) {
        return NULL;
    }
    struct history *made = calloc(1, sizeof *made);
    if (made != NULL) {
        made->display = display;
        made->surface = surface;
        made->next_id = 1;
        made->next = histories;
        histories = made;
    }
    return made;
}

void minato_timestamps_presentation(EGLDisplay display, EGLSurface surface, int64_t time) {
    pthread_mutex_lock(&lock);
    struct history *history = find(display, surface);
    if (history != NULL) {
        history->presentation = time;
    }
    pthread_mutex_unlock(&lock);
}

EGLSyncKHR minato_timestamps_before_swap(EGLDisplay display, EGLSurface surface) {
    pthread_mutex_lock(&lock);
    struct history *history = find(display, surface);
    const int enabled = history != NULL && history->enabled;
    pthread_mutex_unlock(&lock);
    return enabled ? eglCreateSyncKHR(display, EGL_SYNC_FENCE_KHR, NULL) : EGL_NO_SYNC_KHR;
}

/* Rendering complete for a frame whose fence is now signaled: the time it is first seen so. */
static void settle(EGLDisplay display, struct frame *frame) {
    EGLint status = 0;
    if (frame->complete == 0 && frame->sync != EGL_NO_SYNC_KHR &&
        eglGetSyncAttribKHR(display, frame->sync, EGL_SYNC_STATUS_KHR, &status) && status == EGL_SIGNALED_KHR) {
        frame->complete = now_ns();
        eglDestroySyncKHR(display, frame->sync);
        frame->sync = EGL_NO_SYNC_KHR;
    }
}

void minato_timestamps_swapped(EGLDisplay display, EGLSurface surface, EGLSyncKHR sync, EGLBoolean swapped) {
    EGLSyncKHR dropped = sync;
    pthread_mutex_lock(&lock);
    struct history *history = find(display, surface);
    for (int index = 0; history != NULL && index < HISTORY; index++) {
        settle(display, &history->frames[index]);
    }
    if (history != NULL && swapped) {
        const EGLuint64KHR id = history->next_id++;
        struct frame *frame = &history->frames[id % HISTORY];
        if (frame->sync != EGL_NO_SYNC_KHR) {
            eglDestroySyncKHR(display, frame->sync);
        }
        *frame = (struct frame){id, history->presentation != 0 ? history->presentation : now_ns(),
                                history->enabled ? sync : EGL_NO_SYNC_KHR, 0};
        if (frame->sync != EGL_NO_SYNC_KHR) {
            dropped = EGL_NO_SYNC_KHR;
        }
        history->presentation = 0;
    }
    pthread_mutex_unlock(&lock);
    if (dropped != EGL_NO_SYNC_KHR) {
        eglDestroySyncKHR(display, dropped);
    }
}

void minato_timestamps_destroyed(EGLDisplay display, EGLSurface surface) {
    pthread_mutex_lock(&lock);
    for (struct history **entry = &histories; *entry != NULL;) {
        struct history *history = *entry;
        if (history->display == display && (surface == EGL_NO_SURFACE || history->surface == surface)) {
            *entry = history->next;
            for (int index = 0; index < HISTORY; index++) {
                if (history->frames[index].sync != EGL_NO_SYNC_KHR) {
                    eglDestroySyncKHR(display, history->frames[index].sync);
                }
            }
            free(history);
        } else {
            entry = &history->next;
        }
    }
    pthread_mutex_unlock(&lock);
}

EGLBoolean eglSurfaceAttrib(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint value) {
    if (attribute != EGL_TIMESTAMPS_ANDROID) {
        own_error = 0;
        return minato_host_eglSurfaceAttrib(display, surface, attribute, value);
    }
    pthread_mutex_lock(&lock);
    struct history *history = find(display, surface);
    if (history != NULL) {
        history->enabled = value != 0;
    }
    pthread_mutex_unlock(&lock);
    return history != NULL ? succeed() : fail(EGL_BAD_SURFACE);
}

EGLBoolean eglGetNextFrameIdANDROID(EGLDisplay display, EGLSurface surface, EGLuint64KHR *frameId) {
    pthread_mutex_lock(&lock);
    struct history *history = find(display, surface);
    if (history != NULL && frameId != NULL) {
        *frameId = history->next_id;
    }
    pthread_mutex_unlock(&lock);
    return history != NULL ? succeed() : fail(EGL_BAD_SURFACE);
}

static int timestamp_known(EGLint name) {
    switch (name) {
    case EGL_REQUESTED_PRESENT_TIME_ANDROID:
    case EGL_RENDERING_COMPLETE_TIME_ANDROID:
    case EGL_COMPOSITION_LATCH_TIME_ANDROID:
    case EGL_FIRST_COMPOSITION_START_TIME_ANDROID:
    case EGL_LAST_COMPOSITION_START_TIME_ANDROID:
    case EGL_FIRST_COMPOSITION_GPU_FINISHED_TIME_ANDROID:
    case EGL_DISPLAY_PRESENT_TIME_ANDROID:
    case EGL_DEQUEUE_READY_TIME_ANDROID:
    case EGL_READS_DONE_TIME_ANDROID:
        return 1;
    default:
        return 0;
    }
}

EGLBoolean eglGetFrameTimestampsANDROID(EGLDisplay display, EGLSurface surface, EGLuint64KHR frameId,
                                        EGLint numTimestamps, const EGLint *timestamps, EGLnsecsANDROID *values) {
    for (EGLint index = 0; index < numTimestamps; index++) {
        if (!timestamp_known(timestamps[index])) {
            return fail(EGL_BAD_PARAMETER);
        }
    }
    pthread_mutex_lock(&lock);
    struct history *history = find(display, surface);
    EGLint error = EGL_SUCCESS;
    struct frame *frame = NULL;
    if (history == NULL || !history->enabled) {
        error = EGL_BAD_SURFACE;
    } else if (frameId == 0 || frameId >= history->next_id ||
               history->frames[frameId % HISTORY].id != frameId) {
        error = EGL_BAD_ACCESS;
    } else {
        frame = &history->frames[frameId % HISTORY];
        settle(display, frame);
        for (EGLint index = 0; index < numTimestamps; index++) {
            switch (timestamps[index]) {
            case EGL_REQUESTED_PRESENT_TIME_ANDROID:
                values[index] = frame->requested;
                break;
            case EGL_RENDERING_COMPLETE_TIME_ANDROID:
                values[index] = frame->complete != 0                ? frame->complete
                                : frame->sync != EGL_NO_SYNC_KHR ? EGL_TIMESTAMP_PENDING_ANDROID
                                                                    : EGL_TIMESTAMP_INVALID_ANDROID;
                break;
            default:
                values[index] = EGL_TIMESTAMP_INVALID_ANDROID;
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return error == EGL_SUCCESS ? succeed() : fail(error);
}

EGLBoolean eglGetFrameTimestampSupportedANDROID(EGLDisplay display, EGLSurface surface, EGLint timestamp) {
    pthread_mutex_lock(&lock);
    const int window = find(display, surface) != NULL;
    pthread_mutex_unlock(&lock);
    if (!window) {
        return fail(EGL_BAD_SURFACE);
    }
    own_error = EGL_SUCCESS;
    switch (timestamp) {
    case EGL_COMPOSITE_DEADLINE_ANDROID:
    case EGL_COMPOSITE_INTERVAL_ANDROID:
    case EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID:
        return EGL_TRUE;
    case EGL_DISPLAY_PRESENT_TIME_ANDROID:
        return EGL_FALSE;
    default:
        return timestamp_known(timestamp) ? EGL_TRUE : EGL_FALSE;
    }
}

EGLBoolean eglGetCompositorTimingSupportedANDROID(EGLDisplay display, EGLSurface surface, EGLint name) {
    pthread_mutex_lock(&lock);
    const int window = find(display, surface) != NULL;
    pthread_mutex_unlock(&lock);
    if (!window) {
        return fail(EGL_BAD_SURFACE);
    }
    own_error = EGL_SUCCESS;
    return name == EGL_COMPOSITE_DEADLINE_ANDROID || name == EGL_COMPOSITE_INTERVAL_ANDROID ||
           name == EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID;
}

/* The display's newest vsync tick and period, from a channel of libEGL's own made at the first ask; 0 without one. */
static long channel = -1;
static pthread_once_t channel_once = PTHREAD_ONCE_INIT;

static void connect_channel(void) {
    channel = syscall(APKRT_NR_VSYNC_CONNECT);
    if (channel >= 0) {
        syscall(APKRT_NR_VSYNC_REQUEST, channel);
    }
}

static int vsync(struct apkrt_vsync *tick) {
    pthread_once(&channel_once, connect_channel);
    return channel >= 0 && syscall(APKRT_NR_VSYNC_READ, channel, tick) >= 0 && tick->period > 0;
}

EGLBoolean eglGetCompositorTimingANDROID(EGLDisplay display, EGLSurface surface, EGLint numTimestamps,
                                         const EGLint *names, EGLnsecsANDROID *values) {
    for (EGLint index = 0; index < numTimestamps; index++) {
        if (names[index] != EGL_COMPOSITE_DEADLINE_ANDROID && names[index] != EGL_COMPOSITE_INTERVAL_ANDROID &&
            names[index] != EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID) {
            return fail(EGL_BAD_PARAMETER);
        }
    }
    pthread_mutex_lock(&lock);
    const int window = find(display, surface) != NULL;
    pthread_mutex_unlock(&lock);
    if (!window) {
        return fail(EGL_BAD_SURFACE);
    }
    struct apkrt_vsync tick = {0};
    if (!vsync(&tick)) {
        return fail(EGL_BAD_SURFACE);
    }
    const int64_t now = now_ns();
    int64_t deadline = tick.time > 0 ? tick.time + tick.period : now;
    while (deadline <= now) {
        deadline += tick.period;
    }
    for (EGLint index = 0; index < numTimestamps; index++) {
        values[index] = names[index] == EGL_COMPOSITE_DEADLINE_ANDROID   ? deadline
                        : names[index] == EGL_COMPOSITE_INTERVAL_ANDROID ? tick.period
                                                                          : 2 * tick.period;
    }
    return succeed();
}
