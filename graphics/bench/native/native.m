// MinatoBench (ADR-0037, decision 1): the parity benchmark's scene built for the host, the twin of bench-gles and
// bench-vulkan. The same scene files draw with ANGLE and MoltenVK directly: EGL through ANGLE's EGL_* entries with
// libEGL's Metal display and fixed-size surface (metal_display.h), Vulkan through MoltenVK's own vkGetInstanceProcAddr
// with libvulkan's pinned settings (moltenvk_settings.h). MoltenVK is opened by dlopen, since it has no Simulator
// slice (ADR-0030, decision 8). A render thread at QOS_CLASS_USER_INTERACTIVE (QOS_CLASS_DEFAULT with the launch
// argument -benchmarkRenderQoS default, ADR-0030 V5) draws a frame per tick that minato_bench_tick passes in, the
// latest it has not drawn, until bench.conf's seconds from the first frame's tick. With the launch argument
// -benchmarkQueuePath libvulkan, MoltenVK gets the calls with libvulkan's queue path (queue_path.h, ADR-0030 V5).
#define VK_NO_PROTOTYPES
#import <QuartzCore/QuartzCore.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../bench_gles.h"
#include "../bench_vulkan.h"
#include "../scene.h"
#include "metal_display.h"
#include "minato_bench.h"
#include "moltenvk_settings.h"
#include "queue_path.h"

#include <vulkan/vulkan_metal.h>

static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;  // a tick came, or the run ended
    int started, ended, status;
    char error[256];
    int64_t tick;
    uint64_t ticks;
    _Atomic uint64_t frames;
    enum bench_api api;
    const char *tag;
    struct bench_conf conf;
    void *layer;  // the CAMetalLayer, retained until the run ends
    EGLint width, height;
    void *moltenvk;
    int queue_path;  // libvulkan's in front of MoltenVK
    char scene_csv[1024];
} bench = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER, .tag = "bench"};

void bench_log(const char *format, ...) {
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    fprintf(stderr, "%s: %s\n", bench.tag, line);
}

static void end_run(int status, const char *error) {
    pthread_mutex_lock(&bench.lock);
    bench.ended = 1;
    bench.status = status;
    snprintf(bench.error, sizeof bench.error, "%s", error);
    pthread_cond_broadcast(&bench.changed);
    pthread_mutex_unlock(&bench.lock);
}

static VkResult metal_surface(void *context, VkInstance instance, PFN_vkGetInstanceProcAddr get,
                              VkSurfaceKHR *surface) {
    const PFN_vkCreateMetalSurfaceEXT create = (PFN_vkCreateMetalSurfaceEXT)get(instance, "vkCreateMetalSurfaceEXT");
    if (create == NULL) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkMetalSurfaceCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                                              .pLayer = (__bridge CAMetalLayer *)context};
    return create(instance, &info, NULL, surface);
}

