// rotation-test (ADR-0038, test 3): a NativeActivity on native_app_glue whose activity handles a turn itself
// (configChanges "orientation|screenSize"). With the tag "rotation-test" it logs what Android 15 fixes: every
// command it gets, the configuration's orientation at the start, and whether its window and content rect are
// wider than high. Sizes, which differ by device, go to the tag "rotation-test-device", and so does the
// configuration after the start: at APP_CMD_CONFIG_CHANGED (the glue's) and at each redraw (read anew from the
// asset manager). Android 15's asset manager keeps the orientation at start through a turn the activity handles,
// so a NativeActivity learns of the turn from its window, not from its configuration. At the start, it also logs
// Minato's configuration of the session (minato_AConfiguration_fromSession, ADR-0039 config-test) and whether it is
// the activity's, to the device tag, as Android has no such function. It
// logs "ready" a second after it first gained the focus; the check then turns the device to landscape and
// back. A second after the second change of the configuration it calls ANativeActivity_finish.
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>

#include "APKRuntime/guest_interface.h"

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "rotation-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "rotation-test-device", __VA_ARGS__)

static const char *const command_names[] = {
    "APP_CMD_INPUT_CHANGED", "APP_CMD_INIT_WINDOW", "APP_CMD_TERM_WINDOW", "APP_CMD_WINDOW_RESIZED",
    "APP_CMD_WINDOW_REDRAW_NEEDED", "APP_CMD_CONTENT_RECT_CHANGED", "APP_CMD_GAINED_FOCUS", "APP_CMD_LOST_FOCUS",
    "APP_CMD_CONFIG_CHANGED", "APP_CMD_LOW_MEMORY", "APP_CMD_START", "APP_CMD_RESUME", "APP_CMD_SAVE_STATE",
    "APP_CMD_PAUSE", "APP_CMD_STOP", "APP_CMD_DESTROY",
};

struct state {
    int focused;
    int changes;
    int64_t ready_at;   // CLOCK_MONOTONIC in ns, 0 while "ready" is not due
    int64_t finish_at;  // CLOCK_MONOTONIC in ns, 0 while no finish is due
};

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static const char *shape(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return "empty";
    return width > height ? "landscape" : "portrait";
}

static const char *orientation_name(int32_t orientation) {
    switch (orientation) {
        case ACONFIGURATION_ORIENTATION_PORT: return "port";
        case ACONFIGURATION_ORIENTATION_LAND: return "land";
        case ACONFIGURATION_ORIENTATION_SQUARE: return "square";
        default: return "any";
    }
}

static void log_configuration(const char *when, AConfiguration *config) {
    DEVICE("%s: orientation %s, screen %d x %d dp", when, orientation_name(AConfiguration_getOrientation(config)),
           AConfiguration_getScreenWidthDp(config), AConfiguration_getScreenHeightDp(config));
}

// Minato's configuration of the session, and whether it is `activity`; "none" where libandroid lacks the function.
static void log_session_configuration(AConfiguration *activity) {
    void *android = dlopen("libandroid.so", RTLD_NOW);
    int (*from_session)(AConfiguration *, struct apkrt_configuration *) =
        android != NULL ? (int (*)(AConfiguration *, struct apkrt_configuration *))dlsym(
                              android, "minato_AConfiguration_fromSession")
                        : NULL;
    if (from_session == NULL) {
        DEVICE("session: none");
        return;
    }
    AConfiguration *config = AConfiguration_new();
    struct apkrt_configuration session;
    const int result = from_session(config, &session);
    DEVICE("session: %d, screen %d x %d px at scale %.1f, the activity's configuration: %s", result, session.width,
           session.height, session.scale, result == 0 && AConfiguration_diff(config, activity) == 0 ? "yes" : "no");
    AConfiguration_delete(config);
}

// The asset manager's configuration now.
static void log_current_configuration(const char *when, AAssetManager *assets) {
    AConfiguration *config = AConfiguration_new();
    AConfiguration_fromAssetManager(config, assets);
    log_configuration(when, config);
    AConfiguration_delete(config);
}

static void log_window(const char *when, ANativeWindow *window) {
    if (window == NULL) {
        LOG("%s: no window", when);
        return;
    }
    const int32_t width = ANativeWindow_getWidth(window), height = ANativeWindow_getHeight(window);
    LOG("%s: window %s", when, shape(width, height));
    DEVICE("%s: window %d x %d", when, width, height);
}

static void on_command(struct android_app *app, int32_t command) {
    struct state *state = app->userData;
    if (command >= 0 && command < (int32_t)(sizeof command_names / sizeof command_names[0])) {
        LOG("%s", command_names[command]);
    } else {
        LOG("command %d", command);
    }
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            log_window("init", app->window);
            break;
        case APP_CMD_WINDOW_RESIZED:
            log_window("resized", app->window);
            break;
        case APP_CMD_CONTENT_RECT_CHANGED: {
            const ARect rect = app->contentRect;
            LOG("content rect %s", shape(rect.right - rect.left, rect.bottom - rect.top));
            DEVICE("content rect: %d, %d, %d, %d", rect.left, rect.top, rect.right, rect.bottom);
            break;
        }
        case APP_CMD_WINDOW_REDRAW_NEEDED:
            log_current_configuration("redraw", app->activity->assetManager);
            break;
        case APP_CMD_CONFIG_CHANGED:
            // What the glue read from the asset manager before this callback.
            DEVICE("changed: orientation %s", orientation_name(AConfiguration_getOrientation(app->config)));
            if (++state->changes == 2) state->finish_at = now() + 1000000000;
            break;
        case APP_CMD_GAINED_FOCUS:
            if (!state->focused) {
                state->focused = 1;
                state->ready_at = now() + 1000000000;
            }
            break;
        default:
            break;
    }
}

void android_main(struct android_app *app) {
    struct state state = {0};
    app->userData = &state;
    app->onAppCmd = on_command;
    LOG("android_main");
    LOG("start: orientation %s", orientation_name(AConfiguration_getOrientation(app->config)));
    log_configuration("start", app->config);
    log_session_configuration(app->config);

    while (!app->destroyRequested) {
        int timeout = -1;
        const int64_t due = state.ready_at != 0 ? state.ready_at : state.finish_at;
        if (due != 0) {
            const int64_t left = due - now();
            timeout = left > 0 ? (int)((left + 999999) / 1000000) : 0;
        }
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(timeout, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        if (state.ready_at != 0 && now() >= state.ready_at) {
            state.ready_at = 0;
            LOG("ready");
        }
        if (state.finish_at != 0 && now() >= state.finish_at) {
            state.finish_at = 0;
            log_window("before the finish", app->window);
            LOG("ANativeActivity_finish");
            ANativeActivity_finish(app->activity);
        }
    }
    LOG("android_main returns");
}
