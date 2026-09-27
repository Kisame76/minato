// libandroid-test: what only Minato's libnativewindow.so and libandroid.so have (ADR-0028, decisions 7 and
// 9): the window as activity-host makes it, with minato_ANativeWindow_create, its getters before and
// after ANativeWindow_setBuffersGeometry, minato_ANativeWindow_getInfo for the front libs, what changes
// nothing and what answers an error, libandroid's forwards to libnativewindow, and the generated stubs,
// which log their name once. A test of the runtime: Android has none of it.
// Expected output: fixtures/expected/libandroid-test.out and .err
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/performance_hint.h>
#include <android/sensor.h>
#include <android/thermal.h>
#include <android/trace.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>

#include "APKRuntime/guest_interface.h"

static ANativeWindow *(*create)(uint64_t, int32_t, int32_t);
static int (*get_info)(ANativeWindow *, struct minato_window_info *);

static void print_info(const char *what, ANativeWindow *window) {
    struct minato_window_info info = {0};
    const int result = get_info(window, &info);
    printf("%s: %d", what, result);
    if (result == 0) {
        printf(", id %llu, size %d x %d, buffers %d x %d, format %d", (unsigned long long)info.id, info.width,
               info.height, info.buffer_width, info.buffer_height, info.buffer_format);
    }
    printf("\n");
}

static void print_getters(const char *what, ANativeWindow *window) {
    const int width = ANativeWindow_getWidth(window);
    const int height = ANativeWindow_getHeight(window);
    printf("%s: width %d, height %d, format %d\n", what, width, height, ANativeWindow_getFormat(window));
}

