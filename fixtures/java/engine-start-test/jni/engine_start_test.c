// engine-start-test: what a game engine's player asks of EGL, GLES and the Java display at its start, past the
// lookups that gles-procs-test checks. A NativeActivity whose worker thread, once the window has the focus, runs:
// - contexts: an ES 3.2 and an ES 3.1 context asked first (a player asks for its best, then falls back), each made
//   or refused (EGL_BAD_MATCH, or EGL_BAD_CONFIG as the emulator), then glGetString without a current context,
//   then an ES 3.0 context;
// - shared contexts: a context shared with the first, made current on a second thread, uploads a texture that the
//   first thread reads back through a framebuffer;
// - frame timestamps (EGL_ANDROID_get_frame_timestamps): the answers before they are enabled, the supported
//   timestamps and compositor timings, the frame IDs across swaps, an old frame's timestamps and a future frame's;
// - a frame pacer's start: EGL fences waited on from the render thread, a thread without a context and one with
//   another context, the default display's refresh rate, app vsync offset and presentation deadline through JNI,
//   and a class of the APK found through the activity's class loader, whose Java code hands the display's refresh
//   periods to a native method registered from here.
// The lines under the tag engine-start-test hold on every Android device (an answer is one Android allows, a value
// is in its range); the values themselves go to engine-start-test-device. It ends with "done" and finishes.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TAG "engine-start-test"

static void say(const char *tag, const char *format, ...) {
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    __android_log_write(ANDROID_LOG_INFO, tag, line);
}
#define LOG(...) say(TAG, __VA_ARGS__)
#define DEV(...) say(TAG "-device", __VA_ARGS__)

static ANativeActivity *g_activity;
static ANativeWindow *g_window;
static atomic_bool g_started;

static const char *yes(bool value) {
    return value ? "true" : "false";
}

static const char *error_name(EGLint error) {
    switch (error) {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    default: return "another error";
    }
}

static bool listed(const char *list, const char *name) {
    const size_t length = strlen(name);
    for (const char *at = list; list != NULL && (at = strstr(at, name)) != NULL; at += length) {
        if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == '\0')) return true;
    }
    return false;
}

static EGLConfig es3_config(EGLDisplay display, EGLint surface_type) {
    const EGLint spec[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, surface_type,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                           EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config = NULL;
    EGLint count = 0;
    eglChooseConfig(display, spec, &config, 1, &count);
    return count > 0 ? config : NULL;
}

static EGLContext make_context(EGLDisplay display, EGLConfig config, int minor, EGLContext share) {
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, minor, EGL_NONE};
    return eglCreateContext(display, config, share, attributes);
}

static EGLSurface pbuffer(EGLDisplay display, EGLConfig config) {
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    return eglCreatePbufferSurface(display, config, size);
}

/* The version GL_VERSION names, as major * 10 + minor, 0 for none. */
static int gl_version(void) {
    const char *version = (const char *)glGetString(GL_VERSION);
    int major = 0, minor = 0;
    if (version == NULL || sscanf(version, "OpenGL ES %d.%d", &major, &minor) != 2) return 0;
    return major * 10 + minor;
}

/* An ES 3.2 and an ES 3.1 context, each made (and then of that version or above) or refused with EGL_BAD_MATCH, as
   EGL_KHR_create_context has it, or EGL_BAD_CONFIG, as the emulator's driver answers; glGetString without a current
   context; then the ES 3.0 context. */
