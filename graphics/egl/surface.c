/* libEGL's window surfaces and swaps (ADR-0029, decisions 14 and 15):
   - eglCreateWindowSurface and eglCreatePlatformWindowSurface take the app's ANativeWindow; graphics/host
     turns its host window's ID into the window's CAMetalLayer. The surface is always of a fixed size
     (EGL_FIXED_SIZE_ANGLE, patch 0001): the buffer geometry of ANativeWindow_setBuffersGeometry, or the
     window's size in pixels while none is set. After every swap the swapping thread compares, and a new size
     takes effect with the next frame, as Android's next buffer does. The surface holds a reference to the
     window until eglDestroySurface or eglTerminate.
   - eglSwapBuffers and eglSwapBuffersWithDamageKHR, which ignores the damage, pass the window's gate first,
     outside ANGLE: open, ANGLE's swap; closed, glFlush commits the frame in flight, then the thread waits,
     holding none of ANGLE's locks, and a signal's handler runs meanwhile; gone, EGL_FALSE with
     EGL_BAD_NATIVE_WINDOW at once, without GPU work.
   A SurfaceTexture's queue window gets a surface of queue.c's instead (ADR-0058 c)). A window surface is its host
   window's producer from its making to its destroy (ADR-0060 e2c3), as Android's connects: a host window a decoder
   draws into gets none (EGL_BAD_NATIVE_WINDOW where Android answers EGL_BAD_ALLOC), and a decoder's claim of a
   window with one is refused. */
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "libegl.h"

MINATO_HOST(eglCreateWindowSurface);
MINATO_HOST(eglCreatePlatformWindowSurface);
MINATO_HOST(eglDestroySurface);
MINATO_HOST(eglSwapBuffers);

/* libnativewindow's private function (ADR-0028, decision 7), which its NDK stub lacks: found once. */
static int (*get_info)(struct ANativeWindow *, struct minato_window_info *);
static pthread_once_t found = PTHREAD_ONCE_INIT;

static void find_info(void) {
    void *library = dlopen("libnativewindow.so", RTLD_NOW);
    if (library != NULL) {
        get_info = (int (*)(struct ANativeWindow *, struct minato_window_info *))dlsym(library,
                                                                                    "minato_ANativeWindow_getInfo");
    }
}

static int window_info(struct ANativeWindow *window, struct minato_window_info *info) {
    pthread_once(&found, find_info);
    return get_info != NULL && get_info(window, info) == 0 ? 0 : -1;
}

/* The size of a surface on the window: its buffer geometry once set, else its size in pixels. */
static void surface_size(const struct minato_window_info *info, EGLint *width, EGLint *height) {
    const int geometry = info->buffer_width > 0 && info->buffer_height > 0;
    *width = geometry ? info->buffer_width : info->width;
    *height = geometry ? info->buffer_height : info->height;
}

/* The window surfaces libEGL made: few in any app, a list under a lock. */
struct window_surface {
    struct window_surface *next;
    EGLSurface surface;
    EGLDisplay display;
    EGLConfig config;
    struct ANativeWindow *window;
    uint64_t id;
    EGLint width, height;
};
static struct window_surface *surfaces;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void minato_bad_native_window(EGLDisplay display, EGLConfig config) {
    minato_host_eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
}

