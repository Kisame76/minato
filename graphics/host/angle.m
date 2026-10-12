/* graphics/host's functions in front of ANGLE's for libEGL and libGLES (ADR-0029, decisions 14 to 16); see
   minato_graphics.h. The "egl" and "gles" providers put them into their slots, so they run as host code on
   the guest threads that call the front libs:
   - window surfaces: libEGL names the host window by its ID, which becomes the window's CAMetalLayer for
     ANGLE (none for the ID 0, which ANGLE answers with EGL_BAD_NATIVE_WINDOW); Core Animation gets ANGLE's
     changes to the layer, made on a thread without a run loop, when a surface is made or destroyed; the
     presents are counted per window for the FPS label;
   - presentation times (EGL_ANDROID_presentation_time, patch 0005): ANGLE gets the time asked for on the display's
     refresh grid, less half a period (present_time.h, ADR-0041 decision 11). Each swap leaves the time asked for it,
     or none, on the window's layer before ANGLE's swap and takes it back when the swap fails (present_pace.h,
     decision 11 c));
   - the debug callback: ANGLE gets graphics/host's function with a record of the app's function and user
     data, which calls the app's function through apkrt_call_guest on guest threads only and logs the
     message on any other thread (decision 16, ADR-0030's decision 5); glGetPointerv answers from the record;
   - ANGLE's commit gate (patch 0003): a commit counts itself, then reads the background flag and backs out
     and waits for the foreground if it is set; the count drops once ANGLE counted the command buffer. The
     app sets the flag, then "scheduled" waits until the count is 0 and then until ANGLE's committed command
     buffers are scheduled: either the commit sees the flag or "scheduled" sees the commit (decision 15);
   - EGL images of the guest's hardware buffers (ADR-0058 b), image.m);
   - the ES 3.1 path (ADR-0044, decision 4): when the session chose ANGLE's Vulkan backend, ANGLE gets
     graphics/host's vkGetInstanceProcAddr (patch 0009, angle_vulkan.m), and libEGL's display, which asks for
     ANGLE's Metal display, becomes ANGLE's Vulkan display.
   Manual reference counting, as vulkan.m. ANGLE's C types are written out here, as ANGLE's headers are not
   among graphics/host's. */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "angle_host.h"
#include "egl_table.h"
#include "gles_table.h"
#include "present_pace.h"
#include "present_time.h"

/* EGL's and GLES's types and names as ANGLE has them. */
typedef unsigned int egl_boolean;
#define EGL_TRUE 1u
typedef int32_t egl_int;
typedef intptr_t egl_attrib;
typedef unsigned int gl_enum;
typedef unsigned int egl_enum;
#define EGL_NONE 0x3038
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE 0x3450
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_FEATURE_OVERRIDES_DISABLED_ANGLE 0x3467
#define GL_DEBUG_CALLBACK_FUNCTION 0x8244
#define GL_DEBUG_CALLBACK_USER_PARAM 0x8245

typedef void *(*create_window_surface_function)(void *display, void *config, void *layer, const egl_int *attributes);
typedef void *(*create_platform_window_surface_function)(void *display, void *config, void *layer,
                                                          const egl_attrib *attributes);
typedef egl_boolean (*destroy_surface_function)(void *display, void *surface);
typedef egl_boolean (*swap_buffers_function)(void *display, void *surface);
typedef egl_boolean (*presentation_time_function)(void *display, void *surface, int64_t time);
typedef egl_boolean (*terminate_function)(void *display);
typedef void *(*get_platform_display_function)(egl_enum platform, void *native, const egl_attrib *attributes);
typedef void (*debug_function)(unsigned source, unsigned type, unsigned id, unsigned severity, int length,
                               const char *text, const void *user);
typedef void (*debug_callback_function)(debug_function callback, const void *user);
typedef void (*get_pointerv_function)(gl_enum pname, void **params);

/* ANGLE's functions that graphics/host's stand in front of, and the one they use. */
static create_window_surface_function create_window_surface;
static create_platform_window_surface_function create_platform_window_surface;
static destroy_surface_function destroy_surface;
static swap_buffers_function swap_buffers;
static presentation_time_function presentation_time;
static terminate_function terminate;
static get_platform_display_function get_platform_display;
static debug_callback_function debug_callback, debug_callback_khr;
static get_pointerv_function get_pointerv, get_pointerv_khr;

/* The window surfaces ANGLE made on a window's layer, with the window's ID, its layer (unretained: ANGLE's surface
   retains it) and the time the app asked for the next present, 0 for none: few, a list under a lock. */
struct surface {
    struct surface *next;
    void *display, *surface;
    uint64_t window;
    void *layer;
    int64_t asked;
};
static struct surface *surfaces;
static pthread_mutex_t surfaces_lock = PTHREAD_MUTEX_INITIALIZER;

