// native-jni-ime-test (ADR-0069 step 1): a NativeActivity without native_app_glue that opens the soft keyboard
// through JNI from its activity object at its window's first focus, as NDK apps do since
// ANativeActivity_showSoftInput does nothing on Android 15 (ADR-0068 step 2e), and logs each answer under the tag
// "native-jni-ime-test": getSystemService(INPUT_METHOD_SERVICE), getWindow() and getDecorView(); showSoftInput on the
// decor view with SHOW_FORCED, which Android 15 ignores, as no view of the window is served; what the manager says of
// the decor view (isActive, isAcceptingText); showSoftInput on NativeActivity's content view, ignored too; then
// setFocusableInTouchMode and requestFocus on the decor view, and half a second later, on the main looper, the manager
// again and showSoftInput on the now focused, served decor view, which shows the keyboard. Then it logs each key
// event its input queue gets: the action, the key code, the meta state, the flags, the repeat count and the source;
// the device, the scan code and whether the down time is the event time go to "native-jni-ime-test-device". On
// Android the fixture input method of the same APK types into the window, through the manager's fallback connection.
// It logs its window's creation and size, and the window shows one frame drawn by the CPU, without which Android
// gives it no focus. At the release of the editor
// action's ENTER (FLAG_EDITOR_ACTION), the input method's last call, or 15 s after the focus, it logs "soft input
// done" and hides the keyboard with hideSoftInputFromWindow, on the main thread or one it attaches to the VM.
// After "soft input done" the script sends B and DEL from outside the app, as a hardware keyboard's; a motion event is
// logged too, and none comes, since nothing touches the window. The DEL's release ends the test with "done" and
// ANativeActivity_finish, or "timeout" and "done" ten seconds later.
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "native-jni-ime-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "native-jni-ime-test-device", __VA_ARGS__)

enum { SHOW_FORCED = 2 };  // InputMethodManager.SHOW_FORCED

static ANativeActivity *g_activity;
static jobject g_imm, g_decor;  // global references, from the main thread
static atomic_int g_keys;
static atomic_bool g_soft_done, g_done, g_started;

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

/// False, after a line naming `what` and the exception, when the last call threw.
static bool succeeded(JNIEnv *env, const char *what) {
    if (!(*env)->ExceptionCheck(env)) return true;
    jthrowable thrown = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    jclass type = (*env)->GetObjectClass(env, thrown);
    jmethodID to_string = (*env)->GetMethodID(env, type, "toString", "()Ljava/lang/String;");
    jstring text = (*env)->CallObjectMethod(env, thrown, to_string);
    const char *chars = text != NULL ? (*env)->GetStringUTFChars(env, text, NULL) : NULL;
    LOG("%s threw %s", what, chars != NULL ? chars : "(no text)");
    if (chars != NULL) (*env)->ReleaseStringUTFChars(env, text, chars);
    (*env)->ExceptionClear(env);
    return false;
}