/* The app's attributes without the fixed size's, then the fixed size: for EGLint and EGLAttrib lists. */
#define FIXED(name, type)                                                                                   \
    static type *name(const type *attributes, EGLint width, EGLint height) {                               \
        size_t length = 0;                                                                                  \
        while (attributes != NULL && attributes[length] != EGL_NONE) {                                      \
            length += 2;                                                                                    \
        }                                                                                                   \
        type *ours = malloc((length + 7) * sizeof *ours);                                                   \
        size_t kept = 0;                                                                                    \
        for (size_t index = 0; ours != NULL && index < length; index += 2) {                                \
            if (attributes[index] != MINATO_EGL_FIXED_SIZE_ANGLE && attributes[index] != EGL_WIDTH &&       \
                attributes[index] != EGL_HEIGHT) {                                                          \
                ours[kept++] = attributes[index];                                                           \
                ours[kept++] = attributes[index + 1];                                                       \
            }                                                                                               \
        }                                                                                                   \
        if (ours != NULL) {                                                                                 \
            const type fixed[] = {MINATO_EGL_FIXED_SIZE(width, height), EGL_NONE};                          \
            for (size_t index = 0; index < sizeof fixed / sizeof *fixed; index++) {                         \
                ours[kept++] = fixed[index];                                                                \
            }                                                                                               \
        }                                                                                                   \
        return ours;                                                                                        \
    }
FIXED(fixed_attributes, EGLint)
FIXED(fixed_attribs, EGLAttrib)
#undef FIXED

/* The record of a window surface, made before ANGLE's surface; NULL without memory or for no window. */
static struct window_surface *prepared(EGLDisplay display, EGLConfig config, struct ANativeWindow *window) {
    struct minato_window_info info;
    if (window_info(window, &info) != 0) {
        return NULL;
    }
    struct window_surface *record = calloc(1, sizeof *record);
    if (record != NULL) {
        *record = (struct window_surface){NULL, EGL_NO_SURFACE, display, config, window, info.id, 0, 0};
        surface_size(&info, &record->width, &record->height);
    }
    return record;
}

/* The surface ANGLE made, or none: then the record goes and the window's producer disconnects. */
static EGLSurface kept(struct window_surface *record, EGLSurface surface) {
    if (surface == EGL_NO_SURFACE) {
        minato_window_disconnect(record->window);
        free(record);
        return surface;
    }
    ANativeWindow_acquire(record->window);
    record->surface = surface;
    pthread_mutex_lock(&lock);
    record->next = surfaces;
    surfaces = record;
    pthread_mutex_unlock(&lock);
    return surface;
}

EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config, EGLNativeWindowType window,
                                  const EGLint *attributes) {
    if (minato_queue_window(window)) {
        return minato_queue_surface(display, config, window);
    }
    if (minato_window_connect(window) != 0) {
        minato_bad_native_window(display, config);  /* a decoder's: Android's connect fails, "already connected" */
        return EGL_NO_SURFACE;
    }
    struct window_surface *record = prepared(display, config, window);
    EGLint *ours = record != NULL ? fixed_attributes(attributes, record->width, record->height) : NULL;
    if (ours == NULL) {
        free(record);
        minato_window_disconnect(window);
        minato_bad_native_window(display, config);
        return EGL_NO_SURFACE;
    }
    const EGLSurface surface =
        minato_host_eglCreateWindowSurface(display, config, (EGLNativeWindowType)(uintptr_t)record->id, ours);
    free(ours);
    return kept(record, surface);
}

EGLSurface eglCreatePlatformWindowSurface(EGLDisplay display, EGLConfig config, void *window,
                                          const EGLAttrib *attributes) {
    if (minato_queue_window(window)) {
        return minato_queue_surface(display, config, window);
    }
    if (minato_window_connect(window) != 0) {
        minato_bad_native_window(display, config);  /* a decoder's: Android's connect fails, "already connected" */
        return EGL_NO_SURFACE;
    }
    struct window_surface *record = prepared(display, config, window);
    EGLAttrib *ours = record != NULL ? fixed_attribs(attributes, record->width, record->height) : NULL;
    if (ours == NULL) {
        free(record);
        minato_window_disconnect(window);
        minato_bad_native_window(display, config);
        return EGL_NO_SURFACE;
    }
    const EGLSurface surface =
        minato_host_eglCreatePlatformWindowSurface(display, config, (void *)(uintptr_t)record->id, ours);
    free(ours);
    return kept(record, surface);
}

