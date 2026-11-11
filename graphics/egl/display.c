/* libEGL's displays, configs, strings, eglGetProcAddress and waits (ADR-0029, decision 14; ADR-0030,
   decision 4):
   - EGL_DEFAULT_DISPLAY, also through EGL_PLATFORM_ANDROID_KHR, is ANGLE's Metal display, asked for as such;
     another native display or platform is EGL_NO_DISPLAY with EGL_BAD_PARAMETER, as Android's libEGL answers.
   - eglChooseConfig takes EGL_RECORDABLE_ANDROID and EGL_FRAMEBUFFER_TARGET_ANDROID out before ANGLE sees
     them; eglGetConfigAttrib answers them with EGL_FALSE and EGL_NATIVE_VISUAL_ID with WINDOW_FORMAT_RGBA_8888
     for an RGBA8888 config and WINDOW_FORMAT_RGBX_8888 for an RGB888 one without alpha (patch 0006), as
     Android 15 answers.
   - The extension strings leave out what tools/frontgen withholds (egl_withheld.inc); the client extensions
     name EGL_KHR_platform_android, whose display libEGL gives, and a display's name
     EGL_ANDROID_get_native_client_buffer and EGL_ANDROID_image_native_buffer, whose images of AHardwareBuffers
     libEGL and graphics/host make (image.c, ADR-0058 b)).
   - eglGetProcAddress hands out libEGL's own functions and libGLESv2's, never ANGLE's.
   - eglPresentationTimeANDROID goes to ANGLE, whose Metal backend presents the surface's next swap at that time
     (patch 0005, ADR-0041 decision 11); the guest's CLOCK_MONOTONIC is the uptime clock it expects (F10). With
     MINATO_PRESENTS_WITHOUT_TIME=1 in the environment, decision 11's measurement switch, which only the benchmark
     and Debug builds pass to the guest, it answers EGL_TRUE and keeps the time from ANGLE: the swap comes without
     one.
     On a SurfaceTexture's queue window the time is the next frame's timestamp (queue.c).
   - eglClientWaitSync and eglClientWaitSyncKHR wait in slices of at most 10 ms, their flush is glFlush; a
     reusable sync, on which ANGLE cannot wait, is asked without a wait between sleeps. On a thread without a
     current context, where ANGLE's Metal backend refuses a client wait with EGL_BAD_CONTEXT and Android's drivers
     wait (a frame pacer waits for its render thread's fences so), the sync's status is asked between the same
     sleeps, and the flush bit has no context to flush. eglWaitSync and eglWaitSyncKHR answer EGL_TRUE there for a
     sync that exists, as Android's drivers do (no context's commands to hold back), where ANGLE refuses with
     EGL_BAD_MATCH.
   - eglMakeCurrent, eglCreateContext, eglDestroyContext and eglReleaseThread go to ANGLE, traced with
     MINATO_EGL_TRACE=1 (trace.c). eglMakeCurrent's failure is logged (E/libEGL, at most one line a second, the others
     counted), and so is the first time a context is made current without a draw surface (diagnostics that stay in
     the build). */
#include <android/log.h>
#include <android/native_window.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libegl.h"

MINATO_HOST(eglGetPlatformDisplay);
MINATO_HOST(eglTerminate);
MINATO_HOST(eglChooseConfig);
MINATO_HOST(eglGetConfigAttrib);
MINATO_HOST(eglQueryString);
MINATO_HOST(eglPresentationTimeANDROID);
MINATO_HOST(eglClientWaitSync);
MINATO_HOST(eglClientWaitSyncKHR);
MINATO_HOST(eglWaitSync);
MINATO_HOST(eglWaitSyncKHR);
MINATO_HOST(eglMakeCurrent);
MINATO_HOST(eglCreateContext);
MINATO_HOST(eglDestroyContext);
MINATO_HOST(eglReleaseThread);

static EGLDisplay metal_display(void) {
    static const EGLAttrib metal[] = MINATO_EGL_METAL_DISPLAY;
    return minato_host_eglGetPlatformDisplay(MINATO_EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, metal);
}

/* EGL_NO_DISPLAY with EGL_BAD_PARAMETER, ANGLE's answer for a platform it does not know. */
static EGLDisplay no_display(void) {
    return minato_host_eglGetPlatformDisplay(0, EGL_DEFAULT_DISPLAY, NULL);
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType display) {
    return display == EGL_DEFAULT_DISPLAY ? metal_display() : no_display();
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *display, const EGLAttrib *attributes) {
    (void)attributes;
    return platform == EGL_PLATFORM_ANDROID_KHR && display == EGL_DEFAULT_DISPLAY ? metal_display() : no_display();
}

