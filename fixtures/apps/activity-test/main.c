// activity-test (ADR-0028, test 3): a NativeActivity on native_app_glue that logs, with the tag
// "activity-test", what Android 15 fixes: every command it gets, the fields of ANativeActivity, the JNI
// answers on its own thread, the window's format, three assets and folder listings, and the
// configuration's SDK version. What differs by device (sizes, density, locale) goes to the tag
// "activity-test-device", which is not compared with Android. At its first start it sets
// AWINDOW_FLAG_KEEP_SCREEN_ON. At its second window, a second after it gained the focus, it calls
// ANativeActivity_finish, the one way to onDestroy that Android 15 offers without Java.
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "activity-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "activity-test-device", __VA_ARGS__)

static const char *const command_names[] = {
    "APP_CMD_INPUT_CHANGED", "APP_CMD_INIT_WINDOW", "APP_CMD_TERM_WINDOW", "APP_CMD_WINDOW_RESIZED",
    "APP_CMD_WINDOW_REDRAW_NEEDED", "APP_CMD_CONTENT_RECT_CHANGED", "APP_CMD_GAINED_FOCUS", "APP_CMD_LOST_FOCUS",
    "APP_CMD_CONFIG_CHANGED", "APP_CMD_LOW_MEMORY", "APP_CMD_START", "APP_CMD_RESUME", "APP_CMD_SAVE_STATE",
    "APP_CMD_PAUSE", "APP_CMD_STOP", "APP_CMD_DESTROY",
};

struct state {
    int starts;
    int windows;
    int64_t finish_at;  // CLOCK_MONOTONIC in ns, 0 while no finish is due
    int finish_planned;
};

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

// FNV-1a, to show that two reads gave the same bytes.
static uint32_t hash(const char *data, size_t size) {
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < size; ++index) value = (value ^ (uint8_t)data[index]) * 16777619u;
    return value;
}

// A letter of a language or country code, '-' for none.
static char letter(char value) {
    return value != 0 ? value : '-';
}

static void log_folder(const char *what, const char *path) {
    struct stat status;
    const int folder = path != NULL && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
    LOG("%s: %s (%s)", what, path != NULL ? path : "NULL", folder ? "a folder" : "no folder");
}

static void check_activity(ANativeActivity *activity) {
    LOG("sdkVersion: %d", activity->sdkVersion);
    log_folder("internalDataPath", activity->internalDataPath);
    log_folder("externalDataPath", activity->externalDataPath);
    log_folder("obbPath", activity->obbPath);
    LOG("vm, env and clazz: %s, %s, %s", activity->vm != NULL ? "set" : "NULL", activity->env != NULL ? "set" : "NULL",
        activity->clazz != NULL ? "set" : "NULL");
}

// The JNI answers on the app's own thread, which Java never attached.
static void check_jni(JavaVM *vm) {
    JNIEnv *env = NULL, *attached = NULL;
    LOG("GetEnv on the app's thread: %d", (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6));
    LOG("AttachCurrentThread: %d", (*vm)->AttachCurrentThread(vm, &attached, NULL));
    const jint again = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    LOG("GetEnv again: %d, %s", again, env != NULL && env == attached ? "the attached env" : "another env");
    LOG("GetEnv for an unknown version: %d", (*vm)->GetEnv(vm, (void **)&env, 0x7fffffff));
    LOG("DetachCurrentThread: %d", (*vm)->DetachCurrentThread(vm));
    LOG("DetachCurrentThread again: %d", (*vm)->DetachCurrentThread(vm));
}

