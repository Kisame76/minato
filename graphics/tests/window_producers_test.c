// window-producers-test (the black windows' candidate (c), ADR-0043): one window, two producers. A window's
// BufferQueue takes one producer at a time; Android 15's answers are the reference. Each case has a window of its
// own, an AImageReader's (64 x 48 RGBA_8888, CPU and GPU usage), so that the same program runs on the emulator as a
// shell program and on Minato:
// - A: a CPU post (ANativeWindow_lock and unlockAndPost), then eglCreateWindowSurface on the same window while the CPU
//   producer is still connected: the surface or EGL_NO_SURFACE, and EGL's error;
// - B: an EGL window surface first, then ANativeWindow_lock (the CPU's connect) while EGL is connected, then the same
//   lock after eglDestroySurface (EGL's disconnect).
// Every library is opened with dlopen, so that one that is missing shows as such.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

typedef void *Display;
typedef void *Surface;
typedef void *Config;

struct ANativeWindow_Buffer {
    int32_t width, height, stride, format;
    void *bits;
    uint32_t reserved[6];
};

static int (*new_reader)(int32_t, int32_t, int32_t, uint64_t, int32_t, void **);
static int (*reader_window)(void *, void **);
static void (*delete_reader)(void *);
static int32_t (*window_lock)(void *, struct ANativeWindow_Buffer *, void *);
static int32_t (*window_post)(void *);
static Display (*get_display)(void *);
static unsigned (*initialize)(Display, int32_t *, int32_t *);
static unsigned (*choose_config)(Display, const int32_t *, Config *, int32_t, int32_t *);
static Surface (*create_window_surface)(Display, Config, void *, const int32_t *);
static int32_t (*get_error)(void);
static unsigned (*destroy_surface)(Display, Surface);

static void *window_of(void **reader) {
    void *window = NULL;
    // AIMAGE_FORMAT_RGBA_8888; CPU_READ_OFTEN | CPU_WRITE_OFTEN | GPU_COLOR_OUTPUT.
    if (new_reader(64, 48, 1, 0x3 | 0x30 | 0x200, 3, reader) != 0 || reader_window(*reader, &window) != 0) {
        return NULL;
    }
    return window;
}

static int32_t cpu_post(void *window, const char *what) {
    struct ANativeWindow_Buffer buffer;
    const int32_t locked = window_lock(window, &buffer, NULL);
    const int32_t posted = locked == 0 ? window_post(window) : -1;
    printf("%s: lock %d, post %d\n", what, locked, posted);
    return locked;
}

int main(void) {
    void *media = dlopen("libmediandk.so", RTLD_NOW), *native = dlopen("libnativewindow.so", RTLD_NOW);
    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (media == NULL || native == NULL || egl == NULL) {
        printf("absent: mediandk %d, nativewindow %d, EGL %d\n", media == NULL, native == NULL, egl == NULL);
        return 1;
    }
    *(void **)&new_reader = dlsym(media, "AImageReader_newWithUsage");
    *(void **)&reader_window = dlsym(media, "AImageReader_getWindow");
    *(void **)&delete_reader = dlsym(media, "AImageReader_delete");
    *(void **)&window_lock = dlsym(native, "ANativeWindow_lock");
    *(void **)&window_post = dlsym(native, "ANativeWindow_unlockAndPost");
    *(void **)&get_display = dlsym(egl, "eglGetDisplay");
    *(void **)&initialize = dlsym(egl, "eglInitialize");
    *(void **)&choose_config = dlsym(egl, "eglChooseConfig");
    *(void **)&create_window_surface = dlsym(egl, "eglCreateWindowSurface");
    *(void **)&get_error = dlsym(egl, "eglGetError");
    *(void **)&destroy_surface = dlsym(egl, "eglDestroySurface");
    const Display display = get_display(NULL);  // EGL_DEFAULT_DISPLAY
    initialize(display, NULL, NULL);
    // EGL_SURFACE_TYPE EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE EGL_OPENGL_ES2_BIT, EGL_RED_SIZE 8, ..., EGL_NONE.
    const int32_t wanted[] = {0x3033, 0x0004, 0x3040, 0x0004, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038};
    Config config = NULL;
    int32_t configs = 0;
    choose_config(display, wanted, &config, 1, &configs);
    printf("a config: %s\n", configs > 0 ? "yes" : "no");

    void *reader_a = NULL;
    void *window_a = window_of(&reader_a);
    printf("A: a window %s\n", window_a != NULL ? "yes" : "no");
    if (window_a != NULL) {
        cpu_post(window_a, "A: the cpu");
        const Surface surface = create_window_surface(display, config, window_a, NULL);
        const int32_t error = get_error();
        printf("A: eglCreateWindowSurface while the cpu is connected: %s, error 0x%x\n",
               surface == NULL ? "EGL_NO_SURFACE" : "a surface", error);
        if (surface != NULL) destroy_surface(display, surface);
        delete_reader(reader_a);
    }

    void *reader_b = NULL;
    void *window_b = window_of(&reader_b);
    printf("B: a window %s\n", window_b != NULL ? "yes" : "no");
    if (window_b != NULL) {
        const Surface surface = create_window_surface(display, config, window_b, NULL);
        printf("B: eglCreateWindowSurface first: %s, error 0x%x\n", surface == NULL ? "EGL_NO_SURFACE" : "a surface",
               get_error());
        cpu_post(window_b, "B: the cpu while EGL is connected");
        if (surface != NULL) printf("B: eglDestroySurface %u\n", destroy_surface(display, surface));
        cpu_post(window_b, "B: the cpu after EGL's disconnect");
        delete_reader(reader_b);
    }
    return 0;
}