/* Takes the records of a surface (or of every surface of a display) out of the list and lets their windows go. */
static void release(EGLDisplay display, EGLSurface surface) {
    struct window_surface *gone = NULL;
    pthread_mutex_lock(&lock);
    for (struct window_surface **entry = &surfaces; *entry != NULL;) {
        if ((*entry)->display == display && (surface == EGL_NO_SURFACE || (*entry)->surface == surface)) {
            struct window_surface *record = *entry;
            *entry = record->next;
            record->next = gone;
            gone = record;
        } else {
            entry = &(*entry)->next;
        }
    }
    pthread_mutex_unlock(&lock);
    while (gone != NULL) {
        struct window_surface *next = gone->next;
        minato_window_disconnect(gone->window);
        ANativeWindow_release(gone->window);
        free(gone);
        gone = next;
    }
}

EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
    const EGLBoolean result = minato_host_eglDestroySurface(display, surface);
    if (result && surface != EGL_NO_SURFACE) {
        release(display, surface);
        minato_queue_destroyed(display, surface);
    }
    return result;
}

void minato_release_windows(EGLDisplay display) {
    release(display, EGL_NO_SURFACE);
    minato_queue_terminated(display);
}

/* A copy of a window surface's record; 0 for any other surface. */
static int recall(EGLSurface surface, struct window_surface *copy) {
    int found = 0;
    pthread_mutex_lock(&lock);
    for (const struct window_surface *entry = surfaces; entry != NULL; entry = entry->next) {
        if (entry->surface == surface) {
            *copy = *entry;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return found;
}

static long gate(uint64_t window, int kind) {
    long answer;
    do {
        answer = syscall(APKRT_NR_WINDOW_GATE, window, kind);
    } while (answer == -1 && errno == EINTR);
    return answer;
}

/* The window's gate (decision 15): 1 once the window is gone, else 0. Closed, the frame in flight is
   committed first (ADR-0032: the next present waits after it), then the thread waits until it opens. */
static int gone(uint64_t window) {
    const long now = gate(window, APKRT_GATE_PRESENT_TRY);
    if (now == 0 || now == 1) {
        return (int)now;
    }
    if (now == 2) {
        void (*flush)(void) = (void (*)(void))minato_gles_function("glFlush");
        if (flush != NULL) {
            flush();
        }
    }
    return gate(window, APKRT_GATE_PRESENT) == 1;
}

/* After a swap: a new size of the window's buffers becomes the surface's, for the next frame. */
static void follow(EGLDisplay display, EGLSurface surface, const struct window_surface *record) {
    struct minato_window_info info;
    EGLint width, height;
    if (window_info(record->window, &info) != 0) {
        return;
    }
    surface_size(&info, &width, &height);
    if ((width == record->width && height == record->height) || !eglSurfaceAttrib(display, surface, EGL_WIDTH, width) ||
        !eglSurfaceAttrib(display, surface, EGL_HEIGHT, height)) {
        return;
    }
    pthread_mutex_lock(&lock);
    for (struct window_surface *entry = surfaces; entry != NULL; entry = entry->next) {
        if (entry->surface == surface) {
            entry->width = width;
            entry->height = height;
        }
    }
    pthread_mutex_unlock(&lock);
}

static EGLBoolean swap(EGLDisplay display, EGLSurface surface) {
    struct window_surface record;
    EGLBoolean queued;
    if (minato_queue_swap(display, surface, &queued)) {
        return queued;
    }
    if (!recall(surface, &record)) {
        return minato_host_eglSwapBuffers(display, surface);
    }
    if (gone(record.id)) {
        minato_bad_native_window(display, record.config);
        return EGL_FALSE;
    }
    const EGLBoolean result = minato_host_eglSwapBuffers(display, surface);
    if (result) {
        follow(display, surface, &record);
    }
    return result;
}

EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    return swap(display, surface);
}

EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay display, EGLSurface surface, const EGLint *rectangles,
                                       EGLint count) {
    (void)rectangles, (void)count;
    return swap(display, surface);
}