static void surface_keep(void *display, void *surface, uint64_t window, void *layer) {
    struct surface *entry = malloc(sizeof *entry);
    if (entry == NULL) {
        return;  /* its presents go uncounted and leave no time */
    }
    *entry = (struct surface){NULL, display, surface, window, layer, 0};
    pthread_mutex_lock(&surfaces_lock);
    entry->next = surfaces;
    surfaces = entry;
    pthread_mutex_unlock(&surfaces_lock);
}

/* The layers of a surface (or of every surface of a display) forget their presents' records, while ANGLE's surfaces
   still hold them, before they go. */
static void surfaces_leave(void *display, void *surface) {
    pthread_mutex_lock(&surfaces_lock);
    for (const struct surface *entry = surfaces; entry != NULL; entry = entry->next) {
        if (entry->display == display && (surface == NULL || entry->surface == surface)) {
            minato_present_forget(entry->layer);
        }
    }
    pthread_mutex_unlock(&surfaces_lock);
}

/* Takes a surface (or every surface of a display) out of the list. */
static void surfaces_forget(void *display, void *surface) {
    pthread_mutex_lock(&surfaces_lock);
    for (struct surface **entry = &surfaces; *entry != NULL;) {
        if ((*entry)->display == display && (surface == NULL || (*entry)->surface == surface)) {
            struct surface *gone = *entry;
            *entry = gone->next;
            free(gone);
        } else {
            entry = &(*entry)->next;
        }
    }
    pthread_mutex_unlock(&surfaces_lock);
}

/* The surface's window, 0 for one not in the list, with its layer and the time asked for its next present. */
static uint64_t window_of(void *surface, void **layer, int64_t *asked) {
    uint64_t window = 0;
    pthread_mutex_lock(&surfaces_lock);
    for (const struct surface *entry = surfaces; entry != NULL; entry = entry->next) {
        if (entry->surface == surface) {
            window = entry->window;
            *layer = entry->layer;
            *asked = entry->asked;
            break;
        }
    }
    pthread_mutex_unlock(&surfaces_lock);
    return window;
}

/* Sets the time the app asked for the surface's next present, 0 for none. */
static void surface_asked(void *surface, int64_t asked) {
    pthread_mutex_lock(&surfaces_lock);
    for (struct surface *entry = surfaces; entry != NULL; entry = entry->next) {
        if (entry->surface == surface) {
            entry->asked = asked;
            break;
        }
    }
    pthread_mutex_unlock(&surfaces_lock);
}

/* The window's layer, retained for this call (ANGLE's surface retains it for its own life); nil for the ID 0
   and for a window that is gone. */
static CAMetalLayer *layer_of(uint64_t window) {
    return window != 0 ? (CAMetalLayer *)apkrt_window_layer(window) : nil;
}

static void *window_surface_created(void *display, void *config, void *window, const egl_int *attributes) {
    const uint64_t id = (uint64_t)(uintptr_t)window;
    CAMetalLayer *layer = layer_of(id);
    void *const surface = create_window_surface(display, config, layer, attributes);
    if (surface != NULL) {
        surface_keep(display, surface, id, layer);
        [CATransaction flush];
    }
    [layer release];
    return surface;
}

static void *platform_window_surface_created(void *display, void *config, void *window, const egl_attrib *attributes) {
    const uint64_t id = (uint64_t)(uintptr_t)window;
    CAMetalLayer *layer = layer_of(id);
    void *const surface = create_platform_window_surface(display, config, layer, attributes);
    if (surface != NULL) {
        surface_keep(display, surface, id, layer);
        [CATransaction flush];
    }
    [layer release];
    return surface;
}

static egl_boolean surface_destroyed(void *display, void *surface) {
    surfaces_leave(display, surface);
    const egl_boolean result = destroy_surface(display, surface);
    if (result) {
        surfaces_forget(display, surface);
        [CATransaction flush];
    }
    return result;
}

static egl_boolean terminated(void *display) {
    surfaces_leave(display, NULL);
    const egl_boolean result = terminate(display);
    if (result) {
        surfaces_forget(display, NULL);
        minato_graphics_images_terminated(display);
    }
    return result;
}

/* The present leaves the time asked for it on the layer before ANGLE's swap, as its drawable's present may come before
   the swap returns, and takes it back when the swap fails; once it succeeded, the time is spent, and the present counts
   for the FPS label. */
static egl_boolean swapped(void *display, void *surface) {
    void *layer = NULL;
    int64_t asked = 0;
    const uint64_t window = window_of(surface, &layer, &asked);
    const struct minato_present_record record = {.asked = asked, .ruled = 1};
    const uint64_t ticket = layer != NULL ? minato_present_push(layer, &record) : 0;
    const egl_boolean result = swap_buffers(display, surface);
    if (!result) {
        minato_present_take_back(layer, ticket);
    } else if (window != 0) {
        surface_asked(surface, 0);
        apkrt_window_presented(window);
    }
    return result;
}

