// input-test (ADR-0036, step b): a NativeActivity on native_app_glue that takes its input queue's events
// itself. Its window shows one frame drawn by the CPU, without which Android finds no window to touch. Once it
// has the focus it logs "ready" and sets AWINDOW_FLAG_KEEP_SCREEN_ON, the Mac host's cue; then
// a script plays the four gestures of gestures.txt (tools/android-reference.sh through the emulator's console,
// runtime.input through apkrt_input_post). With the tag "input-test" it logs what Android 15 fixes: each event
// but a MOVE after a MOVE with the same pointers, with its action, the pointer's index, the pointer count and
// the IDs; per gesture, that its events are a touchscreen's fingers of one device, the down time, the event
// times and their history, the pressure, the raw places and axis values, and what its places show; that the
// history's size as an index gives an event's own sample; and the queue's answers to getEvent, hasEvents and
// preDispatchEvent. What differs by device goes to the tag
// "input-test-device": the device's values, the places in pixels, and the MOVEs and their history, which
// Android batches by frame. After the fourth gesture it calls ANativeActivity_finish; a test that stalls ends
// 30 s after "ready".
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "input-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "input-test-device", __VA_ARGS__)

static const int64_t kMillisecond = 1000000, kSecond = 1000000000;

// The gestures in the order gestures.txt plays them.
enum { kTap, kSwipe, kTwoFingers, kLowestFreeId, kGestures };
static const char *const kNames[kGestures] = {"tap", "swipe", "two fingers", "lowest free ID"};
static const char *const kPlaces[kGestures] = {
    "at a quarter of the width and the height, the UP at the DOWN's place",
    "x never falls, y stays, the UP right of the DOWN",
    "the second finger right of the first",
    "the third finger with ID 0 at index 0, right of the second",
};

// What one gesture showed.
struct gesture {
    int64_t down_time;       // the DOWN's event time
    int64_t last_time;       // the last event's
    float start_x, start_y;  // the DOWN's place
    float last_x;            // the swipe's x at its last sample
    bool device;             // a touchscreen's fingers, of the first event's device
    bool times;              // the down time the DOWN's, event times never back and on CLOCK_MONOTONIC
    bool history;            // historical times never back and not after each event's time
    bool pressure;           // above 0 and at most 1
    bool raw;                // raw places and axis values the same as x and y
    bool place;              // what the gesture's places show (kPlaces)
    int events, moves, with_history, historical;
};

struct state {
    struct android_app *app;
    bool drawn;
    bool ready;
    bool done;
    int64_t deadline;  // CLOCK_MONOTONIC in ns: the end of a stalled test, 0 while none is due
    bool have_device;
    int32_t device;    // the first event's
    int index;         // the gesture under way or the next, kGestures once all ended
    bool in_gesture;
    struct gesture gesture;
    char last_move[64];  // the IDs of the MOVE logged last, "" after any other event
    bool history_shown;  // the samples of the swipe's first MOVE with history
    bool own_sample;     // the history's size as the index gives each event's own time and place
    int events;
    // The queue's answers: getEvent 0 for each event, then -11 and NULL; hasEvents 1 at each wake;
    // preDispatchEvent 0.
    bool got, empty, has_events, pre_dispatch;
};

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * kSecond + time.tv_nsec;
}

static const char *yes(bool value) {
    return value ? "yes" : "no";
}

static void finish(struct state *state) {
    state->done = true;
    LOG("ANativeActivity_finish");
    ANativeActivity_finish(state->app->activity);
}

static void describe_action(int32_t action, char *text, size_t size) {
    const int index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    switch (action & AMOTION_EVENT_ACTION_MASK) {
        case AMOTION_EVENT_ACTION_DOWN: snprintf(text, size, "DOWN"); break;
        case AMOTION_EVENT_ACTION_UP: snprintf(text, size, "UP"); break;
        case AMOTION_EVENT_ACTION_MOVE: snprintf(text, size, "MOVE"); break;
        case AMOTION_EVENT_ACTION_CANCEL: snprintf(text, size, "CANCEL"); break;
        case AMOTION_EVENT_ACTION_POINTER_DOWN: snprintf(text, size, "POINTER_DOWN(%d)", index); break;
        case AMOTION_EVENT_ACTION_POINTER_UP: snprintf(text, size, "POINTER_UP(%d)", index); break;
        default: snprintf(text, size, "action %d", action); break;
    }
}