static EGLContext contexts(EGLDisplay display, EGLConfig config, EGLSurface surface) {
    for (int minor = 2; minor >= 1; minor--) {
        EGLContext context = make_context(display, config, minor, EGL_NO_CONTEXT);
        const EGLint error = eglGetError();
        bool allowed;
        if (context == EGL_NO_CONTEXT) {
            allowed = error == EGL_BAD_MATCH || error == EGL_BAD_CONFIG;
            DEV("ES 3.%d context: refused, %s", minor, error_name(error));
        } else {
            const bool current = eglMakeCurrent(display, surface, surface, context);
            const int version = current ? gl_version() : 0;
            allowed = current && version >= 30 + minor;
            DEV("ES 3.%d context: made, GL_VERSION %s, GL_ANDROID_extension_pack_es31a %s", minor,
                current ? (const char *)glGetString(GL_VERSION) : "(not current)",
                yes(listed((const char *)glGetString(GL_EXTENSIONS), "GL_ANDROID_extension_pack_es31a")));
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
        }
        LOG("ES 3.%d context asked first: an answer Android allows %s", minor, yes(allowed));
    }
    LOG("glGetString without a current context: %s", glGetString(GL_VERSION) == NULL ? "NULL" : "a string");
    EGLContext context = make_context(display, config, 0, EGL_NO_CONTEXT);
    const bool current = context != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, context);
    LOG("ES 3.0 context after them: made %s, current %s, error %s", yes(context != EGL_NO_CONTEXT), yes(current),
        error_name(eglGetError()));
    const int version = current ? gl_version() : 0;
    LOG("ES 3.0 context: GL_VERSION ES 3.0 or above %s, GL_EXTENSIONS %s", yes(version >= 30),
        current && glGetString(GL_EXTENSIONS) != NULL ? "a string" : "NULL");
    return context;
}

/* The shared context's thread: its own pbuffer, a 1x1 texture with a known color, then glFinish. */
struct shared {
    EGLDisplay display;
    EGLConfig config;
    EGLContext first;
    bool made, current;
    GLuint texture;
};

static void *shared_thread(void *argument) {
    struct shared *shared = argument;
    EGLContext context = make_context(shared->display, shared->config, 0, shared->first);
    EGLSurface surface = pbuffer(shared->display, shared->config);
    shared->made = context != EGL_NO_CONTEXT;
    shared->current = shared->made && eglMakeCurrent(shared->display, surface, surface, context);
    if (shared->current) {
        const GLubyte color[4] = {10, 200, 30, 255};
        glGenTextures(1, &shared->texture);
        glBindTexture(GL_TEXTURE_2D, shared->texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, color);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glFinish();
    }
    eglMakeCurrent(shared->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (shared->made) eglDestroyContext(shared->display, context);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(shared->display, surface);
    eglReleaseThread();
    return NULL;
}

/* A context shared with the current one, on a second thread: its texture read back here. */
static void shared_contexts(EGLDisplay display, EGLConfig config, EGLContext first) {
    struct shared shared = {display, config, first, false, false, 0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, shared_thread, &shared) != 0) return;
    pthread_join(thread, NULL);
    LOG("shared context on a second thread: made %s, current %s", yes(shared.made), yes(shared.current));
    GLubyte pixel[4] = {0, 0, 0, 0};
    if (shared.texture != 0) {
        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shared.texture, 0);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &shared.texture);
    }
    LOG("its texture read back here: %s", yes(pixel[0] == 10 && pixel[1] == 200 && pixel[2] == 30));
}

static const struct {
    EGLint name;
    const char *text;
} timestamps[] = {
    {EGL_REQUESTED_PRESENT_TIME_ANDROID, "requested present"},
    {EGL_RENDERING_COMPLETE_TIME_ANDROID, "rendering complete"},
    {EGL_COMPOSITION_LATCH_TIME_ANDROID, "composition latch"},
    {EGL_FIRST_COMPOSITION_START_TIME_ANDROID, "first composition start"},
    {EGL_LAST_COMPOSITION_START_TIME_ANDROID, "last composition start"},
    {EGL_FIRST_COMPOSITION_GPU_FINISHED_TIME_ANDROID, "first composition GPU finished"},
    {EGL_DISPLAY_PRESENT_TIME_ANDROID, "display present"},
    {EGL_DEQUEUE_READY_TIME_ANDROID, "dequeue ready"},
    {EGL_READS_DONE_TIME_ANDROID, "reads done"},
}, timings[] = {
    {EGL_COMPOSITE_DEADLINE_ANDROID, "composite deadline"},
    {EGL_COMPOSITE_INTERVAL_ANDROID, "composite interval"},
    {EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID, "composite to present latency"},
};