EGLBoolean eglTerminate(EGLDisplay display) {
    const EGLBoolean result = minato_host_eglTerminate(display);
    if (minato_trace_on()) {
        minato_trace("eglTerminate(display %p): %s, error 0x%x", display, result ? "EGL_TRUE" : "EGL_FALSE",
                     result ? EGL_SUCCESS : minato_peek_error());
    }
    if (result) {
        minato_release_windows(display);
    }
    return result;
}

static int android_only(EGLint attribute) {
    return attribute == EGL_RECORDABLE_ANDROID || attribute == EGL_FRAMEBUFFER_TARGET_ANDROID;
}

EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint *attributes, EGLConfig *configs, EGLint size,
                           EGLint *count) {
    size_t length = 0;
    while (attributes != NULL && attributes[length] != EGL_NONE) {
        length += 2;
    }
    EGLint *ours = attributes != NULL ? malloc((length + 1) * sizeof *ours) : NULL;
    if (ours == NULL) {
        return minato_host_eglChooseConfig(display, attributes, configs, size, count);
    }
    size_t kept = 0;
    for (size_t index = 0; index < length; index += 2) {
        if (!android_only(attributes[index])) {
            ours[kept++] = attributes[index];
            ours[kept++] = attributes[index + 1];
        }
    }
    ours[kept] = EGL_NONE;
    const EGLBoolean result = minato_host_eglChooseConfig(display, ours, configs, size, count);
    free(ours);
    return result;
}

/* The window format of the config's buffers: RGBA_8888 for RGBA8888, RGBX_8888 for RGB888 without alpha, 0 for
   another. */
static EGLint window_format(EGLDisplay display, EGLConfig config) {
    static const EGLint channels[] = {EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE};
    EGLint bits[sizeof channels / sizeof *channels];
    for (size_t index = 0; index < sizeof channels / sizeof *channels; index++) {
        if (!minato_host_eglGetConfigAttrib(display, config, channels[index], &bits[index]) ||
            (index < 3 && bits[index] != 8)) {
            return 0;
        }
    }
    return bits[3] == 8 ? WINDOW_FORMAT_RGBA_8888 : bits[3] == 0 ? WINDOW_FORMAT_RGBX_8888 : 0;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value) {
    if (android_only(attribute)) {
        /* ANGLE checks the display and the config, and leaves its error. */
        EGLint id = 0;
        if (!minato_host_eglGetConfigAttrib(display, config, EGL_CONFIG_ID, &id)) {
            return EGL_FALSE;
        }
        *value = EGL_FALSE;
        return EGL_TRUE;
    }
    const EGLBoolean result = minato_host_eglGetConfigAttrib(display, config, attribute, value);
    if (result && attribute == EGL_NATIVE_VISUAL_ID) {
        const EGLint format = window_format(display, config);
        if (format != 0) {
            *value = format;
        }
    }
    return result;
}

/* The extensions withheld by name, and by a prefix, which also takes names beyond the registry
   (EGL_ANGLE_platform_angle_display_key). The first entry of each is empty. */
static const char *const withheld[] = {
    "",
#define MINATO_WITHHELD(name) #name,
#define MINATO_WITHHELD_PREFIX(prefix)
#include "egl_withheld.inc"
#undef MINATO_WITHHELD
#undef MINATO_WITHHELD_PREFIX
};
static const char *const withheld_prefixes[] = {
    "",
#define MINATO_WITHHELD(name)
#define MINATO_WITHHELD_PREFIX(prefix) #prefix,
#include "egl_withheld.inc"
#undef MINATO_WITHHELD
#undef MINATO_WITHHELD_PREFIX
};

static int is_withheld(const char *name, size_t length) {
    for (size_t index = 1; index < sizeof withheld / sizeof *withheld; index++) {
        if (strlen(withheld[index]) == length && memcmp(withheld[index], name, length) == 0) {
            return 1;
        }
    }
    for (size_t index = 1; index < sizeof withheld_prefixes / sizeof *withheld_prefixes; index++) {
        const size_t prefix = strlen(withheld_prefixes[index]);
        if (prefix <= length && memcmp(withheld_prefixes[index], name, prefix) == 0) {
            return 1;
        }
    }
    return 0;
}

#define PLATFORM_ANDROID "EGL_KHR_platform_android"