static void describe_ids(const AInputEvent *event, char *text, size_t size) {
    size_t used = 0;
    text[0] = '\0';
    const size_t count = AMotionEvent_getPointerCount(event);
    for (size_t index = 0; index < count && used < size; ++index) {
        const int written =
            snprintf(text + used, size - used, index == 0 ? "%d" : " %d", AMotionEvent_getPointerId(event, index));
        if (written < 0) break;
        used += (size_t)written;
    }
}

// Whether `value` lies within 1 % of `size` from the fraction `fraction` of it.
static bool near(float value, float fraction, int32_t size) {
    return fabsf(value - fraction * (float)size) <= (float)size / 100;
}

static void log_device_values(const AInputEvent *event) {
    DEVICE("device %d, source 0x%x, flags 0x%x, meta 0x%x, buttons 0x%x, edge 0x%x, classification %d, action button 0x%x",
           AInputEvent_getDeviceId(event), (unsigned)AInputEvent_getSource(event), (unsigned)AMotionEvent_getFlags(event),
           (unsigned)AMotionEvent_getMetaState(event), (unsigned)AMotionEvent_getButtonState(event),
           (unsigned)AMotionEvent_getEdgeFlags(event), AMotionEvent_getClassification(event),
           (unsigned)AMotionEvent_getActionButton(event));
    DEVICE("precision %.1f, %.1f, offset %.1f, %.1f", AMotionEvent_getXPrecision(event),
           AMotionEvent_getYPrecision(event), AMotionEvent_getXOffset(event), AMotionEvent_getYOffset(event));
    DEVICE("pressure %.3f, size %.3f, touch %.1f, %.1f, tool %.1f, %.1f, orientation %.3f",
           AMotionEvent_getPressure(event, 0), AMotionEvent_getSize(event, 0), AMotionEvent_getTouchMajor(event, 0),
           AMotionEvent_getTouchMinor(event, 0), AMotionEvent_getToolMajor(event, 0),
           AMotionEvent_getToolMinor(event, 0), AMotionEvent_getOrientation(event, 0));
}

static void start_gesture(struct state *state, const AInputEvent *event) {
    if (!state->have_device) {
        state->have_device = true;
        state->device = AInputEvent_getDeviceId(event);
        log_device_values(event);
    }
    struct gesture *gesture = &state->gesture;
    memset(gesture, 0, sizeof *gesture);
    gesture->device = gesture->times = gesture->history = gesture->pressure = gesture->raw = true;
    gesture->place = state->index == kSwipe;
    gesture->down_time = gesture->last_time = AMotionEvent_getEventTime(event);
    gesture->start_x = gesture->last_x = AMotionEvent_getX(event, 0);
    gesture->start_y = AMotionEvent_getY(event, 0);
    state->in_gesture = true;
    DEVICE("%s: DOWN at %.1f, %.1f", kNames[state->index], gesture->start_x, gesture->start_y);
}