/* EGL_ANDROID_get_frame_timestamps on a window surface, as a frame pacer uses it. */
static void frame_timestamps(EGLDisplay display, EGLConfig config, EGLContext context) {
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    LOG("EGL_ANDROID_get_frame_timestamps listed: %s",
        yes(listed(extensions, "EGL_ANDROID_get_frame_timestamps")));
    PFNEGLGETNEXTFRAMEIDANDROIDPROC next_id =
        (PFNEGLGETNEXTFRAMEIDANDROIDPROC)eglGetProcAddress("eglGetNextFrameIdANDROID");
    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC get_timestamps =
        (PFNEGLGETFRAMETIMESTAMPSANDROIDPROC)eglGetProcAddress("eglGetFrameTimestampsANDROID");
    PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC timestamp_supported =
        (PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC)eglGetProcAddress("eglGetFrameTimestampSupportedANDROID");
    PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC timing_supported =
        (PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC)eglGetProcAddress("eglGetCompositorTimingSupportedANDROID");
    if (next_id == NULL || get_timestamps == NULL || timestamp_supported == NULL || timing_supported == NULL) {
        LOG("frame timestamps: a function NULL");
        return;
    }
    EGLSurface surface = eglCreateWindowSurface(display, config, g_window, NULL);
    if (surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context)) {
        LOG("frame timestamps: no window surface, %s", error_name(eglGetError()));
        return;
    }
    EGLuint64KHR id = 0;
    EGLBoolean result = next_id(display, surface, &id);
    LOG("before they are enabled: eglGetNextFrameIdANDROID %s, %s", yes(result), error_name(eglGetError()));
    const EGLint one[] = {EGL_REQUESTED_PRESENT_TIME_ANDROID};
    EGLnsecsANDROID value = 0;
    result = get_timestamps(display, surface, 1, 1, one, &value);
    LOG("before they are enabled: eglGetFrameTimestampsANDROID %s, %s", yes(result), error_name(eglGetError()));
    // Display present needs the display's present fences, which the emulator lacks: its answer is the device's.
    DEV("display present timestamp supported: %s",
        yes(timestamp_supported(display, surface, EGL_DISPLAY_PRESENT_TIME_ANDROID)));
    char line[512] = "";
    for (size_t index = 0; index < sizeof timestamps / sizeof *timestamps; index++) {
        if (timestamps[index].name == EGL_DISPLAY_PRESENT_TIME_ANDROID) continue;
        if (timestamp_supported(display, surface, timestamps[index].name)) {
            strncat(line, *line ? ", " : "", sizeof line - strlen(line) - 1);
            strncat(line, timestamps[index].text, sizeof line - strlen(line) - 1);
        }
    }
    LOG("supported timestamps besides display present: %s", *line ? line : "none");
    *line = '\0';
    for (size_t index = 0; index < sizeof timings / sizeof *timings; index++) {
        if (timing_supported(display, surface, timings[index].name)) {
            strncat(line, *line ? ", " : "", sizeof line - strlen(line) - 1);
            strncat(line, timings[index].text, sizeof line - strlen(line) - 1);
        }
    }
    LOG("supported compositor timings: %s", *line ? line : "none");
    result = eglSurfaceAttrib(display, surface, EGL_TIMESTAMPS_ANDROID, EGL_TRUE);
    EGLint enabled = 0;
    eglQuerySurface(display, surface, EGL_TIMESTAMPS_ANDROID, &enabled);
    LOG("EGL_TIMESTAMPS_ANDROID set: %s", yes(result));
    DEV("EGL_TIMESTAMPS_ANDROID queried: %d", enabled);  // libEGL sets it on the window; the driver answers the query
    EGLuint64KHR first = 0, second = 0;
    const bool got_first = next_id(display, surface, &first);
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(display, surface);
    const bool got_second = next_id(display, surface, &second);
    LOG("frame IDs: got %s, the next one after a swap is the one before plus 1 %s", yes(got_first && got_second),
        yes(second == first + 1));
    for (int frame = 0; frame < 4; frame++) {
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display, surface);
    }
    usleep(200000);
    const EGLint asked[] = {EGL_REQUESTED_PRESENT_TIME_ANDROID, EGL_RENDERING_COMPLETE_TIME_ANDROID,
                            EGL_DEQUEUE_READY_TIME_ANDROID};
    EGLnsecsANDROID values[3] = {0, 0, 0};
    result = get_timestamps(display, surface, first, 3, asked, values);
    const EGLint error = eglGetError();
    LOG("an earlier frame's timestamps: %s, %s, rendering complete is a time or pending %s", yes(result),
        error_name(error), yes(!result || values[1] > 0 || values[1] == EGL_TIMESTAMP_PENDING_ANDROID));
    DEV("an earlier frame's timestamps: %lld, %lld, %lld", (long long)values[0], (long long)values[1],
        (long long)values[2]);
    result = get_timestamps(display, surface, second + 100, 1, one, &value);
    LOG("a future frame's timestamps: %s, %s", yes(result), error_name(eglGetError()));
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
}