static void check_deflated(AAssetManager *assets) {
    AAsset *asset = AAssetManager_open(assets, "text/hello.txt", AASSET_MODE_STREAMING);
    if (asset == NULL) {
        LOG("text/hello.txt: NULL");
        return;
    }
    static char text[4096];
    const off_t length = AAsset_getLength(asset);
    const int first = AAsset_read(asset, text, 100);
    const off_t remaining = AAsset_getRemainingLength(asset);
    const int rest = first < 0 ? -1 : AAsset_read(asset, text + first, sizeof text - (size_t)first);
    const int end = AAsset_read(asset, text, 1);
    const size_t total = first > 0 && rest > 0 ? (size_t)first + (size_t)rest : 0;
    LOG("text/hello.txt: length %ld; read %d, %ld remaining, read %d, then %d; hash %08x", (long)length, first,
        (long)remaining, rest, end, hash(text, total));
    off64_t start = 0, size = 0;
    const int descriptor = AAsset_openFileDescriptor64(asset, &start, &size);
    LOG("text/hello.txt: descriptor %s", descriptor < 0 ? "refused" : "given");
    if (descriptor >= 0) close(descriptor);
    char line[64] = "";
    const off_t position = AAsset_seek(asset, 29, SEEK_SET);
    const int got = AAsset_read(asset, line, 29);  // the second line with its newline
    LOG("text/hello.txt: seek to %ld, then \"%.*s\"", (long)position, got > 0 ? got - 1 : 0, line);
    LOG("text/hello.txt: seek back 10 from the end: %ld, past the end: %ld", (long)AAsset_seek(asset, -10, SEEK_END),
        (long)AAsset_seek(asset, length + 1, SEEK_SET));
    AAsset_close(asset);
}

static void check_stored(AAssetManager *assets) {
    AAsset *asset = AAssetManager_open(assets, "stored.ogg", AASSET_MODE_RANDOM);
    if (asset == NULL) {
        LOG("stored.ogg: NULL");
        return;
    }
    char contents[256] = "";
    off64_t start = 0, size = 0;
    const int descriptor = AAsset_openFileDescriptor64(asset, &start, &size);
    ssize_t got = -1;
    if (descriptor >= 0 && size > 0 && size <= (off64_t)sizeof contents) {
        got = pread64(descriptor, contents, (size_t)size, start);
        close(descriptor);
    }
    LOG("stored.ogg: %ld bytes through a descriptor of the APK: \"%.*s\"", (long)size, got > 0 ? (int)got - 1 : 0,
        contents);
    const void *buffer = AAsset_getBuffer(asset);
    LOG("stored.ogg: the buffer %s", buffer != NULL && got > 0 && memcmp(buffer, contents, (size_t)got) == 0
                                         ? "has the same bytes" : "differs");
    AAsset_close(asset);
}

static void list_folder(AAssetManager *assets, const char *name) {
    AAssetDir *folder = AAssetManager_openDir(assets, name);
    if (folder == NULL) {
        LOG("folder \"%s\": NULL", name);
        return;
    }
    char line[512];
    int used = snprintf(line, sizeof line, "folder \"%s\":", name);
    int count = 0;
    for (const char *file; (file = AAssetDir_getNextFileName(folder)) != NULL; ++count) {
        if (used >= 0 && (size_t)used < sizeof line) used += snprintf(line + used, sizeof line - (size_t)used, " %s", file);
    }
    AAssetDir_rewind(folder);
    const char *first = AAssetDir_getNextFileName(folder);
    LOG("%s (%d files; after a rewind: %s)", line, count, first != NULL ? first : "nothing");
    AAssetDir_close(folder);
}

static void check_assets(AAssetManager *assets) {
    check_deflated(assets);
    check_stored(assets);
    LOG("missing.txt: %s", AAssetManager_open(assets, "missing.txt", AASSET_MODE_UNKNOWN) == NULL ? "NULL" : "found");
    list_folder(assets, "text");
    list_folder(assets, "");
    list_folder(assets, "missing");
}

static void log_configuration(AConfiguration *config) {
    LOG("configuration: sdkVersion %d", AConfiguration_getSdkVersion(config));
    char language[2], country[2];
    AConfiguration_getLanguage(config, language);
    AConfiguration_getCountry(config, country);
    DEVICE("configuration: mcc %d, mnc %d, locale %c%c-%c%c, density %d, orientation %d, touchscreen %d, keyboard %d, "
           "keys hidden %d, navigation %d, navigation hidden %d",
           AConfiguration_getMcc(config), AConfiguration_getMnc(config), letter(language[0]), letter(language[1]),
           letter(country[0]), letter(country[1]), AConfiguration_getDensity(config),
           AConfiguration_getOrientation(config), AConfiguration_getTouchscreen(config),
           AConfiguration_getKeyboard(config), AConfiguration_getKeysHidden(config),
           AConfiguration_getNavigation(config), AConfiguration_getNavHidden(config));
    DEVICE("configuration: screen size %d, long %d, round %d, %d x %d dp, smallest %d dp, layout direction %d, "
           "UI mode %d, night %d, grammatical gender %d",
           AConfiguration_getScreenSize(config), AConfiguration_getScreenLong(config),
           AConfiguration_getScreenRound(config), AConfiguration_getScreenWidthDp(config),
           AConfiguration_getScreenHeightDp(config), AConfiguration_getSmallestScreenWidthDp(config),
           AConfiguration_getLayoutDirection(config), AConfiguration_getUiModeType(config),
           AConfiguration_getUiModeNight(config), AConfiguration_getGrammaticalGender(config));
}

