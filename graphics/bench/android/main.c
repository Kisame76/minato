// bench-gles and bench-vulkan (ADR-0037, decision 1): the parity benchmark's scene as NativeActivity apps on
// native_app_glue. One frame per AChoreographer callback, drawn from the frame index alone; the rate by
// ANativeWindow_setFrameRate and the screen kept on. The run's settings come from bench.conf in internalDataPath,
// else the emulator's defaults; after the run's seconds from its first frame's tick the app writes scene.csv beside
// bench.conf, logs its frames and ends with ANativeActivity_finish. Built twice from this file: BENCH_APK_VULKAN picks
// the API.
#include <android/choreographer.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>

#include "scene.h"

#ifdef BENCH_APK_VULKAN
#define VK_USE_PLATFORM_ANDROID_KHR
#include "bench_vulkan.h"
static const char tag[] = "bench-vulkan";
static const enum bench_api api = BENCH_VULKAN;
#else
#include "bench_gles.h"
static const char tag[] = "bench-gles";
static const enum bench_api api = BENCH_GLES;
#endif

void bench_log(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    __android_log_vprint(ANDROID_LOG_INFO, tag, format, arguments);
    va_end(arguments);
}

static struct {
    struct android_app *app;
    struct bench_conf conf;
    AChoreographer *choreographer;
    int posted;  // a frame callback waits for its tick
    int ended;   // the run is over, or never began
    int frame;   // the next frame's index
    int64_t first;
    int failures;
    struct bench_frames frames;
#ifdef BENCH_APK_VULKAN
    struct bench_vulkan *scene;
#else
    struct bench_gles *scene;
#endif
} run;

/* bench.conf in internalDataPath over the emulator's defaults: 0, or -1 when it holds what the scene does not take. */
static int read_conf(void) {
    bench_conf_defaults(&run.conf);
    char path[512];
    snprintf(path, sizeof path, "%s/bench.conf", run.app->activity->internalDataPath);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        bench_log("no bench.conf: the emulator's defaults");
        return 0;
    }
    char text[4096];
    const size_t length = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[length] = '\0';
    char error[256];
    if (bench_conf_parse(text, &run.conf, error, sizeof error) != 0) {
        bench_log("bench.conf: %s", error);
        return -1;
    }
    return 0;
}

#ifdef BENCH_APK_VULKAN
static VkResult android_surface(void *context, VkInstance instance, PFN_vkGetInstanceProcAddr get,
                                VkSurfaceKHR *surface) {
    const PFN_vkCreateAndroidSurfaceKHR create =
        (PFN_vkCreateAndroidSurfaceKHR)get(instance, "vkCreateAndroidSurfaceKHR");
    if (create == NULL) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkAndroidSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                .window = context};
    return create(instance, &info, NULL, surface);
}
#endif

static void start_scene(ANativeWindow *window) {
    const int meshes = bench_meshes(api, run.conf.load);
#ifdef BENCH_APK_VULKAN
    static const char *const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const struct bench_vulkan_platform platform = {vkGetInstanceProcAddr, extensions, 2, NULL, android_surface,
                                                   window};
    run.scene = bench_vulkan_start(&platform, meshes, run.conf.check, run.conf.scale);
#else
    run.scene = bench_gles_start(eglGetDisplay(EGL_DEFAULT_DISPLAY), window, NULL, meshes, run.conf.check,
                                  run.conf.scale);
#endif
}

static void stop_scene(void) {
#ifdef BENCH_APK_VULKAN
    bench_vulkan_stop(run.scene);
#else
    bench_gles_stop(run.scene);
#endif
    run.scene = NULL;
}

/* Frame `frame` drawn and presented: 0, or -1 with the error logged. */
static int draw(int frame) {
#ifdef BENCH_APK_VULKAN
    const VkResult result = bench_vulkan_frame(run.scene, frame);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) return 0;
    bench_log("frame %d: %d", frame, result);
#else
    const EGLint result = bench_gles_frame(run.scene, frame);
    if (result == EGL_SUCCESS) return 0;
    bench_log("frame %d: 0x%x", frame, result);
#endif
    return -1;
}

typedef int32_t (*set_frame_rate_function)(ANativeWindow *window, float rate, int8_t compatibility);

/* ANativeWindow_setFrameRate came with API 30, above the minSdk: from libnativewindow by dlsym. */
static void ask_rate(ANativeWindow *window) {
    void *library = dlopen("libnativewindow.so", RTLD_NOW);
    const set_frame_rate_function set_frame_rate =
        library != NULL ? (set_frame_rate_function)dlsym(library, "ANativeWindow_setFrameRate") : NULL;
    if (set_frame_rate == NULL) {
        bench_log("no ANativeWindow_setFrameRate");
        return;
    }
    bench_log("ANativeWindow_setFrameRate(%d): %d", run.conf.rate,
              set_frame_rate(window, (float)run.conf.rate, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT));
}

/* The run's end: scene.csv, the log line of its frames, and the app's finish. */
static void end_run(void) {
    run.ended = 1;
    char path[512];
    snprintf(path, sizeof path, "%s/scene.csv", run.app->activity->internalDataPath);
    const int written = bench_frames_write(&run.frames, path) == 0;
    bench_log("run %s: %d frames, %d failures%s", run.conf.run, run.frame, run.failures,
              written ? ", scene.csv written" : ", scene.csv not written");
    ANativeActivity_finish(run.app->activity);
}

static void on_frame(int64_t tick, void *data) {
    (void)data;
    run.posted = 0;
    if (run.ended || run.scene == NULL) return;
    if (run.frame == 0) run.first = tick;
    if (tick - run.first >= (int64_t)run.conf.seconds * 1000000000) {
        end_run();
        return;
    }
    AChoreographer_postFrameCallback64(run.choreographer, on_frame, NULL);
    run.posted = 1;
    const int64_t cpu = bench_cpu_ns();
    if (draw(run.frame) != 0) {
        ++run.failures;
        return;
    }
    bench_frames_add(&run.frames, tick, bench_cpu_ns() - cpu);
    ++run.frame;
}

static void on_command(struct android_app *app, int32_t command) {
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (run.ended || run.scene != NULL) break;
            start_scene(app->window);
            if (run.scene == NULL) {
                ++run.failures;
                end_run();
                break;
            }
            ask_rate(app->window);
            if (!run.posted) {
                AChoreographer_postFrameCallback64(run.choreographer, on_frame, NULL);
                run.posted = 1;
            }
            break;
        case APP_CMD_TERM_WINDOW:
            stop_scene();
            break;
        default:
            break;
    }
}

void android_main(struct android_app *app) {
    run.app = app;
    app->onAppCmd = on_command;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    run.choreographer = AChoreographer_getInstance();
    if (run.choreographer == NULL || read_conf() != 0) {
        run.ended = 1;
        ANativeActivity_finish(app->activity);
    } else {
        bench_frames_reserve(&run.frames, run.conf.seconds * run.conf.rate + 64);
        bench_log("run %s: %s at %d Hz for %d s, %d meshes", run.conf.run, bench_load_name(run.conf.load),
                  run.conf.rate, run.conf.seconds, bench_meshes(api, run.conf.load));
    }
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(-1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
    }
    stop_scene();
    bench_frames_free(&run.frames);
}