int main(void) {
    void *nativewindow = dlopen("libnativewindow.so", RTLD_NOW);
    create = (ANativeWindow * (*)(uint64_t, int32_t, int32_t)) dlsym(nativewindow, "minato_ANativeWindow_create");
    get_info = (int (*)(ANativeWindow *, struct minato_window_info *))dlsym(nativewindow, "minato_ANativeWindow_getInfo");
    if (create == NULL || get_info == NULL) {
        printf("the private functions are missing: %s\n", dlerror());
        return 1;
    }

    // A window and its buffer geometry.
    ANativeWindow *window = create(7, 1080, 2340);
    print_info("a new window", window);
    print_getters("  getters", window);
    printf("setBuffersGeometry(640, 360, RGBA_8888): %d\n",
           ANativeWindow_setBuffersGeometry(window, 640, 360, WINDOW_FORMAT_RGBA_8888));
    print_getters("  getters", window);
    print_info("  getInfo", window);
    printf("setBuffersGeometry(0, 0, RGBX_8888): %d\n", ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBX_8888));
    print_getters("  getters", window);
    printf("setBuffersGeometry(640, 0, RGBA_8888): %d\n",
           ANativeWindow_setBuffersGeometry(window, 640, 0, WINDOW_FORMAT_RGBA_8888));
    print_getters("  getters", window);
    print_info("  getInfo", window);
    printf("setBuffersGeometry(-1, 360, 0): %d\n", ANativeWindow_setBuffersGeometry(window, -1, 360, 0));
    print_getters("  getters", window);
    print_info("  getInfo", window);
    printf("setBuffersGeometry(0, 0, 0): %d\n", ANativeWindow_setBuffersGeometry(window, 0, 0, 0));
    print_getters("  getters", window);
    print_info("  getInfo", window);

    // References, and what is not a window.
    ANativeWindow_acquire(window);
    ANativeWindow_release(window);
    print_info("after acquire and release", window);
    int not_a_window[16] = {0};
    print_info("getInfo of NULL", NULL);
    print_info("getInfo of something else", (ANativeWindow *)not_a_window);
    print_getters("getters of something else", (ANativeWindow *)not_a_window);
    printf("setBuffersGeometry of something else: %d\n",
           ANativeWindow_setBuffersGeometry((ANativeWindow *)not_a_window, 0, 0, 0));

    // Accepted where Android accepts them (answers of an AImageReader's window in the emulator), and
    // changing nothing.
    const int8_t by_default = ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT;
    printf("setFrameRate: %d\n", ANativeWindow_setFrameRate(window, 60.0f, by_default));
    printf("setFrameRate(-1): %d\n", ANativeWindow_setFrameRate(window, -1.0f, by_default));
    printf("setFrameRate(nan): %d\n", ANativeWindow_setFrameRate(window, NAN, by_default));
    printf("setFrameRate(60, compatibility 7): %d\n", ANativeWindow_setFrameRate(window, 60.0f, 7));
    printf("setFrameRateWithChangeStrategy: %d\n",
           ANativeWindow_setFrameRateWithChangeStrategy(window, 60.0f, by_default,
                                                        ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS));
    printf("setFrameRateWithChangeStrategy(60, default, 9): %d\n",
           ANativeWindow_setFrameRateWithChangeStrategy(window, 60.0f, by_default, 9));
    printf("setBuffersTransform: %d\n", ANativeWindow_setBuffersTransform(window, ANATIVEWINDOW_TRANSFORM_ROTATE_90));
    printf("setBuffersTransform(0x100): %d\n", ANativeWindow_setBuffersTransform(window, 0x100));
    printf("setBuffersDataSpace: %d\n", ANativeWindow_setBuffersDataSpace(window, ADATASPACE_SRGB));
    printf("getBuffersDataSpace: %d\n", ANativeWindow_getBuffersDataSpace(window));
    printf("getBuffersDefaultDataSpace: %d\n", ANativeWindow_getBuffersDefaultDataSpace(window));
    ANativeWindow_tryAllocateBuffers(window);
    print_getters("  getters", window);

    // Drawing by the CPU and hardware buffers: errors, each logged once.
    ANativeWindow_Buffer buffer;
    printf("lock: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    printf("lock again: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    printf("unlockAndPost: %d\n", ANativeWindow_unlockAndPost(window));
    AHardwareBuffer_Desc description = {.width = 64, .height = 64, .layers = 1,
                                        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE};
    printf("AHardwareBuffer_isSupported: %d\n", AHardwareBuffer_isSupported(&description));
    AHardwareBuffer *hardware_buffer = NULL;
    printf("AHardwareBuffer_allocate: %d\n", AHardwareBuffer_allocate(&description, &hardware_buffer));

    // libandroid's ANativeWindow functions forward to libnativewindow's.
    void *android = dlopen("libandroid.so", RTLD_NOW);
    int32_t (*android_get_width)(ANativeWindow *) = (int32_t(*)(ANativeWindow *))dlsym(android, "ANativeWindow_getWidth");
    void (*android_acquire)(ANativeWindow *) = (void (*)(ANativeWindow *))dlsym(android, "ANativeWindow_acquire");
    void (*android_release)(ANativeWindow *) = (void (*)(ANativeWindow *))dlsym(android, "ANativeWindow_release");
    printf("libandroid's ANativeWindow_getWidth: %s\n",
           android_get_width == NULL ? "missing" : android_get_width == ANativeWindow_getWidth ? "libnativewindow's" : "its own");
    printf("  answers: %d\n", android_get_width(window));
    android_acquire(window);
    android_release(window);
    print_info("after libandroid's acquire and release", window);
    ANativeWindow_release(window);

    // Generated stubs: each logs once and answers its value.
    printf("AThermal_acquireManager: %s\n", AThermal_acquireManager() == NULL ? "null" : "a manager");
    printf("AThermal_acquireManager again: %s\n", AThermal_acquireManager() == NULL ? "null" : "a manager");
    // No manager exists; the stubs never look at it.
    AThermalManager *manager = (AThermalManager *)not_a_window;
    printf("AThermal_getCurrentThermalStatus: %d\n", AThermal_getCurrentThermalStatus(manager));
    printf("AThermal_getThermalHeadroom: %s\n", isnan(AThermal_getThermalHeadroom(manager, 10)) ? "nan" : "a number");
    printf("APerformanceHint_getManager: %s\n", APerformanceHint_getManager() == NULL ? "null" : "a manager");
    printf("ASensorManager_getInstanceForPackage: %s\n",
           ASensorManager_getInstanceForPackage("io.github.kisame76.minato") == NULL ? "null" : "a manager");
    printf("ASensor_getResolution: %g\n", ASensor_getResolution(NULL));
    printf("ATrace_isEnabled: %d\n", ATrace_isEnabled());
    ATrace_beginSection("minato");
    ATrace_endSection();
    printf("ATrace_beginSection and ATrace_endSection: done\n");
    return 0;
}