// What every event of a gesture shows.
static void check_event(struct state *state, const AInputEvent *event, int32_t masked, size_t count) {
    struct gesture *gesture = &state->gesture;
    ++gesture->events;
    bool fingers = true;
    for (size_t index = 0; index < count; ++index) {
        fingers = fingers && AMotionEvent_getToolType(event, index) == AMOTION_EVENT_TOOL_TYPE_FINGER;
    }
    gesture->device = gesture->device && fingers && AInputEvent_getDeviceId(event) == state->device &&
                      (AInputEvent_getSource(event) & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN;
    const int64_t time = AMotionEvent_getEventTime(event), at = now();
    gesture->times = gesture->times && AMotionEvent_getDownTime(event) == gesture->down_time &&
                     time >= gesture->last_time && time <= at && at - time < kSecond;
    gesture->last_time = time;
    const size_t history = AMotionEvent_getHistorySize(event);
    state->own_sample = state->own_sample && AMotionEvent_getHistoricalEventTime(event, history) == time &&
                        AMotionEvent_getHistoricalX(event, 0, history) == AMotionEvent_getX(event, 0) &&
                        AMotionEvent_getHistoricalY(event, 0, history) == AMotionEvent_getY(event, 0);
    if (masked == AMOTION_EVENT_ACTION_MOVE) {
        ++gesture->moves;
        gesture->with_history += history > 0;
        gesture->historical += (int)history;
    }
    // Never back rather than rising: the emulator's touchscreen stamps the reports of one burst in microseconds,
    // and two of them often get the same stamp.
    for (size_t sample = 0; sample < history; ++sample) {
        const int64_t later = sample + 1 < history ? AMotionEvent_getHistoricalEventTime(event, sample + 1) : time;
        gesture->history = gesture->history && AMotionEvent_getHistoricalEventTime(event, sample) <= later;
    }
    for (size_t index = 0; index < count; ++index) {
        const float x = AMotionEvent_getX(event, index), y = AMotionEvent_getY(event, index);
        const float pressure = AMotionEvent_getPressure(event, index);
        gesture->pressure = gesture->pressure && pressure > 0 && pressure <= 1;
        gesture->raw = gesture->raw && AMotionEvent_getRawX(event, index) == x &&
                       AMotionEvent_getRawY(event, index) == y &&
                       AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, index) == x &&
                       AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, index) == y;
    }
}

// What the places of each gesture show (kPlaces).
static void check_places(struct state *state, const AInputEvent *event, int32_t masked, size_t pointer, size_t count) {
    struct gesture *gesture = &state->gesture;
    switch (state->index) {
        case kTap:
            if (masked == AMOTION_EVENT_ACTION_DOWN) {
                gesture->place = near(gesture->start_x, 0.25f, ANativeWindow_getWidth(state->app->window)) &&
                                 near(gesture->start_y, 0.25f, ANativeWindow_getHeight(state->app->window));
            } else {
                gesture->place = gesture->place && AMotionEvent_getX(event, 0) == gesture->start_x &&
                                 AMotionEvent_getY(event, 0) == gesture->start_y;
            }
            break;
        case kSwipe: {
            const size_t history = AMotionEvent_getHistorySize(event);
            for (size_t sample = 0; sample <= history; ++sample) {
                const float x = sample < history ? AMotionEvent_getHistoricalX(event, 0, sample) : AMotionEvent_getX(event, 0);
                const float y = sample < history ? AMotionEvent_getHistoricalY(event, 0, sample) : AMotionEvent_getY(event, 0);
                gesture->place = gesture->place && x >= gesture->last_x && fabsf(y - gesture->start_y) < 0.5f;
                gesture->last_x = x;
            }
            if (masked == AMOTION_EVENT_ACTION_UP) gesture->place = gesture->place && gesture->last_x > gesture->start_x;
            break;
        }
        case kTwoFingers:
            if (masked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                gesture->place = count == 2 && AMotionEvent_getX(event, 1) > AMotionEvent_getX(event, 0);
            }
            break;
        case kLowestFreeId:
            if (masked == AMOTION_EVENT_ACTION_POINTER_DOWN && pointer == 0) {
                gesture->place = count == 2 && AMotionEvent_getPointerId(event, 0) == 0 &&
                                 AMotionEvent_getX(event, 0) > AMotionEvent_getX(event, 1);
            }
            break;
    }
}