/* A frame pacer's fence waits: EGL_KHR_fence_sync's fences that the render thread makes (after drawing, then
   glFlush), waited on with eglClientWaitSyncKHR from the render thread itself, from a thread without a context and
   from one with a context of its own, with and without EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, for a timeout of 0, 1 s and
   EGL_FOREVER_KHR; eglWaitSyncKHR from each; eglGetSyncAttribKHR's status from a thread without a context; and
   EGL_ANDROID_native_fence_sync's file descriptor when it is listed. A wait for 0 may answer either way. */
enum waiter { SAME, NONE, OTHER };
static const char *const waiter_names[] = {"the render thread", "a thread without a context",
                                           "a thread with another context"};

struct fence {
    EGLDisplay display;
    EGLConfig config;
    enum waiter waiter;
    EGLSyncKHR sync;
    EGLint flags;
    EGLTimeKHR timeout;
    bool server;  // eglWaitSyncKHR in place of eglClientWaitSyncKHR
    EGLint result, error, status;
};

static PFNEGLCREATESYNCKHRPROC create_sync;
static PFNEGLDESTROYSYNCKHRPROC destroy_sync;
static PFNEGLCLIENTWAITSYNCKHRPROC client_wait;
static PFNEGLWAITSYNCKHRPROC server_wait;
static PFNEGLGETSYNCATTRIBKHRPROC sync_attribute;

static void wait_once(struct fence *fence) {
    if (fence->server) {
        fence->result = server_wait != NULL ? server_wait(fence->display, fence->sync, 0) : EGL_FALSE;
    } else {
        fence->result = client_wait(fence->display, fence->sync, fence->flags, fence->timeout);
    }
    fence->error = eglGetError();
    sync_attribute(fence->display, fence->sync, EGL_SYNC_STATUS_KHR, &fence->status);
}