/* The time of a window surface's next present, for Core Animation; none for a present without a time (decision 11
   b)), which ANGLE's swap then presents as it would without the call. The surface keeps the time asked for until a swap
   succeeds; one of 0 or less is none (decision 11 c)). */
static egl_boolean presentation_time_set(void *display, void *surface, int64_t time) {
    surface_asked(surface, time > 0 ? time : 0);
    int64_t ours = 0;
    if (minato_present_time(time, &ours) == MINATO_PRESENT_WITHOUT_TIME) {
        return EGL_TRUE;
    }
    return presentation_time(display, surface, ours);
}

/* The app's debug callback: the user data graphics/host's function gets from ANGLE. */
struct callback {
    uint64_t magic;
    uint64_t function, user;
};
#define CALLBACK_MAGIC 0x6d696e61746f4342ull

static const struct callback *callback_of(const void *user) {
    const struct callback *callback = user;
    return callback != NULL && callback->magic == CALLBACK_MAGIC ? callback : NULL;
}

static void message(unsigned source, unsigned type, unsigned id, unsigned severity, int length, const char *text,
                    const void *user) {
    if (!apkrt_is_guest_thread()) {
        fprintf(stderr, "minato graphics: a GL debug message on a host thread, logged instead of calling the app: %s\n",
                text != NULL ? text : "(none)");
        return;
    }
    const struct callback *callback = callback_of(user);
    if (callback != NULL && callback->function != 0) {
        apkrt_call_guest(callback->function, source, type, id, severity, (uint64_t)(int64_t)length,
                         (uint64_t)(uintptr_t)text, callback->user, 0);
    }
}

/* Registers graphics/host's function (none for none) with a new record, which ANGLE's user data then names;
   the one it named before goes, and a record ANGLE refused (glDebugMessageCallback in an ES 3.0 context) too.
   ANGLE calls the callback only on the thread that uses its context, which is this one. */
static void callback_set(debug_callback_function set, uint64_t function, const void *user) {
    void *before = NULL, *after = NULL;
    get_pointerv_khr(GL_DEBUG_CALLBACK_USER_PARAM, &before);
    struct callback *record = malloc(sizeof *record);
    if (record == NULL) {
        return;
    }
    *record = (struct callback){CALLBACK_MAGIC, function, (uint64_t)(uintptr_t)user};
    set(function != 0 ? message : NULL, record);
    get_pointerv_khr(GL_DEBUG_CALLBACK_USER_PARAM, &after);
    if (after != record) {
        free(record);
    } else if (callback_of(before) != NULL) {
        free(before);
    }
}

static void callback_given(debug_function function, const void *user) {
    callback_set(debug_callback, (uint64_t)(uintptr_t)function, user);
}

static void callback_given_khr(debug_function function, const void *user) {
    callback_set(debug_callback_khr, (uint64_t)(uintptr_t)function, user);
}

/* ANGLE answers first, with its errors; a debug answer it gave becomes the app's function or user data. */
static void pointer_asked(get_pointerv_function get, gl_enum pname, void **params) {
    const int debug = (pname == GL_DEBUG_CALLBACK_FUNCTION || pname == GL_DEBUG_CALLBACK_USER_PARAM) && params != NULL;
    void *const before = debug ? *params : NULL;
    if (debug) {
        *params = (void *)pointer_asked;  /* never an answer of ANGLE's */
    }
    get(pname, params);
    if (!debug) {
        return;
    }
    if (*params == (void *)pointer_asked) {
        *params = before;
        return;
    }
    void *user = NULL;
    get_pointerv_khr(GL_DEBUG_CALLBACK_USER_PARAM, &user);
    const struct callback *callback = callback_of(user);
    if (callback != NULL) {
        *params = (void *)(uintptr_t)(pname == GL_DEBUG_CALLBACK_FUNCTION ? callback->function : callback->user);
    }
}

static void pointer_given(gl_enum pname, void **params) {
    pointer_asked(get_pointerv, pname, params);
}

static void pointer_given_khr(gl_enum pname, void **params) {
    pointer_asked(get_pointerv_khr, pname, params);
}

/* ANGLE's commits between its gate and its count (patch 0003), and its wait until they are scheduled. */
static atomic_uint committing;
/* The ES 3.1 path (ADR-0044, decision 4): libEGL asks for ANGLE's Metal display, which becomes its Vulkan display once
   the session chose that backend and ANGLE took graphics/host's vkGetInstanceProcAddr (patch 0009). That display goes
   without ANGLE's usePrimitiveTopologyDynamicState (EGL_ANGLE_feature_control): MoltenVK draws a triangle fan as
   triangles only for a pipeline made with that topology (MVKCmdDraw.mm), and with the topology as dynamic state it
   draws a fan's vertices as a list of triangles. */