static void *render(void *unused) {
    (void)unused;
    const int meshes = bench_meshes(bench.api, bench.conf.load);
    bench_log("run %s: %s at %d Hz for %d s, %d meshes", bench.conf.run, bench_load_name(bench.conf.load),
              bench.conf.rate, bench.conf.seconds, meshes);
    struct bench_frames frames = {0};
    bench_frames_reserve(&frames, bench.conf.seconds * bench.conf.rate + 64);
    struct bench_gles *gles = NULL;
    struct bench_vulkan *vulkan = NULL;
    if (bench.api == BENCH_GLES) {
        static const EGLAttrib metal[] = MINATO_EGL_METAL_DISPLAY;
        const EGLint surface[] = {MINATO_EGL_FIXED_SIZE(bench.width, bench.height), EGL_NONE};
        void *const display = (void *)(intptr_t)EGL_DEFAULT_DISPLAY;
        gles = bench_gles_start(eglGetPlatformDisplay(MINATO_EGL_PLATFORM_ANGLE_ANGLE, display, metal),
                                (EGLNativeWindowType)bench.layer, surface, meshes, bench.conf.check);
    } else {
        static const char *const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME,
                                                  VK_EXT_LAYER_SETTINGS_EXTENSION_NAME};
        const VkLayerSettingsCreateInfoEXT settings = {
            .sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
            .settingCount = sizeof minato_moltenvk_settings / sizeof *minato_moltenvk_settings,
            .pSettings = minato_moltenvk_settings};
        PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)dlsym(bench.moltenvk, "vkGetInstanceProcAddr");
        if (bench.queue_path) get = bench_queue_path(get);
        const struct bench_vulkan_platform platform = {get, extensions, 3, &settings, metal_surface, bench.layer};
        if (platform.get_instance_proc_addr != NULL) vulkan = bench_vulkan_start(&platform, meshes, bench.conf.check);
    }

    int frame = 0, failed = gles == NULL && vulkan == NULL;
    uint64_t seen = 0;
    int64_t first = 0;
    while (!failed) {
        pthread_mutex_lock(&bench.lock);
        while (bench.ticks == seen) pthread_cond_wait(&bench.changed, &bench.lock);
        seen = bench.ticks;
        const int64_t tick = bench.tick;
        pthread_mutex_unlock(&bench.lock);
        if (frame == 0) first = tick;
        if (tick - first >= (int64_t)bench.conf.seconds * 1000000000) break;
        const int64_t cpu = bench_cpu_ns();
        if (gles != NULL) {
            const EGLint result = bench_gles_frame(gles, frame);
            if (result != EGL_SUCCESS) bench_log("frame %d: 0x%x", frame, result);
            failed = result != EGL_SUCCESS;
        } else {
            const VkResult result = bench_vulkan_frame(vulkan, frame);
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) bench_log("frame %d: %d", frame, result);
            failed = result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR;
        }
        if (!failed) {
            bench_frames_add(&frames, tick, bench_cpu_ns() - cpu);
            atomic_store(&bench.frames, (uint64_t)++frame);
        }
    }
    bench_gles_stop(gles);
    bench_vulkan_stop(vulkan);
    const int written = !failed && bench_frames_write(&frames, bench.scene_csv) == 0;
    bench_log("run %s: %d frames%s", bench.conf.run, frame, written ? ", scene.csv written" : "");
    bench_frames_free(&frames);
    CFRelease(bench.layer);
    end_run(written ? 0 : -1, written ? "" : failed ? "the scene failed" : "scene.csv was not written");
    return NULL;
}

/* The value after `flag` in the process's launch arguments (ADR-0030 V5): its index in `values`, two of them, or
   1 without the flag; -1 with the reason for another value. */
static int launch_choice(NSString *flag, NSArray<NSString *> *values, char *error, size_t size) {
    @autoreleasepool {
        NSArray<NSString *> *arguments = NSProcessInfo.processInfo.arguments;
        const NSUInteger at = [arguments indexOfObject:flag];
        if (at == NSNotFound) return 1;
        NSString *value = at + 1 < arguments.count ? arguments[at + 1] : @"";
        const NSUInteger chosen = [values indexOfObject:value];
        if (chosen != NSNotFound) return (int)chosen;
        snprintf(error, size, "%s takes %s or %s, not %s", flag.UTF8String, values[0].UTF8String,
                 values[1].UTF8String, value.UTF8String);
        return -1;
    }
}

