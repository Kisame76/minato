// gamepad-native-test (ADR-0068 decision 5, the controllers' step 4): a NativeActivity without native_app_glue that
// logs what a game controller gives its input queue, under the tag "gamepad-native-test": each key's action, key code
// and source, and each motion event's action, source, pointer and history counts and the axes that are not zero (the
// ones gamepad-test names, by AMOTION_EVENT_AXIS_*). What follows the device goes to "gamepad-native-test-device":
// whether it is the first event's device, and a motion event's tool type, button state, meta state, flags, edge flags,
// classification, down time against the event time, precisions, pointer ID and place. The window shows one frame drawn
// by the CPU, without which Android gives it no focus, and logs "ready: pad" at its first focus, where the script
// starts the controller. The 18th motion event ends the test with "done" and ANativeActivity_finish, or "timeout"
// and "done" 60 seconds after the focus. The key flow is native-ime-test's: preDispatchEvent finishes a key's first
// delivery, which comes again.
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "gamepad-native-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "gamepad-native-test-device", __VA_ARGS__)

static ANativeActivity *g_activity;
static atomic_bool g_done, g_started;
static int g_motions;
static int32_t g_device = -1;

static const struct {
    int32_t axis;
    const char *name;
} AXES[] = {
    {AMOTION_EVENT_AXIS_X, "AXIS_X"},
    {AMOTION_EVENT_AXIS_Y, "AXIS_Y"},
    {AMOTION_EVENT_AXIS_Z, "AXIS_Z"},
    {AMOTION_EVENT_AXIS_RX, "AXIS_RX"},
    {AMOTION_EVENT_AXIS_RY, "AXIS_RY"},
    {AMOTION_EVENT_AXIS_RZ, "AXIS_RZ"},
    {AMOTION_EVENT_AXIS_LTRIGGER, "AXIS_LTRIGGER"},
    {AMOTION_EVENT_AXIS_RTRIGGER, "AXIS_RTRIGGER"},
    {AMOTION_EVENT_AXIS_BRAKE, "AXIS_BRAKE"},
    {AMOTION_EVENT_AXIS_GAS, "AXIS_GAS"},
    {AMOTION_EVENT_AXIS_HAT_X, "AXIS_HAT_X"},
    {AMOTION_EVENT_AXIS_HAT_Y, "AXIS_HAT_Y"},
    {AMOTION_EVENT_AXIS_PRESSURE, "AXIS_PRESSURE"},
};

static const char *key_name(int32_t code, char *other) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return "BUTTON_A";
    case AKEYCODE_BUTTON_B: return "BUTTON_B";
    case AKEYCODE_BUTTON_X: return "BUTTON_X";
    case AKEYCODE_BUTTON_Y: return "BUTTON_Y";
    case AKEYCODE_BUTTON_L1: return "BUTTON_L1";
    case AKEYCODE_BUTTON_R1: return "BUTTON_R1";
    case AKEYCODE_BUTTON_THUMBL: return "BUTTON_THUMBL";
    case AKEYCODE_BUTTON_THUMBR: return "BUTTON_THUMBR";
    case AKEYCODE_BUTTON_START: return "BUTTON_START";
    case AKEYCODE_BUTTON_SELECT: return "BUTTON_SELECT";
    case AKEYCODE_BUTTON_MODE: return "BUTTON_MODE";
    default: snprintf(other, 16, "%d", code); return other;
    }
}

static void finish(const char *why) {
    if (atomic_exchange(&g_done, true)) return;
    if (why != NULL) LOG("%s", why);
    LOG("done");
    ANativeActivity_finish(g_activity);
}

static const char *motion_action(int32_t action) {
    static char other[16];
    switch (action & AMOTION_EVENT_ACTION_MASK) {
    case AMOTION_EVENT_ACTION_MOVE: return "MOVE";
    case AMOTION_EVENT_ACTION_HOVER_MOVE: return "HOVER_MOVE";
    case AMOTION_EVENT_ACTION_SCROLL: return "SCROLL";
    default: snprintf(other, sizeof other, "%d", action); return other;
    }
}