/* The extensions libEGL adds to a string: the client's, and a display's. */
static const char *const client_added[] = {PLATFORM_ANDROID};
static const char *const display_added[] = {"EGL_ANDROID_get_native_client_buffer", "EGL_ANDROID_image_native_buffer",
                                             "EGL_ANDROID_get_frame_timestamps"};

/* One of ANGLE's extension strings and what is kept of it, for the process's life: displays and their
   strings are few. */
struct filtered {
    struct filtered *next;
    int client;
    char *angle;
    char *text;
};
static struct filtered *filtered;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Whether the space-separated `list` names `name`. */
static int names(const char *list, const char *name) {
    const size_t length = strlen(name);
    for (const char *at = list; *at != '\0';) {
        const size_t size = strcspn(at, " ");
        if (size == length && memcmp(at, name, size) == 0) {
            return 1;
        }
        at += size + (at[size] == ' ');
    }
    return 0;
}

static struct filtered *make(const char *angle, int client) {
    const char *const *added = client ? client_added : display_added;
    const size_t count = client ? sizeof client_added / sizeof *client_added
                                : sizeof display_added / sizeof *display_added;
    size_t length = strlen(angle) + 1;
    for (size_t index = 0; index < count; index++) {
        length += strlen(added[index]) + 1;
    }
    struct filtered *made = calloc(1, sizeof *made);
    if (made == NULL || (made->angle = strdup(angle)) == NULL || (made->text = malloc(length)) == NULL) {
        if (made != NULL) {
            free(made->angle);
            free(made);
        }
        return NULL;
    }
    made->client = client;
    char *out = made->text;
    *out = '\0';
    for (const char *at = angle; *at != '\0';) {
        if (*at == ' ') {
            at++;
            continue;
        }
        const size_t size = strcspn(at, " ");
        if (!is_withheld(at, size)) {
            if (out != made->text) {
                *out++ = ' ';
            }
            memcpy(out, at, size);
            out += size;
            *out = '\0';
        }
        at += size;
    }
    for (size_t index = 0; index < count; index++) {
        if (!names(made->text, added[index])) {
            if (out != made->text) {
                *out++ = ' ';
            }
            strcpy(out, added[index]);
            out += strlen(added[index]);
        }
    }
    return made;
}

static const char *filter(const char *angle, int client) {
    pthread_mutex_lock(&lock);
    struct filtered *entry = filtered;
    while (entry != NULL && (entry->client != client || strcmp(entry->angle, angle) != 0)) {
        entry = entry->next;
    }
    if (entry == NULL && (entry = make(angle, client)) != NULL) {
        entry->next = filtered;
        filtered = entry;
    }
    pthread_mutex_unlock(&lock);
    return entry != NULL ? entry->text : angle;
}

const char *eglQueryString(EGLDisplay display, EGLint name) {
    const char *string = minato_host_eglQueryString(display, name);
    return name == EGL_EXTENSIONS && string != NULL ? filter(string, display == EGL_NO_DISPLAY) : string;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    void *function = NULL;
    if (name != NULL && strncmp(name, "egl", 3) == 0) {
        function = minato_egl_proc(name);
    } else if (name != NULL && strncmp(name, "gl", 2) == 0) {
        function = minato_gles_function(name);
    }
    return (__eglMustCastToProperFunctionPointerType)function;
}

EGLBoolean eglPresentationTimeANDROID(EGLDisplay display, EGLSurface surface, EGLnsecsANDROID time) {
    minato_timestamps_presentation(display, surface, time);
    if (minato_queue_presentation_time(display, surface, time)) {
        return EGL_TRUE;  // the next frame's timestamp on a SurfaceTexture's queue (queue.c)
    }
    const char *without_time = getenv("MINATO_PRESENTS_WITHOUT_TIME");
    if (without_time != NULL && strcmp(without_time, "1") == 0) {
        return EGL_TRUE;
    }
    return minato_host_eglPresentationTimeANDROID(display, surface, time);
}

#define SLICE_NS 10000000u

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

/* Whether a sync is reusable (EGL_KHR_reusable_sync). ANGLE waits for such a sync on a std::unique_lock that
   owns no mutex, which throws and ends the process, unless it is signaled or the timeout is 0: libEGL asks
   with the timeout 0 only, and sleeps between in guest code. */
static int reusable(EGLDisplay display, EGLSyncKHR sync) {
    EGLint type = 0;
    return eglGetSyncAttribKHR(display, sync, EGL_SYNC_TYPE_KHR, &type) && type == EGL_SYNC_REUSABLE_KHR;
}