static void *waiter_thread(void *argument) {
    struct fence *fence = argument;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    if (fence->waiter == OTHER) {
        context = make_context(fence->display, fence->config, 0, EGL_NO_CONTEXT);
        surface = pbuffer(fence->display, fence->config);
        eglMakeCurrent(fence->display, surface, surface, context);
    }
    wait_once(fence);
    eglMakeCurrent(fence->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(fence->display, context);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(fence->display, surface);
    eglReleaseThread();
    return NULL;
}

/* One fence after some drawing, flushed, waited on as `fence` says; false when it was not made. */
static bool fence_case(struct fence *fence) {
    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    fence->sync = create_sync(fence->display, EGL_SYNC_FENCE_KHR, NULL);
    glFlush();
    if (fence->sync == EGL_NO_SYNC_KHR) return false;
    pthread_t thread;
    if (fence->waiter == SAME) {
        wait_once(fence);
    } else if (pthread_create(&thread, NULL, waiter_thread, fence) == 0) {
        pthread_join(thread, NULL);
    }
    destroy_sync(fence->display, fence->sync);
    return true;
}

static const char *client_answer(const struct fence *fence) {
    const bool satisfied = fence->result == EGL_CONDITION_SATISFIED_KHR && fence->error == EGL_SUCCESS;
    const bool expired = fence->result == EGL_TIMEOUT_EXPIRED_KHR && fence->error == EGL_SUCCESS;
    return yes(satisfied || (fence->timeout == 0 && expired));
}

static void native_fence(EGLDisplay display) {
    const bool offered = listed(eglQueryString(display, EGL_EXTENSIONS), "EGL_ANDROID_native_fence_sync");
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fd =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    bool works = false;
    if (offered && dup_fd != NULL) {
        const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
        glClear(GL_COLOR_BUFFER_BIT);
        EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
        glFlush();
        const int fd = sync != EGL_NO_SYNC_KHR ? dup_fd(display, sync) : -1;
        if (fd >= 0) {
            struct pollfd poll_fd = {fd, POLLIN, 0};
            works = poll(&poll_fd, 1, 1000) == 1;
            close(fd);
        }
        if (sync != EGL_NO_SYNC_KHR) destroy_sync(display, sync);
    }
    LOG("EGL_ANDROID_native_fence_sync: listed and its fd signals, or not listed: %s", yes(!offered || works));
    DEV("EGL_ANDROID_native_fence_sync listed: %s", yes(offered));
}

static void fence_waits(EGLDisplay display, EGLConfig config) {
    create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    client_wait = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    server_wait = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    sync_attribute = (PFNEGLGETSYNCATTRIBKHRPROC)eglGetProcAddress("eglGetSyncAttribKHR");
    if (create_sync == NULL || destroy_sync == NULL || client_wait == NULL || sync_attribute == NULL) {
        LOG("fences: a function NULL");
        return;
    }
    const EGLTimeKHR timeouts[] = {0, 1000000000, EGL_FOREVER_KHR};
    for (enum waiter waiter = SAME; waiter <= OTHER; waiter++) {
        for (int flush = 0; flush <= 1; flush++) {
            const char *answers[3] = {"", "", ""};
            char raw[160] = "";
            for (int index = 0; index < 3; index++) {
                struct fence fence = {display, config, waiter, EGL_NO_SYNC_KHR,
                                      flush ? EGL_SYNC_FLUSH_COMMANDS_BIT_KHR : 0, timeouts[index], false, 0, 0, 0};
                answers[index] = fence_case(&fence) ? client_answer(&fence) : "not made";
                const size_t used = strlen(raw);
                snprintf(raw + used, sizeof raw - used, " 0x%x/%s", (unsigned)fence.result, error_name(fence.error));
            }
            LOG("client wait from %s%s: for 0 %s, for 1 s %s, forever %s", waiter_names[waiter],
                flush ? ", flushing" : "", answers[0], answers[1], answers[2]);
            DEV("client wait from %s%s:%s", waiter_names[waiter], flush ? ", flushing" : "", raw);
        }
        struct fence fence = {display, config, waiter, EGL_NO_SYNC_KHR, 0, 0, true, 0, 0, 0};
        if (server_wait != NULL && fence_case(&fence)) {
            LOG("eglWaitSyncKHR from %s: %s, %s", waiter_names[waiter], yes(fence.result == EGL_TRUE),
                error_name(fence.error));
        }
        fence = (struct fence){display, config, waiter, EGL_NO_SYNC_KHR, 0, 1000000000, false, 0, 0, 0};
        if (fence_case(&fence)) {
            LOG("its status from %s after the wait: signaled %s", waiter_names[waiter],
                yes(fence.status == EGL_SIGNALED_KHR));
        }
    }
    native_fence(display);
}

static jlongArray g_periods;
static int g_period_count;
static int64_t g_first_period;

static void set_supported_refresh_periods(JNIEnv *env, jclass type, jlongArray periods) {
    (void)type;
    g_period_count = periods != NULL ? (*env)->GetArrayLength(env, periods) : -1;
    if (g_period_count > 0) {
        jlong first = 0;
        (*env)->GetLongArrayRegion(env, periods, 0, 1, &first);
        g_first_period = first;
    }
    g_periods = periods;
}

static bool pending(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return false;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return true;
}

/* A frame pacer's Java side: the default display's timing through JNI, and a class of the APK found through the
   activity's class loader, whose Java code calls a native method registered from here. */
static void java_side(void) {
    JNIEnv *env = NULL;
    if ((*g_activity->vm)->AttachCurrentThread(g_activity->vm, &env, NULL) != JNI_OK) return;
    jobject activity = g_activity->clazz;
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    jmethodID get_window_manager =
        (*env)->GetMethodID(env, activity_class, "getWindowManager", "()Landroid/view/WindowManager;");
    jobject manager = (*env)->CallObjectMethod(env, activity, get_window_manager);
    jclass manager_class = (*env)->FindClass(env, "android/view/WindowManager");
    jmethodID get_display =
        (*env)->GetMethodID(env, manager_class, "getDefaultDisplay", "()Landroid/view/Display;");
    jobject display = manager != NULL ? (*env)->CallObjectMethod(env, manager, get_display) : NULL;
    jclass display_class = (*env)->FindClass(env, "android/view/Display");
    jfloat rate = 0;
    jlong offset = -1, deadline = -1;
    bool thrown = pending(env);
    if (display != NULL) {
        rate = (*env)->CallFloatMethod(env, display, (*env)->GetMethodID(env, display_class, "getRefreshRate", "()F"));
        thrown |= pending(env);
        offset = (*env)->CallLongMethod(env, display,
                                        (*env)->GetMethodID(env, display_class, "getAppVsyncOffsetNanos", "()J"));
        thrown |= pending(env);
        deadline = (*env)->CallLongMethod(env, display, (*env)->GetMethodID(env, display_class,
                                                                             "getPresentationDeadlineNanos", "()J"));
        thrown |= pending(env);
    }
    LOG("default display: found %s, an exception %s", yes(display != NULL), yes(thrown));
    LOG("default display: refresh rate > 0 %s, app vsync offset >= 0 %s, presentation deadline > 0 %s",
        yes(rate > 0), yes(offset >= 0), yes(deadline > 0));
    DEV("default display: refresh rate %.2f, app vsync offset %lld, presentation deadline %lld", rate,
        (long long)offset, (long long)deadline);

    jmethodID get_loader = (*env)->GetMethodID(env, activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = (*env)->CallObjectMethod(env, activity, get_loader);
    jclass loader_class = (*env)->FindClass(env, "java/lang/ClassLoader");
    jmethodID load = (*env)->GetMethodID(env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = (*env)->NewStringUTF(env, "io.github.kisame76.minato.test.enginestart.PacingProbe");
    jclass probe = loader != NULL ? (jclass)(*env)->CallObjectMethod(env, loader, load, name) : NULL;
    thrown = pending(env);
    LOG("the APK's class through the activity's class loader: found %s, an exception %s", yes(probe != NULL),
        yes(thrown));
    if (probe != NULL) {
        const JNINativeMethod method = {"nSetSupportedRefreshPeriods", "([J)V", (void *)set_supported_refresh_periods};
        const bool registered = (*env)->RegisterNatives(env, probe, &method, 1) == JNI_OK;
        thrown = pending(env);
        jmethodID start = (*env)->GetStaticMethodID(env, probe, "start", "(Landroid/app/Activity;)V");
        if (registered && start != NULL) (*env)->CallStaticVoidMethod(env, probe, start, activity);
        thrown |= pending(env);
        LOG("its native method: registered %s, called with the refresh periods %s, an exception %s",
            yes(registered), yes(g_period_count > 0 && g_first_period > 0), yes(thrown));
        DEV("refresh periods: %d, the first %lld", g_period_count, (long long)g_first_period);
    }
    (void)g_periods;
    (*g_activity->vm)->DetachCurrentThread(g_activity->vm);
}

static void *probes(void *argument) {
    (void)argument;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, NULL, NULL)) {
        LOG("no display");
    } else {
        EGLConfig config = es3_config(display, EGL_PBUFFER_BIT | EGL_WINDOW_BIT);
        EGLSurface surface = config != NULL ? pbuffer(display, config) : EGL_NO_SURFACE;
        LOG("an ES 3 config for windows and pbuffers: %s", yes(surface != EGL_NO_SURFACE));
        if (surface != EGL_NO_SURFACE) {
            EGLContext context = contexts(display, config, surface);
            if (context != EGL_NO_CONTEXT) {
                shared_contexts(display, config, context);
                fence_waits(display, config);
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                frame_timestamps(display, config, context);
                eglDestroyContext(display, context);
            }
            eglDestroySurface(display, surface);
        }
    }
    java_side();
    LOG("done");
    ANativeActivity_finish(g_activity);
    return NULL;
}

static void on_window_focus_changed(ANativeActivity *activity, int focused) {
    (void)activity;
    if (!focused || g_window == NULL || atomic_exchange(&g_started, true)) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, probes, NULL) == 0) pthread_detach(thread);
}

static void on_window_created(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    g_window = window;
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *activity, void *state,
                                                                       size_t size) {
    (void)state;
    (void)size;
    g_activity = activity;
    activity->callbacks->onWindowFocusChanged = on_window_focus_changed;
    activity->callbacks->onNativeWindowCreated = on_window_created;
}