/// The object's class name, as getClass().getName() gives it, or "null".
static void class_name(JNIEnv *env, jobject object, char *out, size_t size) {
    snprintf(out, size, "null");
    if (object == NULL) return;
    jclass type = (*env)->GetObjectClass(env, object);
    jclass class_class = (*env)->FindClass(env, "java/lang/Class");
    jmethodID get_name = (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;");
    jstring name = (*env)->CallObjectMethod(env, type, get_name);
    if (!succeeded(env, "getName") || name == NULL) return;
    const char *chars = (*env)->GetStringUTFChars(env, name, NULL);
    snprintf(out, size, "%s", chars);
    (*env)->ReleaseStringUTFChars(env, name, chars);
}

static void probe(JNIEnv *env, const char *when);

/// The JNI calls an NDK app makes for the keyboard, from the activity object on the main thread.
static void show_keyboard(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    jobject self = activity->clazz;
    char name[128];
    jclass context = (*env)->FindClass(env, "android/content/Context");
    if (!succeeded(env, "FindClass android/content/Context") || context == NULL) return;
    jfieldID field = (*env)->GetStaticFieldID(env, context, "INPUT_METHOD_SERVICE", "Ljava/lang/String;");
    jobject service = field != NULL ? (*env)->GetStaticObjectField(env, context, field) : NULL;
    if (!succeeded(env, "Context.INPUT_METHOD_SERVICE") || service == NULL) return;
    jclass type = (*env)->GetObjectClass(env, self);
    class_name(env, self, name, sizeof name);
    LOG("activity: %s", name);
    jmethodID get_system_service =
        (*env)->GetMethodID(env, type, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject imm = (*env)->CallObjectMethod(env, self, get_system_service, service);
    if (!succeeded(env, "getSystemService")) return;
    class_name(env, imm, name, sizeof name);
    LOG("getSystemService(input_method): %s", name);
    jmethodID get_window = (*env)->GetMethodID(env, type, "getWindow", "()Landroid/view/Window;");
    jobject window = (*env)->CallObjectMethod(env, self, get_window);
    if (!succeeded(env, "getWindow")) return;
    class_name(env, window, name, sizeof name);
    LOG("getWindow: %s", name);
    if (imm == NULL || window == NULL) return;
    jmethodID get_decor_view =
        (*env)->GetMethodID(env, (*env)->GetObjectClass(env, window), "getDecorView", "()Landroid/view/View;");
    jobject decor = (*env)->CallObjectMethod(env, window, get_decor_view);
    if (!succeeded(env, "getDecorView")) return;
    class_name(env, decor, name, sizeof name);
    LOG("getDecorView: %s", name);
    if (decor == NULL) return;
    jmethodID show = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, imm), "showSoftInput",
                                         "(Landroid/view/View;I)Z");
    jboolean shown = (*env)->CallBooleanMethod(env, imm, show, decor, SHOW_FORCED);
    if (!succeeded(env, "showSoftInput")) return;
    LOG("showSoftInput(decorView, SHOW_FORCED): %s", shown ? "true" : "false");
    g_imm = (*env)->NewGlobalRef(env, imm);
    g_decor = (*env)->NewGlobalRef(env, decor);
    probe(env, "at the focus");
    // The content view (NativeActivity's NativeContentView), and the decor view focused by hand.
    jmethodID find_view = (*env)->GetMethodID(env, type, "findViewById", "(I)Landroid/view/View;");
    jobject content = (*env)->CallObjectMethod(env, self, find_view, 0x01020002);  // android.R.id.content
    if (!succeeded(env, "findViewById") || content == NULL) return;
    jmethodID get_child = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, content), "getChildAt",
                                              "(I)Landroid/view/View;");
    jobject child = (*env)->CallObjectMethod(env, content, get_child, 0);
    if (!succeeded(env, "getChildAt") || child == NULL) return;
    class_name(env, child, name, sizeof name);
    jmethodID is_active_view = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, imm), "isActive",
                                                   "(Landroid/view/View;)Z");
    LOG("content view: %s, isActive %d", name, (*env)->CallBooleanMethod(env, imm, is_active_view, child));
    jboolean child_shown = (*env)->CallBooleanMethod(env, imm, show, child, SHOW_FORCED);
    if (succeeded(env, "showSoftInput content")) LOG("showSoftInput(contentView, SHOW_FORCED): %d", child_shown);
    jclass view_class = (*env)->FindClass(env, "android/view/View");
    jmethodID focusable = (*env)->GetMethodID(env, view_class, "setFocusableInTouchMode", "(Z)V");
    jmethodID request = (*env)->GetMethodID(env, view_class, "requestFocus", "()Z");
    (*env)->CallVoidMethod(env, decor, focusable, JNI_TRUE);
    jboolean focused = (*env)->CallBooleanMethod(env, decor, request);
    if (succeeded(env, "requestFocus")) LOG("decorView.requestFocus: %d", focused);
}

/// What the manager says of the decor view now.
static void probe(JNIEnv *env, const char *when) {
    jclass type = (*env)->GetObjectClass(env, g_imm);
    jmethodID is_active_view = (*env)->GetMethodID(env, type, "isActive", "(Landroid/view/View;)Z");
    jmethodID is_active = (*env)->GetMethodID(env, type, "isActive", "()Z");
    jmethodID accepting = (*env)->GetMethodID(env, type, "isAcceptingText", "()Z");
    LOG("%s: isActive(decorView) %d, isActive() %d, isAcceptingText %d", when,
        (*env)->CallBooleanMethod(env, g_imm, is_active_view, g_decor),
        (*env)->CallBooleanMethod(env, g_imm, is_active), (*env)->CallBooleanMethod(env, g_imm, accepting));
    succeeded(env, "probe");
}

