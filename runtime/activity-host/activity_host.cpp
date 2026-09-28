// activity-host (ADR-0028, decision 1): the guest program /system/bin/activity-host, which plays Android's app
// process for a NativeActivity.
//   activity-host --package <package> --apk <guest path of base.apk> [--library <lib_name>] [--function <func_name>]
// Its main thread plays Android's main thread. It prepares an ALooper, connects to the session (the event
// pipe's read end and the configuration), creates the app's three folders as NativeActivity's getters do,
// builds the asset manager and the JNI stub, opens lib<lib_name>.so (the host's LD_LIBRARY_PATH names the
// app's library folder) and looks up the function. Then its looper reads the events the host posts: each
// calls exactly its NativeActivity callback, in the host's order, and is answered once the callback returned.
// As NativeActivity does, a new window or queue first destroys the one still there, and DESTROY destroys the
// window and the queue before onDestroy. onSaveInstanceState's buffer is freed, not kept (decision 1). Once
// onDestroy is answered, the program ends with status 0, without destructors, as Android's process is later
// killed. Without a session it ends with status 1; when the APK, the library or the function is missing, it
// reports the reason (dlerror's text) to the host first.
//
// What the host does first (decision 14; runtime/tests/activity_test.cpp does all of it on the Mac):
//   - apkrt_mount: /data/app/<package>/base.apk to the APK; /data/user/0/<package> and /data/data/<package> to
//     the app's data folder; /storage/emulated/0/Android/data/<package> and /sdcard/Android/data/<package> to its
//     external data; /storage/emulated/0/Android/obb/<package> and /sdcard/Android/obb/<package> to its OBB
//     folder (the host folders must exist); /system to the guest system's folder;
//   - apkrt_add_library_directory: /system/lib64 to the system's containers (bionic, libandroid, libnativewindow,
//     libz, the graphics front libs), /data/app/<package>/lib/arm64 to the app's;
//   - apkrt_session_begin, then apkrt_start_program with activity-host's container, argv[0]
//     /system/bin/activity-host and the arguments above, and the environment
//     LD_LIBRARY_PATH=/data/app/<package>/lib/arm64.
// The events, in the order Android 15 calls the callbacks (fixtures/expected/activity-test.logcat), with the
// window's steps of decision 6 in brackets:
//   - start: [apkrt_window_add] CREATE, START, RESUME, INPUT_QUEUE_CREATED, NATIVE_WINDOW_CREATED,
//     NATIVE_WINDOW_RESIZED (the same size), CONTENT_RECT_CHANGED, NATIVE_WINDOW_REDRAW_NEEDED,
//     WINDOW_FOCUS_CHANGED 1, [the gate opens once the scene is active];
//   - background: [background, "scheduled", apkrt_window_remove] PAUSE, WINDOW_FOCUS_CHANGED 0,
//     NATIVE_WINDOW_DESTROYED, STOP, SAVE_INSTANCE_STATE;
//   - foreground: [background off, apkrt_window_add] START, RESUME, NATIVE_WINDOW_CREATED, NATIVE_WINDOW_RESIZED,
//     NATIVE_WINDOW_REDRAW_NEEDED, WINDOW_FOCUS_CHANGED 1 (CONTENT_RECT_CHANGED only when the rectangle
//     changed), [the gate opens];
//   - Quit, or ANativeActivity_finish's FINISH: [apkrt_window_remove] PAUSE, WINDOW_FOCUS_CHANGED 0,
//     NATIVE_WINDOW_DESTROYED, STOP, DESTROY; activity-host destroys the input queue itself before onDestroy.
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "jni_stub.h"

namespace {

constexpr const char *kTag = "activity-host";

struct Host {
    ANativeActivityCallbacks callbacks;
    ANativeActivity activity;
    ANativeActivity_createFunc *create;
    bool created;
    ANativeWindow *window;  // from NATIVE_WINDOW_CREATED to NATIVE_WINDOW_DESTROYED, with activity-host's reference
    AInputQueue *queue;
};

Host host;

[[noreturn]] void start_failed(const char *reason) {
    if (reason == nullptr) reason = "unknown";
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", reason);
    syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_START_FAILED, reason, 0);
    _exit(1);
}

// A path from a format, for the life of the program.
char *path(const char *format, const char *package) {
    char *text = nullptr;
    if (asprintf(&text, format, package) < 0) start_failed("out of memory");
    return text;
}

