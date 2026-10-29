// native-ime-test (ADR-0068 step 2e): a NativeActivity without native_app_glue that asks for the soft keyboard with
// ANativeActivity_showSoftInput at its window's first focus and logs each key event its input queue gets, under the
// tag "native-ime-test": the action, the key code, the meta state, the flags, the repeat count and the source; the
// device, the scan code and whether the down time is the event time go to "native-ime-test-device". On Android 15 the
// fixture input method of the same APK never starts: its input method manager ignores both requests, since the
// activity's view is not the served one. Its window shows one frame drawn by the CPU, without which Android gives it
// no focus. Four seconds after the show it logs "soft input done" and hides the keyboard; when no key came by then it
// first asks again with ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED and waits four seconds more. After "soft input done"
// the script sends B and DEL from outside the app, as a hardware keyboard's; a motion event is logged too, and
// none comes, since nothing touches the window. The DEL's release ends the test with
// "done" and ANativeActivity_finish, or "timeout" and "done" ten seconds later.
// native_ime_test_single_onCreate, for the Mac's scripted host only and never called on Android, takes one event per
// call of its looper's callback, so that a key's second delivery must wake the looper itself, and finishes the
// predispatch of B's down as handled, after which that down does not come again.
#include <android/input.h>
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

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "native-ime-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "native-ime-test-device", __VA_ARGS__)

static ANativeActivity *g_activity;
static AInputQueue *g_queue;
static atomic_int g_keys;
static atomic_bool g_soft_done, g_done, g_started;
static bool g_single;

static const char *key_name(int32_t code) {
    switch (code) {
    case AKEYCODE_UNKNOWN: return "UNKNOWN";
    case AKEYCODE_B: return "B";
    case AKEYCODE_E: return "E";
    case AKEYCODE_H: return "H";
    case AKEYCODE_I: return "I";
    case AKEYCODE_SHIFT_LEFT: return "SHIFT_LEFT";
    case AKEYCODE_ENTER: return "ENTER";
    case AKEYCODE_DEL: return "DEL";
    default: return NULL;
    }
}

static const char *action_name(int32_t action) {
    switch (action) {
    case AKEY_EVENT_ACTION_DOWN: return "down";
    case AKEY_EVENT_ACTION_UP: return "up";
    case AKEY_EVENT_ACTION_MULTIPLE: return "multiple";
    default: return "?";
    }
}

static void finish(const char *why) {
    if (atomic_exchange(&g_done, true)) return;
    if (why != NULL) LOG("%s", why);
    LOG("done");
    ANativeActivity_finish(g_activity);
}

static void log_key(const AInputEvent *event) {
    const int32_t code = AKeyEvent_getKeyCode(event), action = AKeyEvent_getAction(event);
    char key[16];
    const char *name = key_name(code);
    if (name == NULL) {
        snprintf(key, sizeof key, "%d", code);
        name = key;
    }
    LOG("key %s %s, meta 0x%x, flags 0x%x, repeat %d, source 0x%x", action_name(action), name,
        AKeyEvent_getMetaState(event), AKeyEvent_getFlags(event), AKeyEvent_getRepeatCount(event),
        AInputEvent_getSource(event));
    DEVICE("key %s %s: device %d, scan code %d, down time %s event time", action_name(action), name,
           AInputEvent_getDeviceId(event), AKeyEvent_getScanCode(event),
           AKeyEvent_getDownTime(event) == AKeyEvent_getEventTime(event) ? "=" : "!=");
}

static int on_input(int fd, int events, void *data) {
    (void)fd;
    (void)events;
    AInputQueue *queue = data;
    AInputEvent *event = NULL;
    while (AInputQueue_getEvent(queue, &event) >= 0) {
        if (g_single && AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY
            && (AKeyEvent_getFlags(event) & 0x20000000) != 0 && AKeyEvent_getKeyCode(event) == AKEYCODE_B
            && AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN) {
            LOG("predispatch handled: down B");  // AKEY_EVENT_FLAG_PREDISPATCH
            AInputQueue_finishEvent(queue, event, 1);
            return 1;
        }
        if (AInputQueue_preDispatchEvent(queue, event)) {
            LOG("pre-dispatched");
            if (g_single) return 1;
            continue;
        }
        int handled = 0;
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
            handled = 1;
            log_key(event);
            if (!atomic_load(&g_soft_done)) {
                atomic_fetch_add(&g_keys, 1);
            } else if (AKeyEvent_getKeyCode(event) == AKEYCODE_DEL
                       && AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP) {
                finish(NULL);
            }
        } else {
            LOG("motion action %d", AMotionEvent_getAction(event));  // none on the AVD, which nothing touches
        }
        AInputQueue_finishEvent(queue, event, handled);
        if (g_single) return 1;
    }
    return 1;
}

static void *timer(void *unused) {
    (void)unused;
    sleep(4);
    if (atomic_load(&g_keys) == 0) {
        LOG("no keys: showSoftInput forced");
        ANativeActivity_showSoftInput(g_activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
        sleep(4);
    }
    LOG("soft input done: %d key events", atomic_load(&g_keys));
    atomic_store(&g_soft_done, true);
    ANativeActivity_hideSoftInput(g_activity, 0);
    LOG("hideSoftInput 0");
    sleep(10);
    finish("timeout");
    return NULL;
}

static void on_window_focus_changed(ANativeActivity *activity, int focused) {
    if (!focused || atomic_exchange(&g_started, true)) return;
    LOG("ready");
    ANativeActivity_showSoftInput(activity, 0);
    LOG("showSoftInput 0");
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
    g_queue = queue;
    AInputQueue_attachLooper(queue, ALooper_forThread(), 1, on_input, queue);
}

static void on_queue_destroyed(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    AInputQueue_detachLooper(queue);
    g_queue = NULL;
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

__attribute__((visibility("default"))) void native_ime_test_single_onCreate(ANativeActivity *activity, void *state,
                                                                              size_t size) {
    g_single = true;
    ANativeActivity_onCreate(activity, state, size);
}