/// Half a second after the focus, on the main thread: the show again, once the focus moved.
static int show_again(int fd, int events, void *data) {
    (void)events;
    (void)data;
    char byte;
    (void)read(fd, &byte, 1);
    JNIEnv *env = g_activity->env;
    if (g_imm == NULL) return 0;
    probe(env, "after half a second");
    jmethodID show = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, g_imm), "showSoftInput",
                                         "(Landroid/view/View;I)Z");
    jboolean shown = (*env)->CallBooleanMethod(env, g_imm, show, g_decor, SHOW_FORCED);
    if (succeeded(env, "showSoftInput again")) LOG("showSoftInput(decorView, SHOW_FORCED) again: %d", shown);
    return 0;
}

/// hideSoftInputFromWindow(decorView.getWindowToken(), 0), on the calling thread, which it attaches to the VM unless
/// it is attached (the main thread).
static void hide_keyboard(void) {
    if (g_imm == NULL) return;
    JavaVM *vm = g_activity->vm;
    JNIEnv *env = NULL;
    const bool attached = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK;
    JavaVMAttachArgs attach = {JNI_VERSION_1_6, "native-jni-ime-test", NULL};
    if (!attached && (*vm)->AttachCurrentThread(vm, &env, &attach) != JNI_OK) {
        LOG("AttachCurrentThread failed");
        return;
    }
    jmethodID get_token = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, g_decor), "getWindowToken",
                                              "()Landroid/os/IBinder;");
    jobject token = (*env)->CallObjectMethod(env, g_decor, get_token);
    if (succeeded(env, "getWindowToken")) {
        jmethodID hide = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, g_imm), "hideSoftInputFromWindow",
                                             "(Landroid/os/IBinder;I)Z");
        jboolean hidden = (*env)->CallBooleanMethod(env, g_imm, hide, token, 0);
        if (succeeded(env, "hideSoftInputFromWindow")) {
            LOG("hideSoftInputFromWindow: %s", hidden ? "true" : "false");
        }
    }
    if (!attached) (*vm)->DetachCurrentThread(vm);
}

/// The keyboard's part ends once: at the editor action's ENTER, the input method's last call, or after 15 s.
static void soft_input_done(void) {
    if (atomic_exchange(&g_soft_done, true)) return;
    LOG("soft input done: %d key events", atomic_load(&g_keys));
    hide_keyboard();
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
        if (AInputQueue_preDispatchEvent(queue, event)) {
            LOG("pre-dispatched");
            continue;
        }
        int handled = 0;
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
            handled = 1;
            log_key(event);
            if (!atomic_load(&g_soft_done)) {
                atomic_fetch_add(&g_keys, 1);
                if (AKeyEvent_getKeyCode(event) == AKEYCODE_ENTER && AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP
                    && (AKeyEvent_getFlags(event) & AKEY_EVENT_FLAG_EDITOR_ACTION) != 0) {
                    soft_input_done();
                }
            } else if (AKeyEvent_getKeyCode(event) == AKEYCODE_DEL
                       && AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP) {
                finish(NULL);
            }
        } else {
            LOG("motion action %d", AMotionEvent_getAction(event));  // none on the AVD, which nothing touches
        }
        AInputQueue_finishEvent(queue, event, handled);
    }
    return 1;
}

static int g_wake[2] = {-1, -1};  // the timer's pipe to the main looper

static void *timer(void *unused) {
    (void)unused;
    usleep(500000);
    (void)write(g_wake[1], "x", 1);
    for (int tenths = 0; tenths < 150 && !atomic_load(&g_soft_done); ++tenths) usleep(100000);
    soft_input_done();
    sleep(10);
    finish("timeout");
    return NULL;
}

static void on_window_focus_changed(ANativeActivity *activity, int focused) {
    if (!focused || atomic_exchange(&g_started, true)) return;
    LOG("ready");
    if (pipe(g_wake) == 0) ALooper_addFd(ALooper_forThread(), g_wake[0], 2, ALOOPER_EVENT_INPUT, show_again, NULL);
    show_keyboard(activity);
    pthread_t thread;
    if (pthread_create(&thread, NULL, timer, NULL) == 0) pthread_detach(thread);
}

static void on_window_created(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    LOG("window created: %d x %d", ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
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