// Creates one of the app's folders in the folder the host mounted for it (decision 14).
void make_folder(const char *folder) {
    if (mkdir(folder, 0771) != 0 && errno != EEXIST) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "cannot create %s: %s", folder, strerror(errno));
    }
}

void destroy_window() {
    if (host.window == nullptr) return;
    if (host.callbacks.onNativeWindowDestroyed != nullptr) {
        host.callbacks.onNativeWindowDestroyed(&host.activity, host.window);
    }
    ANativeWindow_release(host.window);
    host.window = nullptr;
}

void destroy_queue() {
    if (host.queue == nullptr) return;
    if (host.callbacks.onInputQueueDestroyed != nullptr) host.callbacks.onInputQueueDestroyed(&host.activity, host.queue);
    minato_AInputQueue_delete(host.queue);
    host.queue = nullptr;
}

// Calls the callback of one event.
void dispatch(const apkrt_event &event) {
    ANativeActivityCallbacks &callbacks = host.callbacks;
    ANativeActivity *const activity = &host.activity;
    const uint64_t *const argument = event.arguments;
    switch (event.type) {
        case APKRT_EVENT_CREATE:
            if (!host.created) {
                host.created = true;
                host.create(activity, nullptr, 0);  // no saved state (decision 1)
            }
            break;
        case APKRT_EVENT_START:
            if (callbacks.onStart != nullptr) callbacks.onStart(activity);
            break;
        case APKRT_EVENT_RESUME:
            if (callbacks.onResume != nullptr) callbacks.onResume(activity);
            break;
        case APKRT_EVENT_PAUSE:
            if (callbacks.onPause != nullptr) callbacks.onPause(activity);
            break;
        case APKRT_EVENT_STOP:
            if (callbacks.onStop != nullptr) callbacks.onStop(activity);
            break;
        case APKRT_EVENT_DESTROY:
            destroy_window();
            destroy_queue();
            if (callbacks.onDestroy != nullptr) callbacks.onDestroy(activity);
            break;
        case APKRT_EVENT_SAVE_INSTANCE_STATE:
            if (callbacks.onSaveInstanceState != nullptr) {
                size_t size = 0;
                free(callbacks.onSaveInstanceState(activity, &size));
            }
            break;
        case APKRT_EVENT_WINDOW_FOCUS_CHANGED:
            if (callbacks.onWindowFocusChanged != nullptr) callbacks.onWindowFocusChanged(activity, argument[0] != 0);
            break;
        case APKRT_EVENT_NATIVE_WINDOW_CREATED:
            destroy_window();
            host.window = minato_ANativeWindow_create(argument[0], static_cast<int32_t>(argument[1]),
                                                      static_cast<int32_t>(argument[2]));
            if (host.window != nullptr && callbacks.onNativeWindowCreated != nullptr) {
                callbacks.onNativeWindowCreated(activity, host.window);
            }
            break;
        case APKRT_EVENT_NATIVE_WINDOW_RESIZED:
            if (host.window != nullptr && callbacks.onNativeWindowResized != nullptr) {
                callbacks.onNativeWindowResized(activity, host.window);
            }
            break;
        case APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED:
            if (host.window != nullptr && callbacks.onNativeWindowRedrawNeeded != nullptr) {
                callbacks.onNativeWindowRedrawNeeded(activity, host.window);
            }
            break;
        case APKRT_EVENT_NATIVE_WINDOW_DESTROYED:
            destroy_window();
            break;
        case APKRT_EVENT_INPUT_QUEUE_CREATED:
            destroy_queue();
            host.queue = minato_AInputQueue_create(argument[0]);
            if (host.queue != nullptr && callbacks.onInputQueueCreated != nullptr) {
                callbacks.onInputQueueCreated(activity, host.queue);
            }
            break;
        case APKRT_EVENT_INPUT_QUEUE_DESTROYED:
            destroy_queue();
            break;
        case APKRT_EVENT_CONTENT_RECT_CHANGED:
            if (callbacks.onContentRectChanged != nullptr) {
                const ARect rect{static_cast<int32_t>(argument[0] & 0xffffffff), static_cast<int32_t>(argument[0] >> 32),
                                 static_cast<int32_t>(argument[1] & 0xffffffff), static_cast<int32_t>(argument[1] >> 32)};
                callbacks.onContentRectChanged(activity, &rect);
            }
            break;
        case APKRT_EVENT_CONFIGURATION_CHANGED:
            if (callbacks.onConfigurationChanged != nullptr) callbacks.onConfigurationChanged(activity);
            break;
        case APKRT_EVENT_LOW_MEMORY:
            if (callbacks.onLowMemory != nullptr) callbacks.onLowMemory(activity);
            break;
        default:
            __android_log_print(ANDROID_LOG_WARN, kTag, "an event of type %u", event.type);
            break;
    }
}