int minato_bench_start(const char *api, const char *conf, void *layer, const char *moltenvk, const char *scene_csv,
                       char *error, size_t size) {
    pthread_mutex_lock(&bench.lock);
    const int again = bench.started;
    bench.started = 1;
    pthread_mutex_unlock(&bench.lock);
    if (again) {
        snprintf(error, size, "one run per process");
        return -1;
    }
    const int vulkan = api != NULL && strcmp(api, "vulkan") == 0;
    if (api == NULL || (strcmp(api, "gles") != 0 && !vulkan) || layer == NULL || scene_csv == NULL ||
        strlen(scene_csv) >= sizeof bench.scene_csv) {
        snprintf(error, size, "a bad argument");
        end_run(-1, error);
        return -1;
    }
    bench_conf_defaults(&bench.conf);
    if (conf != NULL && bench_conf_parse(conf, &bench.conf, error, size) != 0) {
        end_run(-1, error);
        return -1;
    }
    // The render thread's QoS, default as the APK's guest render thread's or user-interactive, and the calls'
    // queue path, libvulkan's as the APK's or MoltenVK's own.
    const int qos_value = launch_choice(@"-benchmarkRenderQoS", @[ @"default", @"user-interactive" ], error, size);
    const int path_value =
        qos_value < 0 ? -1 : launch_choice(@"-benchmarkQueuePath", @[ @"libvulkan", @"moltenvk" ], error, size);
    if (path_value < 0) {
        end_run(-1, error);
        return -1;
    }
    const qos_class_t qos = qos_value == 0 ? QOS_CLASS_DEFAULT : QOS_CLASS_USER_INTERACTIVE;
    bench.queue_path = path_value == 0;
    bench.api = vulkan ? BENCH_VULKAN : BENCH_GLES;
    bench.tag = vulkan ? "bench-vulkan" : "bench-gles";
    // MoltenVK's log level as graphics/host's providers set it for the APKs: warnings, unless the environment
    // names one.
    if (vulkan) setenv("MVK_CONFIG_LOG_LEVEL", "2", 0);
    if (vulkan && (moltenvk == NULL || (bench.moltenvk = dlopen(moltenvk, RTLD_NOW | RTLD_LOCAL)) == NULL)) {
        snprintf(error, size, "no MoltenVK: %s", moltenvk == NULL ? "no path" : dlerror());
        end_run(-1, error);
        return -1;
    }
    CAMetalLayer *metal = (__bridge CAMetalLayer *)layer;
    CGSize pixels = metal.drawableSize;
    if (pixels.width < 1 || pixels.height < 1) {
        pixels = CGSizeMake(metal.bounds.size.width * metal.contentsScale,
                            metal.bounds.size.height * metal.contentsScale);
    }
    bench.width = (EGLint)pixels.width;
    bench.height = (EGLint)pixels.height;
    bench.layer = (__bridge_retained void *)metal;
    snprintf(bench.scene_csv, sizeof bench.scene_csv, "%s", scene_csv);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_set_qos_class_np(&attributes, qos, 0);
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, render, NULL);
    pthread_attr_destroy(&attributes);
    if (created != 0) {
        CFRelease(bench.layer);
        snprintf(error, size, "no render thread: %s", strerror(created));
        end_run(-1, error);
        return -1;
    }
    return 0;
}

void minato_bench_tick(int64_t time) {
    pthread_mutex_lock(&bench.lock);
    bench.tick = time;
    ++bench.ticks;
    pthread_cond_broadcast(&bench.changed);
    pthread_mutex_unlock(&bench.lock);
}

uint64_t minato_bench_frames(void) {
    return atomic_load(&bench.frames);
}

int minato_bench_wait(double seconds, char *error, size_t size) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    const double whole = seconds > 0 ? (double)(time_t)seconds : 0;
    deadline.tv_sec += (time_t)whole;
    deadline.tv_nsec += (long)((seconds > 0 ? seconds - whole : 0) * 1e9);
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&bench.lock);
    if (!bench.started) {
        pthread_mutex_unlock(&bench.lock);
        snprintf(error, size, "no run");
        return -1;
    }
    while (!bench.ended && seconds > 0 && pthread_cond_timedwait(&bench.changed, &bench.lock, &deadline) != ETIMEDOUT) {
    }
    const int result = !bench.ended ? 1 : bench.status == 0 ? 0 : -1;
    if (result == -1) snprintf(error, size, "%s", bench.error);
    pthread_mutex_unlock(&bench.lock);
    return result;
}

int minato_bench_meshes(const char *api, const char *load) {
    if (api == NULL || load == NULL) return 0;
    const int gles = strcmp(api, "gles") == 0, vulkan = strcmp(api, "vulkan") == 0;
    const int light = strcmp(load, "light") == 0, heavy = strcmp(load, "heavy") == 0;
    if ((!gles && !vulkan) || (!light && !heavy)) return 0;
    return bench_meshes(vulkan ? BENCH_VULKAN : BENCH_GLES, heavy ? BENCH_HEAVY : BENCH_LIGHT);
}