/* The flush of EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, for the current context: libGLES's glFlush, which does not
   wait for Metal to schedule the command buffer, as ANGLE's own flush for the bit does. */
static void flush(void) {
    void (*gl_flush)(void) = (void (*)(void))minato_gles_function("glFlush");
    if (gl_flush != NULL) {
        gl_flush();
    }
}

/* A client wait without a current context: the sync's status until it is signaled or the timeout ends, with
   sleeps of 64 us growing to 1 ms between; EGL_FALSE, with the status query's error, for a sync it cannot ask. */
static EGLint status_wait(EGLDisplay display, EGLSyncKHR sync, uint64_t start, uint64_t timeout) {
    for (uint64_t pause = 64000;; pause = pause < 1000000 ? 2 * pause : pause) {
        EGLint status = 0;
        if (!eglGetSyncAttribKHR(display, sync, EGL_SYNC_STATUS_KHR, &status)) {
            return EGL_FALSE;
        }
        if (status == EGL_SIGNALED_KHR) {
            return EGL_CONDITION_SATISFIED_KHR;
        }
        const uint64_t waited = now_ns() - start, left = waited < timeout ? timeout - waited : 0;
        if (left == 0) {
            return EGL_TIMEOUT_EXPIRED_KHR;
        }
        const uint64_t sleep = pause < left ? pause : left;
        const struct timespec time = {(time_t)(sleep / 1000000000u), (long)(sleep % 1000000000u)};
        nanosleep(&time, NULL);
    }
}

/* In slices of at most 10 ms, with a return to guest code between them (ADR-0030, decision 4); a timeout of
   one slice or less goes as it is. Once the sync is not signaled already, the flush is glFlush. A reusable
   sync is asked without a wait, with sleeps of 64 us growing to 1 ms between. Without a current context, the
   status is asked (status_wait). */
#define SLICED(name, sync_type, time_type)                                                                        \
    EGLint name(EGLDisplay display, sync_type sync, EGLint flags, time_type timeout) {                            \
        const uint64_t start = now_ns();                                                                          \
        if (eglGetCurrentContext() == EGL_NO_CONTEXT) {                                                           \
            return status_wait(display, (EGLSyncKHR)sync, start, (uint64_t)timeout);                              \
        }                                                                                                         \
        if (flags & EGL_SYNC_FLUSH_COMMANDS_BIT_KHR) {                                                            \
            flags &= ~EGL_SYNC_FLUSH_COMMANDS_BIT_KHR;                                                            \
            const EGLint now = minato_host_##name(display, sync, flags, 0);                                       \
            if (now != EGL_TIMEOUT_EXPIRED_KHR || timeout == 0) {                                                 \
                return now;                                                                                       \
            }                                                                                                     \
            flush();                                                                                              \
        }                                                                                                         \
        const int polled = reusable(display, sync);                                                               \
        for (uint64_t pause = 64000;; pause = pause < 1000000 ? 2 * pause : pause) {                              \
            const uint64_t waited = now_ns() - start, left = waited < timeout ? timeout - waited : 0;             \
            const uint64_t slice = polled ? 0 : left < SLICE_NS ? left : SLICE_NS;                                \
            const EGLint result = minato_host_##name(display, sync, flags, slice);                                \
            if (result != EGL_TIMEOUT_EXPIRED_KHR || left <= slice) {                                             \
                return result;                                                                                    \
            }                                                                                                     \
            if (polled) {                                                                                         \
                const uint64_t sleep = pause < left ? pause : left;                                               \
                const struct timespec time = {(time_t)(sleep / 1000000000u), (long)(sleep % 1000000000u)};        \
                nanosleep(&time, NULL);                                                                           \
            }                                                                                                     \
        }                                                                                                         \
    }
SLICED(eglClientWaitSync, EGLSync, EGLTime)
SLICED(eglClientWaitSyncKHR, EGLSyncKHR, EGLTimeKHR)
#undef SLICED

/* A server wait: on a thread without a current context EGL_TRUE for a sync whose status can be asked (its error
   otherwise), else ANGLE's. */
EGLBoolean eglWaitSync(EGLDisplay display, EGLSync sync, EGLint flags) {
    EGLint status = 0;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return eglGetSyncAttribKHR(display, (EGLSyncKHR)sync, EGL_SYNC_STATUS_KHR, &status);
    }
    return minato_host_eglWaitSync(display, sync, flags);
}

EGLint eglWaitSyncKHR(EGLDisplay display, EGLSyncKHR sync, EGLint flags) {
    EGLint status = 0;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return eglGetSyncAttribKHR(display, sync, EGL_SYNC_STATUS_KHR, &status);
    }
    return minato_host_eglWaitSyncKHR(display, sync, flags);
}