static atomic_int vulkan_ready;
static const char *const vulkan_features_off[] = {"usePrimitiveTopologyDynamicState", NULL};

static void *platform_display_asked(egl_enum platform, void *native, const egl_attrib *attributes) {
    if (platform != EGL_PLATFORM_ANGLE_ANGLE || attributes == NULL || !atomic_load(&vulkan_ready) ||
        !minato_graphics_angle_vulkan_backend()) {
        return get_platform_display(platform, native, attributes);
    }
    size_t length = 0;
    while (attributes[length] != EGL_NONE) {
        length += 2;
    }
    egl_attrib ours[length + 3];
    for (size_t index = 0; index < length; index += 2) {
        ours[index] = attributes[index];
        ours[index + 1] = attributes[index] == EGL_PLATFORM_ANGLE_TYPE_ANGLE &&
                                  attributes[index + 1] == EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
                              ? EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE
                              : attributes[index + 1];
    }
    ours[length] = EGL_FEATURE_OVERRIDES_DISABLED_ANGLE;
    ours[length + 1] = (egl_attrib)vulkan_features_off;
    ours[length + 2] = EGL_NONE;
    return get_platform_display(platform, native, ours);
}

static int (*_Atomic wait_until_scheduled)(double seconds);

static void commit_entered(void) {
    for (;;) {
        atomic_fetch_add(&committing, 1);
        if (!apkrt_is_background()) {
            return;
        }
        atomic_fetch_sub(&committing, 1);
        apkrt_wait_foreground();
    }
}

static void commit_counted(void) {
    atomic_fetch_sub(&committing, 1);
}

static void gates(void *library) {
    void (*set)(void (*)(void), void (*)(void)) =
        (void (*)(void (*)(void), void (*)(void)))dlsym(library, "ANGLEMetalSetCommitGates");
    if (set != NULL) {
        set(commit_entered, commit_counted);
    }
    atomic_store(&wait_until_scheduled, (int (*)(double))dlsym(library, "ANGLEMetalWaitUntilScheduled"));
    if (minato_graphics_angle_vulkan_backend() && !atomic_load(&vulkan_ready)) {
        void (*set_loader)(void *) = (void (*)(void *))dlsym(library, "ANGLEVulkanSetGetInstanceProcAddr");
        void *loader = minato_graphics_angle_vulkan_loader();
        if (set_loader != NULL && loader != NULL) {
            set_loader(loader);
            atomic_store(&vulkan_ready, 1);
        } else {
            fprintf(stderr, "minato graphics: ANGLE %s, so the ES 3.1 path stays on Metal\n",
                    set_loader == NULL ? "lacks ANGLEVulkanSetGetInstanceProcAddr (patch 0009)" : "has no MoltenVK");
        }
    }
}

int minato_graphics_angle_provider(void *context, uint64_t deadline) {
    (void)context;
    int (*wait)(double) = atomic_load(&wait_until_scheduled);
    if (wait == NULL) {
        return 1;
    }
    while (atomic_load(&committing) != 0) {
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
            return 0;
        }
        usleep(100);
    }
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    return now < deadline && wait((double)(deadline - now) / 1e9);
}

#define WRAP(LIB, name, angle, ours)                    \
    angle = (__typeof__(angle))table[MINATO_##LIB##_SLOT_##name]; \
    table[MINATO_##LIB##_SLOT_##name] = (void *)ours;

void minato_graphics_egl_wrap(void **table, void *library) {
    WRAP(EGL, eglCreateWindowSurface, create_window_surface, window_surface_created)
    WRAP(EGL, eglCreatePlatformWindowSurface, create_platform_window_surface, platform_window_surface_created)
    WRAP(EGL, eglDestroySurface, destroy_surface, surface_destroyed)
    WRAP(EGL, eglSwapBuffers, swap_buffers, swapped)
    WRAP(EGL, eglPresentationTimeANDROID, presentation_time, presentation_time_set)
    WRAP(EGL, eglTerminate, terminate, terminated)
    WRAP(EGL, eglGetPlatformDisplay, get_platform_display, platform_display_asked)
    minato_graphics_images_wrap(table);
    gates(library);
}

void minato_graphics_gles_wrap(void **table, void *library) {
    WRAP(GLES, glDebugMessageCallback, debug_callback, callback_given)
    WRAP(GLES, glDebugMessageCallbackKHR, debug_callback_khr, callback_given_khr)
    WRAP(GLES, glGetPointerv, get_pointerv, pointer_given)
    WRAP(GLES, glGetPointervKHR, get_pointerv_khr, pointer_given_khr)
    gates(library);
}
#undef WRAP
