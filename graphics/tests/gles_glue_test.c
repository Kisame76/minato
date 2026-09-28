/* Test 7 of ADR-0029, its second part (graphics.gles_glue): the order at background of decision 15 with
   native_app_glue's handshake, on ANGLE's macOS slice. main plays activity-host (ADR-0028): it reads the
   session's events and calls the NDK's native_app_glue for each, then answers it. The app on the glue's
   thread is the NDK sample's: EGL at APP_CMD_INIT_WINDOW, frames while it has the focus, one more frame at
   APP_CMD_LOST_FOCUS and no frame after it, the sample's teardown at APP_CMD_TERM_WINDOW. graphics-run's
   window host keeps the app's order (MINATO_WINDOW_SCRIPT=glue): the gate closed with the focus lost, then
   the background, "scheduled", the window gone, only then onPause and the rest, each answer timed. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

struct engine {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int animating;
};

static void init_display(struct engine *engine, ANativeWindow *window) {
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    engine->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(engine->display, NULL, NULL);
    eglChooseConfig(engine->display, attributes, &config, 1, &count);
    engine->surface = eglCreateWindowSurface(engine->display, config, window, NULL);
    engine->context = eglCreateContext(engine->display, config, EGL_NO_CONTEXT, version);
    const EGLBoolean current = eglMakeCurrent(engine->display, engine->surface, engine->surface, engine->context);
    printf("app: APP_CMD_INIT_WINDOW, EGL ready: %s\n",
           count == 1 && engine->surface != EGL_NO_SURFACE && current ? "yes" : "no");
}

static EGLBoolean draw_frame(struct engine *engine) {
    if (engine->display == EGL_NO_DISPLAY) {
        return EGL_FALSE;
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return eglSwapBuffers(engine->display, engine->surface);
}

/* The NDK sample's teardown: 1 once every call succeeded. */
static int term_display(struct engine *engine) {
    int all = 1;
    if (engine->display != EGL_NO_DISPLAY) {
        all &= eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        all &= eglDestroyContext(engine->display, engine->context);
        all &= eglDestroySurface(engine->display, engine->surface);
        all &= eglTerminate(engine->display);
    }
    engine->animating = 0;
    engine->display = EGL_NO_DISPLAY;
    engine->context = EGL_NO_CONTEXT;
    engine->surface = EGL_NO_SURFACE;
    return all;
}

static void handle_command(struct android_app *app, int32_t command) {
    struct engine *engine = app->userData;
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            init_display(engine, app->window);
            draw_frame(engine);
            break;
        case APP_CMD_TERM_WINDOW:
            printf("app: APP_CMD_TERM_WINDOW, the display terminated: %s\n", term_display(engine) ? "yes" : "no");
            break;
        case APP_CMD_GAINED_FOCUS:
            engine->animating = 1;
            printf("app: APP_CMD_GAINED_FOCUS\n");
            break;
        case APP_CMD_LOST_FOCUS: {
            engine->animating = 0;
            const EGLBoolean swapped = draw_frame(engine);
            const EGLint error = eglGetError();
            printf("app: APP_CMD_LOST_FOCUS, its frame: %s, %s\n", swapped ? "EGL_TRUE" : "EGL_FALSE",
                   error == EGL_BAD_NATIVE_WINDOW ? "EGL_BAD_NATIVE_WINDOW"
                   : error == EGL_SUCCESS         ? "EGL_SUCCESS"
                                                  : "another error");
            break;
        }
        case APP_CMD_START: printf("app: APP_CMD_START\n"); break;
        case APP_CMD_RESUME: printf("app: APP_CMD_RESUME\n"); break;
        case APP_CMD_PAUSE: printf("app: APP_CMD_PAUSE\n"); break;
        case APP_CMD_SAVE_STATE: printf("app: APP_CMD_SAVE_STATE\n"); break;
        case APP_CMD_STOP: printf("app: APP_CMD_STOP\n"); break;
        case APP_CMD_DESTROY: printf("app: APP_CMD_DESTROY\n"); break;
        default: break;
    }
}

void android_main(struct android_app *app) {
    struct engine engine = {EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT, 0};
    app->userData = &engine;
    app->onAppCmd = handle_command;
    for (;;) {
        int events;
        struct android_poll_source *source;
        while (ALooper_pollOnce(engine.animating ? 0 : -1, NULL, &events, (void **)&source) >= 0) {
            if (source != NULL) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                term_display(&engine);
                return;
            }
        }
        if (engine.animating) {
            draw_frame(&engine);
        }
    }
}

typedef ANativeWindow *(*create_window)(uint64_t, int32_t, int32_t);

/* activity-host's part: the session's events to the glue's callbacks, each answered once it returned. */
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct apkrt_configuration configuration;
    const long pipe = syscall(APKRT_NR_SESSION_CONNECT, &configuration);
    if (pipe < 0) {
        printf("activity: no session\n");
        return 1;
    }
    const create_window create =
        (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    ANativeActivityCallbacks callbacks = {0};
    ANativeActivity activity = {.callbacks = &callbacks, .internalDataPath = "/data/local/tmp",
                                .externalDataPath = "/data/local/tmp", .sdkVersion = 29};
    ANativeWindow *window = NULL;
    for (int destroyed = 0; !destroyed;) {
        struct apkrt_event event;
        if (read((int)pipe, &event, sizeof event) != sizeof event) {
            printf("activity: the event pipe broke\n");
            return 1;
        }
        switch (event.type) {
            case APKRT_EVENT_CREATE: ANativeActivity_onCreate(&activity, NULL, 0); break;
            case APKRT_EVENT_START: callbacks.onStart(&activity); break;
            case APKRT_EVENT_RESUME: callbacks.onResume(&activity); break;
            case APKRT_EVENT_PAUSE: callbacks.onPause(&activity); break;
            case APKRT_EVENT_STOP: callbacks.onStop(&activity); break;
            case APKRT_EVENT_WINDOW_FOCUS_CHANGED:
                callbacks.onWindowFocusChanged(&activity, (int)event.arguments[0]);
                break;
            case APKRT_EVENT_NATIVE_WINDOW_CREATED:
                window = create(event.arguments[0], (int32_t)event.arguments[1], (int32_t)event.arguments[2]);
                callbacks.onNativeWindowCreated(&activity, window);
                break;
            case APKRT_EVENT_NATIVE_WINDOW_DESTROYED:
                callbacks.onNativeWindowDestroyed(&activity, window);
                ANativeWindow_release(window);
                window = NULL;
                break;
            case APKRT_EVENT_SAVE_INSTANCE_STATE: {
                size_t size = 0;
                free(callbacks.onSaveInstanceState(&activity, &size));
                break;
            }
            case APKRT_EVENT_DESTROY:
                callbacks.onDestroy(&activity);
                destroyed = 1;
                break;
            default: break;
        }
        syscall(APKRT_NR_SESSION_ANSWER, event.serial);
    }
    return 0;
}
