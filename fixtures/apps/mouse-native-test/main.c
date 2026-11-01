// mouse-native-test (ADR-0068 decision 5, the mouse step): a NativeActivity without native_app_glue that logs what a
// mouse gives its input queue, under the tag "mouse-native-test": each motion event's action, source, tool, pointer and
// history counts, button state, meta state, whether its place moved from the event before, the wheels' axes and
// whether it has relative axes, and each key's action, key code and source. The window shows one frame drawn by the
// CPU, without which Android gives it no focus, and logs "ready: mouse" at its first focus, where the script starts the
// mouse. The 46th motion event ends the test with "done" and ANativeActivity_finish, or "timeout" and "done" 60
// seconds after the focus. The key flow is native-ime-test's: preDispatchEvent finishes a key's first delivery, which
// comes again.
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mouse-native-test", __VA_ARGS__)

#define MOTIONS 46

#define AXIS_RELATIVE_X 27
#define AXIS_RELATIVE_Y 28

static ANativeActivity *g_activity;
static atomic_bool g_done, g_started;
static int g_motions;
static float g_x = NAN, g_y = NAN;

static void finish(const char *why) {
    if (atomic_exchange(&g_done, true)) return;
    if (why != NULL) LOG("%s", why);
    LOG("done");
    ANativeActivity_finish(g_activity);
}

static const char *motion_action(int32_t action, char *other) {
    switch (action & AMOTION_EVENT_ACTION_MASK) {
    case AMOTION_EVENT_ACTION_DOWN: return "DOWN";
    case AMOTION_EVENT_ACTION_UP: return "UP";
    case AMOTION_EVENT_ACTION_MOVE: return "MOVE";
    case AMOTION_EVENT_ACTION_HOVER_MOVE: return "HOVER_MOVE";
    case AMOTION_EVENT_ACTION_SCROLL: return "SCROLL";
    case AMOTION_EVENT_ACTION_HOVER_ENTER: return "HOVER_ENTER";
    case AMOTION_EVENT_ACTION_HOVER_EXIT: return "HOVER_EXIT";
    case 11: return "BUTTON_PRESS";
    case 12: return "BUTTON_RELEASE";
    default: snprintf(other, 16, "%d", action); return other;
    }
}

static const char *tool_name(int32_t tool) {
    static const char *const names[] = {"UNKNOWN", "FINGER", "STYLUS", "MOUSE", "ERASER", "PALM"};
    return tool >= 0 && tool < 6 ? names[tool] : "?";
}

static void log_key(const AInputEvent *event) {
    const int32_t code = AKeyEvent_getKeyCode(event);
    const char *name = code == AKEYCODE_BACK ? "BACK" : code == AKEYCODE_FORWARD ? "FORWARD" : "?";
    LOG("key %s %s, source 0x%x", AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN ? "down" : "up", name,
        AInputEvent_getSource(event));
}

static void log_motion(const AInputEvent *event) {
    char other[16];
    const float x = AMotionEvent_getX(event, 0), y = AMotionEvent_getY(event, 0);
    const int moved = x != g_x || y != g_y;
    g_x = x;
    g_y = y;
    char line[512];
    int length = snprintf(line, sizeof line,
                          "motion %s, source 0x%x, tool %s, pointers %zu, history %zu, buttons 0x%x, meta 0x%x, "
                          "moved %s",
                          motion_action(AMotionEvent_getAction(event), other), AInputEvent_getSource(event),
                          tool_name(AMotionEvent_getToolType(event, 0)), AMotionEvent_getPointerCount(event),
                          AMotionEvent_getHistorySize(event), AMotionEvent_getButtonState(event),
                          AMotionEvent_getMetaState(event), moved ? "true" : "false");
    const float vscroll = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
    const float hscroll = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0);
    if (vscroll != 0.f) {
        length += snprintf(line + length, sizeof line - (size_t)length, ", VSCROLL=%g", (double)vscroll);
    }
    if (hscroll != 0.f) {
        length += snprintf(line + length, sizeof line - (size_t)length, ", HSCROLL=%g", (double)hscroll);
    }
    if (AMotionEvent_getAxisValue(event, AXIS_RELATIVE_X, 0) != 0.f ||
        AMotionEvent_getAxisValue(event, AXIS_RELATIVE_Y, 0) != 0.f) {
        snprintf(line + length, sizeof line - (size_t)length, ", relative");
    }
    LOG("%s", line);
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
        } else {
            log_motion(event);
            if (++g_motions == MOTIONS) {
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
    LOG("ready: mouse");
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