// The samples of the swipe's first MOVE with history, oldest first, for the device: each one's place, how long
// before the event it came, its touch major and its pressure; then whether the history's other values agree:
// raw places as the places, axis values as the values, touch minor and tool sizes as the touch major, size and
// orientation 0.
static void show_history(struct state *state, const AInputEvent *event) {
    const size_t history = AMotionEvent_getHistorySize(event);
    if (state->index != kSwipe || history == 0 || state->history_shown) return;
    state->history_shown = true;
    const int64_t time = AMotionEvent_getEventTime(event);
    bool others = true;
    for (size_t sample = 0; sample <= history; ++sample) {
        const bool own = sample == history;
        const float x = own ? AMotionEvent_getX(event, 0) : AMotionEvent_getHistoricalX(event, 0, sample);
        const float y = own ? AMotionEvent_getY(event, 0) : AMotionEvent_getHistoricalY(event, 0, sample);
        const float major = own ? AMotionEvent_getTouchMajor(event, 0) : AMotionEvent_getHistoricalTouchMajor(event, 0, sample);
        const float pressure = own ? AMotionEvent_getPressure(event, 0) : AMotionEvent_getHistoricalPressure(event, 0, sample);
        const int64_t at = own ? time : AMotionEvent_getHistoricalEventTime(event, sample);
        DEVICE("swipe: the first MOVE with history, sample %zu: x %.1f, %.1f ms before the event, touch major %.1f, "
               "pressure %.2f",
               sample, x, (double)(time - at) / (double)kMillisecond, major, pressure);
        if (own) continue;
        others = others && AMotionEvent_getHistoricalRawX(event, 0, sample) == x &&
                 AMotionEvent_getHistoricalRawY(event, 0, sample) == y &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_X, 0, sample) == x &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_Y, 0, sample) == y &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_PRESSURE, 0, sample) == pressure &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_TOUCH_MAJOR, 0, sample) == major &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_TOUCH_MINOR, 0, sample) == major &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_TOOL_MAJOR, 0, sample) == major &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_TOOL_MINOR, 0, sample) == major &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_SIZE, 0, sample) == 0 &&
                 AMotionEvent_getHistoricalAxisValue(event, AMOTION_EVENT_AXIS_ORIENTATION, 0, sample) == 0 &&
                 AMotionEvent_getHistoricalTouchMinor(event, 0, sample) == major &&
                 AMotionEvent_getHistoricalToolMajor(event, 0, sample) == major &&
                 AMotionEvent_getHistoricalToolMinor(event, 0, sample) == major &&
                 AMotionEvent_getHistoricalSize(event, 0, sample) == 0 &&
                 AMotionEvent_getHistoricalOrientation(event, 0, sample) == 0;
    }
    DEVICE("swipe: the first MOVE with history: raw places as the places, axis values as the values, touch minor and "
           "tool sizes as the touch major, size and orientation 0: %s",
           yes(others));
}

static void end_gesture(struct state *state) {
    const struct gesture *gesture = &state->gesture;
    const char *name = kNames[state->index];
    LOG("%s: a touchscreen's fingers, one device: %s", name, yes(gesture->device));
    LOG("%s: the down time the DOWN's time, times never back and on CLOCK_MONOTONIC: %s", name, yes(gesture->times));
    LOG("%s: history times never back and not after the event's time: %s, pressure above 0 and at most 1: %s", name,
        yes(gesture->history), yes(gesture->pressure));
    LOG("%s: raw places and axis values the same as x and y: %s", name, yes(gesture->raw));
    LOG("%s: %s: %s", name, kPlaces[state->index], yes(gesture->place));
    DEVICE("%s: %d events, %d MOVEs, %d with history, %d historical samples", name, gesture->events, gesture->moves,
           gesture->with_history, gesture->historical);
    state->in_gesture = false;
    ++state->index;
}

static void on_event(struct state *state, const AInputEvent *event) {
    ++state->events;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        LOG("an event of type %d", AInputEvent_getType(event));
        return;
    }
    const int32_t action = AMotionEvent_getAction(event), masked = action & AMOTION_EVENT_ACTION_MASK;
    const size_t pointer =
        (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const size_t count = AMotionEvent_getPointerCount(event);
    char text[32], ids[64];
    describe_action(action, text, sizeof text);
    describe_ids(event, ids, sizeof ids);
    if (masked == AMOTION_EVENT_ACTION_DOWN && !state->in_gesture && state->index < kGestures) start_gesture(state, event);
    if (!state->in_gesture) {
        LOG("%s outside a gesture, pointers %zu, IDs %s", text, count, ids);
        return;
    }
    // A run of MOVEs with the same pointers is one line: Android batches them by frame.
    if (masked != AMOTION_EVENT_ACTION_MOVE || strcmp(state->last_move, ids) != 0) {
        LOG("%s: %s, pointers %zu, IDs %s", kNames[state->index], text, count, ids);
    }
    snprintf(state->last_move, sizeof state->last_move, "%s", masked == AMOTION_EVENT_ACTION_MOVE ? ids : "");
    check_event(state, event, masked, count);
    check_places(state, event, masked, pointer, count);
    show_history(state, event);
    if (masked == AMOTION_EVENT_ACTION_UP || masked == AMOTION_EVENT_ACTION_CANCEL) end_gesture(state);
}