static void log_key(const AInputEvent *event) {
    if (AKeyEvent_getRepeatCount(event) > 0) return;  // a held key's repeats are the device's timing
    char other[16];
    LOG("key %s %s, source 0x%x", AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN ? "down" : "up",
        key_name(AKeyEvent_getKeyCode(event), other), AInputEvent_getSource(event));
}

static void log_motion(const AInputEvent *event) {
    char line[512];
    int length = snprintf(line, sizeof line, "motion %s, source 0x%x, pointers %zu, history %zu, axes",
                          motion_action(AMotionEvent_getAction(event)), AInputEvent_getSource(event),
                          AMotionEvent_getPointerCount(event), AMotionEvent_getHistorySize(event));
    for (size_t at = 0; at < sizeof AXES / sizeof AXES[0]; ++at) {
        const float value = AMotionEvent_getAxisValue(event, AXES[at].axis, 0);
        if (value != 0.f && length < (int)sizeof line) {
            length += snprintf(line + length, sizeof line - (size_t)length, " %s=%g", AXES[at].name, (double)value);
        }
    }
    LOG("%s", line);
    const int32_t id = AInputEvent_getDeviceId(event);
    if (g_device < 0) g_device = id;
    DEVICE("motion: device %s, tool %d, buttons 0x%x, meta 0x%x, flags 0x%x, edge 0x%x, classification %d, "
           "down time %s event time, precision %g %g, pointer %d, place %g %g raw %g %g",
           id == g_device ? "pad" : "other", AMotionEvent_getToolType(event, 0), AMotionEvent_getButtonState(event),
           AMotionEvent_getMetaState(event), AMotionEvent_getFlags(event), AMotionEvent_getEdgeFlags(event),
           AMotionEvent_getClassification(event),
           AMotionEvent_getDownTime(event) == AMotionEvent_getEventTime(event) ? "=" : "!=",
           (double)AMotionEvent_getXPrecision(event), (double)AMotionEvent_getYPrecision(event),
           AMotionEvent_getPointerId(event, 0),
           (double)AMotionEvent_getX(event, 0), (double)AMotionEvent_getY(event, 0),
           (double)AMotionEvent_getRawX(event, 0), (double)AMotionEvent_getRawY(event, 0));
}

static int on_input(int fd, int events, void *data) {
    (void)fd;
    (void)events;
    AInputQueue *queue = data;
    AInputEvent *event = NULL;
    while (AInputQueue_getEvent(queue, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(queue, event)) continue;
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
            log_key(event);
            const int32_t id = AInputEvent_getDeviceId(event);
            if (g_device < 0) g_device = id;
            DEVICE("key: device %s, scan code %d", id == g_device ? "pad" : "other", AKeyEvent_getScanCode(event));
        } else {
            log_motion(event);
            if (++g_motions == 18) {
                AInputQueue_finishEvent(queue, event, 1);
                finish(NULL);
                return 1;
            }
        }
        AInputQueue_finishEvent(queue, event, 1);
    }
    return 1;
}

static void *timer(void *unused) {
    (void)unused;
    sleep(60);
    finish("timeout");
    return NULL;
}

static void on_window_focus_changed(ANativeActivity *activity, int focused) {
    (void)activity;
    if (!focused || atomic_exchange(&g_started, true)) return;
    LOG("ready: pad");
    pthread_t thread;
    if (pthread_create(&thread, NULL, timer, NULL) == 0) pthread_detach(thread);
}

static void on_window_created(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBX_8888);
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, NULL) != 0) return;
    for (int32_t y = 0; y < buffer.height; y++) {
        memset((uint32_t *)buffer.bits + (size_t)y * (size_t)buffer.stride, 0x40, (size_t)buffer.width * 4);
    }
    ANativeWindow_unlockAndPost(window);
}

static void on_queue_created(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    AInputQueue_attachLooper(queue, ALooper_forThread(), 1, on_input, queue);
}

static void on_queue_destroyed(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    AInputQueue_detachLooper(queue);
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *activity, void *state,
                                                                       size_t size) {
    (void)state;
    (void)size;
    g_activity = activity;
    activity->callbacks->onWindowFocusChanged = on_window_focus_changed;
    activity->callbacks->onNativeWindowCreated = on_window_created;
    activity->callbacks->onInputQueueCreated = on_queue_created;
    activity->callbacks->onInputQueueDestroyed = on_queue_destroyed;
}