// The host window of the app's window, which only Minato's libnativewindow tells; Android has none.
static void log_host_window(int number, ANativeWindow *window) {
    typedef int (*get_info_function)(ANativeWindow *, struct minato_window_info *);
    static get_info_function get_info;
    static int looked;
    if (!looked) {
        looked = 1;
        void *library = dlopen("libnativewindow.so", RTLD_NOW);
        if (library != NULL) get_info = (get_info_function)dlsym(library, "minato_ANativeWindow_getInfo");
    }
    struct minato_window_info info;
    if (get_info != NULL && get_info(window, &info) == 0) {
        DEVICE("window %d: host window %llu", number, (unsigned long long)info.id);
    } else {
        DEVICE("window %d: no host window", number);
    }
}

static void on_command(struct android_app *app, int32_t command) {
    struct state *state = app->userData;
    if (command >= 0 && command < (int32_t)(sizeof command_names / sizeof command_names[0])) {
        LOG("%s", command_names[command]);
    } else {
        LOG("command %d", command);
    }
    switch (command) {
        case APP_CMD_START:
            if (++state->starts == 1) ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
            break;
        case APP_CMD_INIT_WINDOW:
            ++state->windows;
            LOG("window %d: format %d", state->windows, ANativeWindow_getFormat(app->window));
            DEVICE("window %d: %d x %d", state->windows, ANativeWindow_getWidth(app->window),
                   ANativeWindow_getHeight(app->window));
            log_host_window(state->windows, app->window);
            break;
        case APP_CMD_CONTENT_RECT_CHANGED:
            DEVICE("content rect: %d, %d, %d, %d", app->contentRect.left, app->contentRect.top, app->contentRect.right,
                   app->contentRect.bottom);
            break;
        case APP_CMD_GAINED_FOCUS:
            if (state->windows == 2 && !state->finish_planned) {
                state->finish_planned = 1;
                state->finish_at = now() + 1000000000;
            }
            break;
        case APP_CMD_SAVE_STATE:
            // A buffer the host frees, as NativeActivity does.
            app->savedState = malloc(6);
            if (app->savedState != NULL) {
                memcpy(app->savedState, "state", 6);
                app->savedStateSize = 6;
            }
            break;
        default:
            break;
    }
}

// For the Mac's scripted host only (runtime.activity.polling), never called on Android: onStart polls the
// main thread's looper first, as an app may do in a callback. The host's next event must still wait until
// this callback has returned, as Android's lifecycle messages wait for the one before.
static void (*glue_on_start)(ANativeActivity *);

static void polling_on_start(ANativeActivity *activity) {
    ALooper_pollOnce(0, NULL, NULL, NULL);
    glue_on_start(activity);
}

__attribute__((visibility("default"))) void activity_test_polling_onCreate(ANativeActivity *activity, void *state,
                                                                           size_t size) {
    ANativeActivity_onCreate(activity, state, size);
    glue_on_start = activity->callbacks->onStart;
    activity->callbacks->onStart = polling_on_start;
}

void android_main(struct android_app *app) {
    struct state state = {0};
    app->userData = &state;
    app->onAppCmd = on_command;
    LOG("android_main");
    check_activity(app->activity);
    check_jni(app->activity->vm);
    check_assets(app->activity->assetManager);
    log_configuration(app->config);

    while (!app->destroyRequested) {
        int timeout = -1;
        if (state.finish_at != 0) {
            const int64_t left = state.finish_at - now();
            timeout = left > 0 ? (int)((left + 999999) / 1000000) : 0;
        }
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(timeout, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        if (state.finish_at != 0 && now() >= state.finish_at) {
            state.finish_at = 0;
            LOG("ANativeActivity_finish");
            ANativeActivity_finish(app->activity);
        }
    }
    LOG("android_main returns");
}