static void end(struct state *state) {
    LOG("the history's size as an index: each event's own time and place: %s", yes(state->own_sample));
    LOG("the queue: getEvent 0 for each event, -11 and NULL once it is empty: %s", yes(state->got && state->empty));
    LOG("the queue: hasEvents 1 at each wake, preDispatchEvent 0 for each event: %s",
        yes(state->has_events && state->pre_dispatch));
    LOG("the queue after the last gesture: hasEvents %d", AInputQueue_hasEvents(state->app->inputQueue));
    finish(state);
}

// At LOOPER_ID_INPUT: every event the queue holds, as native_app_glue's process_input takes them.
static void take_events(struct state *state) {
    AInputQueue *queue = state->app->inputQueue;
    state->has_events = state->has_events && AInputQueue_hasEvents(queue) == 1;
    AInputEvent *event = NULL;
    int32_t got;
    while ((got = AInputQueue_getEvent(queue, &event)) >= 0) {
        state->got = state->got && got == 0;
        if (AInputQueue_preDispatchEvent(queue, event) != 0) {
            state->pre_dispatch = false;
            continue;
        }
        on_event(state, event);
        AInputQueue_finishEvent(queue, event, 1);
    }
    state->empty = state->empty && got == -11 && event == NULL;
    if (state->index == kGestures && !state->done) end(state);
}

// One frame in a dark gray. The Mac's host has no drawing by the CPU (ANativeWindow_lock answers -ENOSYS)
// and needs none for its touches.
static void draw_once(ANativeWindow *window) {
    ANativeWindow_Buffer buffer;
    const int32_t locked = ANativeWindow_lock(window, &buffer, NULL);
    if (locked == 0) {
        const size_t pixel = buffer.format == WINDOW_FORMAT_RGB_565 ? 2 : 4;
        for (int32_t row = 0; row < buffer.height; ++row) {
            memset((char *)buffer.bits + (size_t)row * (size_t)buffer.stride * pixel, 0x40, (size_t)buffer.width * pixel);
        }
        ANativeWindow_unlockAndPost(window);
    }
    DEVICE("a frame by the CPU: ANativeWindow_lock %d", locked);
}

static void on_command(struct android_app *app, int32_t command) {
    struct state *state = app->userData;
    if (command == APP_CMD_INIT_WINDOW && app->window != NULL && !state->drawn) {
        state->drawn = true;
        draw_once(app->window);
    }
    if (command != APP_CMD_GAINED_FOCUS || state->ready) return;
    state->ready = true;
    state->deadline = now() + 30 * kSecond;
    LOG("ready: an input queue: %s, a window: %s", yes(app->inputQueue != NULL), yes(app->window != NULL));
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
}

void android_main(struct android_app *app) {
    struct state state = {0};
    state.app = app;
    state.got = state.empty = state.has_events = state.pre_dispatch = state.own_sample = true;
    app->userData = &state;
    app->onAppCmd = on_command;
    LOG("android_main");

    while (!app->destroyRequested) {
        int timeout = -1;
        if (state.deadline != 0 && !state.done) {
            const int64_t left = state.deadline - now();
            timeout = left > 0 ? (int)((left + kMillisecond - 1) / kMillisecond) : 0;
        }
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(timeout, NULL, NULL, (void **)&source);
        if (result == LOOPER_ID_MAIN && source != NULL) source->process(app, source);
        if (result == LOOPER_ID_INPUT && app->inputQueue != NULL) take_events(&state);
        if (state.deadline != 0 && !state.done && now() >= state.deadline) {
            LOG("stalled after %d events", state.events);
            finish(&state);
        }
    }
    LOG("android_main returns");
}
