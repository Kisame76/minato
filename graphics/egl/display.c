/* libEGL's displays, configs, strings, eglGetProcAddress and waits (ADR-0029, decision 14; ADR-0030,
   decision 4):
   - EGL_DEFAULT_DISPLAY, also through EGL_PLATFORM_ANDROID_KHR, is ANGLE's Metal display, asked for as such;
     another native display or platform is EGL_NO_DISPLAY with EGL_BAD_PARAMETER, as Android's libEGL answers.
   - eglChooseConfig takes EGL_RECORDABLE_ANDROID and EGL_FRAMEBUFFER_TARGET_ANDROID out before ANGLE sees
     them; eglGetConfigAttrib answers them with EGL_FALSE and EGL_NATIVE_VISUAL_ID with WINDOW_FORMAT_RGBA_8888
     for an RGBA8888 config.
   - The extension strings leave out what tools/frontgen withholds (egl_withheld.inc); the client extensions
     name EGL_KHR_platform_android, whose display libEGL gives.
   - eglGetProcAddress hands out libEGL's own functions and libGLESv2's, never ANGLE's.
   - eglPresentationTimeANDROID goes to ANGLE, whose Metal backend presents the surface's next swap at that time
     (patch 0005, ADR-0041 decision 11); the guest's CLOCK_MONOTONIC is the uptime clock it expects (F10). With
     MINATO_PRESENTS_WITHOUT_TIME=1 in the environment, decision 11's measurement switch, which only the benchmark
     and Debug builds pass to the guest, it answers EGL_TRUE and keeps the time from ANGLE: the swap comes without
     one.
   - eglClientWaitSync and eglClientWaitSyncKHR wait in slices of at most 10 ms, their flush is glFlush; a
     reusable sync, on which ANGLE cannot wait, is asked without a wait between sleeps. */
#include <android/native_window.h>
#include <pthread.h>
#include <stdint.h>
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

static int rgba8888(EGLDisplay display, EGLConfig config) {
    static const EGLint channels[] = {EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE};
    for (size_t index = 0; index < sizeof channels / sizeof *channels; index++) {
        EGLint bits = 0;
        if (!minato_host_eglGetConfigAttrib(display, config, channels[index], &bits) || bits != 8) {
            return 0;
        }
    }
    return 1;
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
    if (result && attribute == EGL_NATIVE_VISUAL_ID && rgba8888(display, config)) {
        *value = WINDOW_FORMAT_RGBA_8888;
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

static struct filtered *make(const char *angle, int client) {
    const size_t length = strlen(angle);
    struct filtered *made = calloc(1, sizeof *made);
    if (made == NULL || (made->angle = strdup(angle)) == NULL ||
        (made->text = malloc(length + sizeof PLATFORM_ANDROID + 1)) == NULL) {
        if (made != NULL) {
            free(made->angle);
            free(made);
        }
        return NULL;
    }
    made->client = client;
    char *out = made->text;
    int android = 0;
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
            android |= size == sizeof PLATFORM_ANDROID - 1 && memcmp(at, PLATFORM_ANDROID, size) == 0;
        }
        at += size;
    }
    if (client && !android) {
        if (out != made->text) {
            *out++ = ' ';
        }
        memcpy(out, PLATFORM_ANDROID, sizeof PLATFORM_ANDROID - 1);
        out += sizeof PLATFORM_ANDROID - 1;
    }
    *out = '\0';
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

/* In slices of at most 10 ms, with a return to guest code between them (ADR-0030, decision 4); a timeout of
   one slice or less goes as it is. Once the sync is not signaled already, the flush is glFlush. A reusable
   sync is asked without a wait, with sleeps of 64 us growing to 1 ms between. */
#define SLICED(name, sync_type, time_type)                                                                        \
    EGLint name(EGLDisplay display, sync_type sync, EGLint flags, time_type timeout) {                            \
        const uint64_t start = now_ns();                                                                          \
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