// The looper's callback for the event pipe: one event per call, as Android's main thread takes one
// message per round. The pipe leaves the looper while the callback runs: an app that polls the main
// thread's looper inside a callback gets no next event in it, as Android's lifecycle messages wait for
// the one before.
int on_event(int fd, int events, void *) {
    apkrt_event event;
    const ssize_t got = read(fd, &event, sizeof event);
    if (got < 0 && errno == EINTR) return 1;
    if (got != static_cast<ssize_t>(sizeof event)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "the event pipe broke (%zd bytes, events %#x)", got, events);
        _exit(1);
    }
    ALooper *const looper = ALooper_forThread();
    ALooper_removeFd(looper, fd);
    dispatch(event);
    syscall(APKRT_NR_SESSION_ANSWER, event.serial);
    if (event.type == APKRT_EVENT_DESTROY) _exit(0);
    if (ALooper_addFd(looper, fd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_event, nullptr) != 1) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "cannot watch the event pipe again");
        _exit(1);
    }
    return 1;
}

[[noreturn]] void usage() {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "usage: activity-host --package <package> --apk <base.apk> [--library <name>] [--function <name>]");
    _exit(2);
}

}  // namespace

int main(int argc, char **argv) {
    const char *package = nullptr, *apk = nullptr, *library = "main", *function = "ANativeActivity_onCreate";
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) usage();
        const char *const option = argv[index], *const value = argv[index + 1];
        if (strcmp(option, "--package") == 0) {
            package = value;
        } else if (strcmp(option, "--apk") == 0) {
            apk = value;
        } else if (strcmp(option, "--library") == 0) {
            library = value;
        } else if (strcmp(option, "--function") == 0) {
            function = value;
        } else {
            usage();
        }
    }
    if (package == nullptr || apk == nullptr) usage();

    ALooper *const looper = ALooper_prepare(0);
    struct apkrt_configuration configuration {};
    const long events = syscall(APKRT_NR_SESSION_CONNECT, &configuration);
    if (events < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "no session: %s", strerror(errno));
        _exit(1);
    }

    // What NativeActivity hands the native side [paths inferred from its getters; activity-test compares them].
    const char *const files = path("/data/user/0/%s/files", package);
    const char *const external = path("/storage/emulated/0/Android/data/%s/files", package);
    const char *const obb = path("/storage/emulated/0/Android/obb/%s", package);
    make_folder(files);
    make_folder(external);
    make_folder(obb);
    AAssetManager *const assets = minato_AAssetManager_create(apk, &configuration);
    if (assets == nullptr) {
        char reason[512];
        snprintf(reason, sizeof reason, "cannot read the APK %s", apk);
        start_failed(reason);
    }
    char name[256];
    if (snprintf(name, sizeof name, "lib%s.so", library) >= static_cast<int>(sizeof name)) start_failed("too long a lib_name");
    void *const handle = dlopen(name, RTLD_NOW);
    if (handle == nullptr) start_failed(dlerror());
    host.create = reinterpret_cast<ANativeActivity_createFunc *>(dlsym(handle, function));
    if (host.create == nullptr) start_failed(dlerror());

    ANativeActivity &activity = host.activity;
    activity.callbacks = &host.callbacks;
    minato_jni_stub(&activity.vm, &activity.env, &activity.clazz);
    activity.internalDataPath = files;
    activity.externalDataPath = external;
    activity.sdkVersion = 35;
    activity.assetManager = assets;
    activity.obbPath = obb;

    if (ALooper_addFd(looper, static_cast<int>(events), ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_event, nullptr) != 1) {
        start_failed("cannot watch the event pipe");
    }
    for (;;) {
        if (ALooper_pollOnce(-1, nullptr, nullptr, nullptr) == ALOOPER_POLL_ERROR) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "the main looper failed");
            _exit(1);
        }
    }
}