/* At most one diagnostic line a second, so that an app failing every frame does not flood the log: the lines in
   between are counted and the next one says how many went. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t last_line_ns;
static unsigned skipped;

static void __attribute__((format(printf, 1, 2))) minato_egl_log(const char *format, ...) {
    const uint64_t now = now_ns();
    pthread_mutex_lock(&log_lock);
    const int write = last_line_ns == 0 || now - last_line_ns >= 1000000000u;
    const unsigned before = skipped;
    if (write) {
        last_line_ns = now;
        skipped = 0;
    } else {
        skipped++;
    }
    pthread_mutex_unlock(&log_lock);
    if (!write) {
        return;
    }
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (before > 0) {
        __android_log_print(ANDROID_LOG_ERROR, "libEGL", "%s (%u lines left out before)", line, before);
    } else {
        __android_log_write(ANDROID_LOG_ERROR, "libEGL", line);
    }
}

/* Whether `context` is made current without a draw surface for the first time (the first 16 contexts). */
static int first_surfaceless(EGLContext context) {
    static EGLContext seen[16];
    static unsigned count;
    int first = 1;
    pthread_mutex_lock(&log_lock);
    for (unsigned index = 0; index < count && first; index++) {
        first = seen[index] != context;
    }
    if (first && count < 16) {
        seen[count++] = context;
    } else if (first) {
        first = 0;
    }
    pthread_mutex_unlock(&log_lock);
    return first;
}

/* The trace's reset status of the context now current on this thread (GL_NO_ERROR, or a reset's kind). */
static unsigned reset_status(void) {
    unsigned (*status)(void) = (unsigned (*)(void))minato_gles_function("glGetGraphicsResetStatusEXT");
    return status != NULL ? status() : 0;
}

EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) {
    const EGLBoolean result = minato_host_eglMakeCurrent(display, draw, read, context);
    const EGLint error = result ? EGL_SUCCESS : minato_peek_error();  // read once, kept for the app
    if (minato_trace_on()) {
        if (result) {
            minato_trace_current(context);
        }
        minato_trace("eglMakeCurrent(draw %p window %llu, read %p, context %p): %s, error 0x%x, reset status 0x%x",
                     draw, (unsigned long long)minato_window_surface_id(draw), read, context,
                     result ? "EGL_TRUE" : "EGL_FALSE", error,
                     result && context != EGL_NO_CONTEXT ? reset_status() : 0);
    }
    if (!result) {
        minato_egl_log("eglMakeCurrent: failed, error 0x%x (draw %s, context %s)", error,
                       draw == EGL_NO_SURFACE ? "none" : minato_window_surface(display, draw) ? "a window" : "another",
                       context == EGL_NO_CONTEXT ? "none" : "given");
    } else if (context != EGL_NO_CONTEXT && draw == EGL_NO_SURFACE && first_surfaceless(context)) {
        __android_log_write(ANDROID_LOG_ERROR, "libEGL",
                            "eglMakeCurrent: a context without a draw surface (once a context)");
    }
    return result;
}

EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share, const EGLint *attributes) {
    const EGLContext context = minato_host_eglCreateContext(display, config, share, attributes);
    if (minato_trace_on()) {
        if (context != EGL_NO_CONTEXT) {
            minato_trace_context_made(context);
        }
        minato_trace("eglCreateContext(share %p): context %p, error 0x%x", share, context,
                     context != EGL_NO_CONTEXT ? EGL_SUCCESS : minato_peek_error());
    }
    return context;
}

EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context) {
    const EGLBoolean result = minato_host_eglDestroyContext(display, context);
    if (minato_trace_on()) {
        if (result) {
            minato_trace_context_destroyed(context);
        }
        minato_trace("eglDestroyContext(context %p): %s, error 0x%x%s", context, result ? "EGL_TRUE" : "EGL_FALSE",
                     result ? EGL_SUCCESS : minato_peek_error(),
                     context == eglGetCurrentContext() ? ", current on this thread" : "");
    }
    return result;
}

EGLBoolean eglReleaseThread(void) {
    const EGLBoolean result = minato_host_eglReleaseThread();
    if (minato_trace_on()) {
        if (result) {
            minato_trace_current(EGL_NO_CONTEXT);
        }
        minato_trace("eglReleaseThread: %s", result ? "EGL_TRUE" : "EGL_FALSE");
    }
    return result;
}
