/* Test 6 of ADR-0029 (graphics.gles_front): the EGL and GLES front libs on the Mac, on ANGLE's macOS slice,
   with graphics-run's window of 64 x 48 pixels (its gate open) and its test control (test_control.h).
   - Displays: EGL_DEFAULT_DISPLAY, also as EGL_PLATFORM_ANDROID_KHR, is ANGLE's Metal display; another native
     display or platform gives EGL_NO_DISPLAY with EGL_BAD_PARAMETER, as Android's libEGL answers.
   - Strings: no EGL_KHR_debug, EGL_ANDROID_blob_cache, EGL_EXT_platform_base or ANGLE's platform extensions;
     EGL_ANDROID_presentation_time among the display extensions (patch 0005, ADR-0041 decision 11) and
     EGL_KHR_platform_android among the client extensions; no GL_ANGLE_blob_cache, and GL_NUM_EXTENSIONS
     through every query counts what glGetStringi lists.
   - Configs: EGL_RECORDABLE_ANDROID and EGL_FRAMEBUFFER_TARGET_ANDROID do not reach ANGLE; the config answers
     them with EGL_FALSE and EGL_NATIVE_VISUAL_ID with WINDOW_FORMAT_RGBA_8888.
   - eglGetProcAddress: for every name of the two tables the front lib's own function, the export where
     there is one (libEGL's, libGLESv2's), never ANGLE's; NULL for other names.
   - Debug output: glGetPointervKHR answers the app's function and user data; a message on a guest thread
     reaches the app's callback, one on a thread of the host's own does not.
   - Window surfaces are fixed-size: the window's size, then its buffer geometry one swap later, then the
     window's size again after setBuffersGeometry(0, 0); the drawable follows with the frame after.
   - eglPresentationTimeANDROID, reached through eglGetProcAddress as Swappy reaches it, hands Core Animation the
     time on the refresh grid graphics-run's test control sets, less half a period (ADR-0041 decision 11), and no
     time for a refresh at most 2 periods after the grid's tick (decision 11 b)); with
     MINATO_PRESENTS_WITHOUT_TIME=1 in the environment it answers the same, and the drawable comes without a time.
   - The waits in slices: a signal's handler runs within 30 ms of its sending during eglClientWaitSyncKHR,
     and the wait goes on. */
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "test_control.h"

#define EGL_PLATFORM_ANGLE_ANGLE 0x3202

static const char *egl_error(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        default: return "another error";
    }
}

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

static int listed(const char *list, const char *name) {
    const size_t length = strlen(name);
    for (const char *at = list; at != NULL && (at = strstr(at, name)) != NULL; at += length) {
        if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == '\0')) {
            return 1;
        }
    }
    return 0;
}

static int words(const char *list) {
    int count = 0;
    for (const char *at = list; at != NULL && *at != '\0';) {
        while (*at == ' ') {
            at++;
        }
        if (*at != '\0') {
            count++;
        }
        while (*at != ' ' && *at != '\0') {
            at++;
        }
    }
    return count;
}

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static void displays(EGLDisplay *display) {
    printf("displays:\n");
    *display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    const EGLDisplay android = eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, EGL_DEFAULT_DISPLAY, NULL);
    printf("  EGL_DEFAULT_DISPLAY: a display: %s; EGL_PLATFORM_ANDROID_KHR gives the same: %s\n",
           yes(*display != EGL_NO_DISPLAY), yes(android == *display));
    EGLDisplay other = eglGetDisplay((EGLNativeDisplayType)1);
    printf("  another native display: %s, %s\n", other == EGL_NO_DISPLAY ? "EGL_NO_DISPLAY" : "a display",
           egl_error(eglGetError()));
    other = eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, (void *)1, NULL);
    printf("  another native display of EGL_PLATFORM_ANDROID_KHR: %s, %s\n",
           other == EGL_NO_DISPLAY ? "EGL_NO_DISPLAY" : "a display", egl_error(eglGetError()));
    other = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, NULL);
    printf("  another platform: %s, %s\n", other == EGL_NO_DISPLAY ? "EGL_NO_DISPLAY" : "a display",
           egl_error(eglGetError()));
    EGLint major = 0, minor = 0;
    const EGLBoolean initialized = eglInitialize(*display, &major, &minor);
    printf("  eglInitialize: %s, EGL %d.%d\n", initialized ? "EGL_TRUE" : "EGL_FALSE", major, minor);
}

static void configs(EGLDisplay display, EGLConfig *config) {
    printf("configs:\n");
    const EGLint recordable[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RECORDABLE_ANDROID, EGL_TRUE, EGL_NONE};
    const EGLint target[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_FRAMEBUFFER_TARGET_ANDROID, EGL_TRUE,
                             EGL_NONE};
    EGLConfig found[32];
    EGLint count = 0, other = 0;
    const EGLBoolean chosen = eglChooseConfig(display, recordable, found, 32, &count);
    const EGLBoolean chosen_other = eglChooseConfig(display, target, found, 32, &other);
    printf("  asked for EGL_RECORDABLE_ANDROID: %s, %d configs; for EGL_FRAMEBUFFER_TARGET_ANDROID: %s, %d configs\n",
           chosen ? "EGL_TRUE" : "EGL_FALSE", count, chosen_other ? "EGL_TRUE" : "EGL_FALSE", other);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_SURFACE_TYPE,
                                 EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    eglChooseConfig(display, attributes, config, 1, &count);
    EGLint visual = -1, recorded = -1, framebuffer = -1;
    const EGLBoolean got_visual = eglGetConfigAttrib(display, *config, EGL_NATIVE_VISUAL_ID, &visual);
    const EGLBoolean got_recorded = eglGetConfigAttrib(display, *config, EGL_RECORDABLE_ANDROID, &recorded);
    const EGLBoolean got_framebuffer =
        eglGetConfigAttrib(display, *config, EGL_FRAMEBUFFER_TARGET_ANDROID, &framebuffer);
    printf("  RGBA8888 D24S8: EGL_NATIVE_VISUAL_ID %d (%s), EGL_RECORDABLE_ANDROID %d (%s), "
           "EGL_FRAMEBUFFER_TARGET_ANDROID %d (%s)\n",
           visual, got_visual ? "EGL_TRUE" : "EGL_FALSE", recorded, got_recorded ? "EGL_TRUE" : "EGL_FALSE",
           framebuffer, got_framebuffer ? "EGL_TRUE" : "EGL_FALSE");
}

typedef void (*get_integer_robust)(GLenum, GLsizei, GLsizei *, GLint *);
typedef void (*get_integer64_robust)(GLenum, GLsizei, GLsizei *, GLint64 *);
typedef void (*get_float_robust)(GLenum, GLsizei, GLsizei *, GLfloat *);
typedef void (*get_boolean_robust)(GLenum, GLsizei, GLsizei *, GLboolean *);

static void strings(EGLDisplay display) {
    printf("strings:\n");
    const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("  client extensions: %s\n", client != NULL ? client : "(NULL)");
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    printf("  display extensions: %d; EGL_ANDROID_blob_cache %s, EGL_KHR_debug %s, EGL_ANDROID_recordable %s, "
           "EGL_KHR_fence_sync %s, EGL_ANDROID_presentation_time %s; the same string at the next call: %s\n",
           words(extensions), yes(listed(extensions, "EGL_ANDROID_blob_cache")),
           yes(listed(extensions, "EGL_KHR_debug")), yes(listed(extensions, "EGL_ANDROID_recordable")),
           yes(listed(extensions, "EGL_KHR_fence_sync")), yes(listed(extensions, "EGL_ANDROID_presentation_time")),
           yes(eglQueryString(display, EGL_EXTENSIONS) == extensions));

    const char *gl = (const char *)glGetString(GL_EXTENSIONS);
    const int count = words(gl);
    GLint integer = -1;
    GLint64 integer64 = -1;
    GLfloat real = -1;
    GLboolean boolean = GL_FALSE;
    GLint robust = -1;
    GLint64 robust64 = -1;
    GLfloat robust_real = -1;
    GLboolean robust_boolean = GL_FALSE;
    glGetIntegerv(GL_NUM_EXTENSIONS, &integer);
    glGetInteger64v(GL_NUM_EXTENSIONS, &integer64);
    glGetFloatv(GL_NUM_EXTENSIONS, &real);
    glGetBooleanv(GL_NUM_EXTENSIONS, &boolean);
    const get_integer_robust integer_robust = (get_integer_robust)eglGetProcAddress("glGetIntegervRobustANGLE");
    const get_integer64_robust integer64_robust =
        (get_integer64_robust)eglGetProcAddress("glGetInteger64vRobustANGLE");
    const get_float_robust float_robust = (get_float_robust)eglGetProcAddress("glGetFloatvRobustANGLE");
    const get_boolean_robust boolean_robust = (get_boolean_robust)eglGetProcAddress("glGetBooleanvRobustANGLE");
    if (integer_robust != NULL && integer64_robust != NULL && float_robust != NULL && boolean_robust != NULL) {
        integer_robust(GL_NUM_EXTENSIONS, 1, NULL, &robust);
        integer64_robust(GL_NUM_EXTENSIONS, 1, NULL, &robust64);
        float_robust(GL_NUM_EXTENSIONS, 1, NULL, &robust_real);
        boolean_robust(GL_NUM_EXTENSIONS, 1, NULL, &robust_boolean);
    }
    printf("  GL_EXTENSIONS: %d; GL_ANGLE_blob_cache %s, GL_KHR_debug %s; the same string at the next call: %s\n",
           count, yes(listed(gl, "GL_ANGLE_blob_cache")), yes(listed(gl, "GL_KHR_debug")),
           yes((const char *)glGetString(GL_EXTENSIONS) == gl));
    printf("  GL_NUM_EXTENSIONS: glGetIntegerv %d, glGetInteger64v %lld, glGetFloatv %.0f, glGetBooleanv %s; "
           "their RobustANGLE forms %d, %lld, %.0f, %s\n",
           integer, (long long)integer64, (double)real, boolean ? "GL_TRUE" : "GL_FALSE", robust, (long long)robust64,
           (double)robust_real, robust_boolean ? "GL_TRUE" : "GL_FALSE");
    int same = 1;
    const char *at = gl;
    for (int index = 0; index < count; index++) {
        while (*at == ' ') {
            at++;
        }
        const size_t length = strcspn(at, " ");
        const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);
        same &= name != NULL && strlen(name) == length && strncmp(name, at, length) == 0;
        at += length;
    }
    const GLubyte *beyond = glGetStringi(GL_EXTENSIONS, (GLuint)count);
    const GLenum error = glGetError();
    printf("  glGetStringi: the same %d names in the same order: %s; index %d: %s, %s\n", count, yes(same), count,
           beyond == NULL ? "NULL" : "a name", error == GL_INVALID_VALUE ? "GL_INVALID_VALUE" : "another error");
}

/* GL_NUM_EXTENSIONS without a current context: ANGLE answers nothing, and the values stay the app's; in an
   ES 2.0 context, which ANGLE answers too, the count of its own string. */
static void other_counts(EGLDisplay display, EGLConfig config, EGLSurface pbuffer, EGLContext context) {
    GLint value = -7;
    GLfloat real = -7;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    glGetIntegerv(GL_NUM_EXTENSIONS, &value);
    glGetFloatv(GL_NUM_EXTENSIONS, &real);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLContext es2 = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    eglMakeCurrent(display, pbuffer, pbuffer, es2);
    GLint es2_count = -1;
    glGetIntegerv(GL_NUM_EXTENSIONS, &es2_count);
    const char *es2_extensions = (const char *)glGetString(GL_EXTENSIONS);
    printf("  without a context, GL_NUM_EXTENSIONS leaves glGetIntegerv's and glGetFloatv's values: %s; in an ES 2.0 "
           "context it counts the names of its GL_EXTENSIONS: %s\n",
           yes(value == -7 && real == -7),
           yes(es2_count == words(es2_extensions) && !listed(es2_extensions, "GL_ANGLE_blob_cache")));
    eglMakeCurrent(display, pbuffer, pbuffer, context);
    eglDestroyContext(display, es2);
}

/* The names of both tables, from tools/frontgen's lists. */
#define MINATO_PROC(name, function) #name,
static const char *const egl_names[] = {
#include "egl_procs.inc"
};
static const char *const gles_names[] = {
#include "gles_procs.inc"
};
#undef MINATO_PROC

static void procs(void) {
    printf("eglGetProcAddress:\n");
    void *egl = dlopen("libEGL.so", RTLD_NOW), *gles = dlopen("libGLESv2.so", RTLD_NOW);
    Dl_info info;
    const void *egl_base = dladdr((void *)eglGetProcAddress, &info) ? info.dli_fbase : NULL;
    const void *gles_base = dladdr(dlsym(gles, "glFlush"), &info) ? info.dli_fbase : NULL;
    const struct {
        const char *name, *library;
        const char *const *names;
        size_t count;
        void *handle;
        const void *base;
    } libs[] = {
        {"EGL", "libEGL", egl_names, sizeof egl_names / sizeof *egl_names, egl, egl_base},
        {"GLES", "libGLESv2", gles_names, sizeof gles_names / sizeof *gles_names, gles, gles_base},
    };
    for (size_t lib = 0; lib < 2; lib++) {
        unsigned exported = 0, inside = 0, elsewhere = 0, wrong = 0;
        for (size_t index = 0; index < libs[lib].count; index++) {
            const char *name = libs[lib].names[index];
            void *function = (void *)eglGetProcAddress(name), *export = dlsym(libs[lib].handle, name);
            if (export != NULL) {
                exported += function == export;
                wrong += function != export;
            } else if (function != NULL && dladdr(function, &info) && info.dli_fbase == libs[lib].base) {
                inside++;
            } else {
                elsewhere++;
                if (elsewhere <= 3) {
                    printf("  %s: not in %s\n", name, libs[lib].library);
                }
            }
        }
        printf("  %s: %zu names, %u exported by %s and handed out as its exports, %u inside it, %u elsewhere, "
               "%u other than the export\n",
               libs[lib].name, libs[lib].count, exported, libs[lib].library, inside, elsewhere, wrong);
    }
    const char *const others[] = {"glNotAFunction", "eglDebugMessageControlKHR", "glBlobCacheCallbacksANGLE",
                                  "eglSetBlobCacheFuncsANDROID", "EGL_GetProcAddress", ""};
    int null = eglGetProcAddress(NULL) == NULL;
    for (size_t index = 0; index < sizeof others / sizeof *others; index++) {
        null &= eglGetProcAddress(others[index]) == NULL;
    }
    printf("  NULL for NULL, \"\", glNotAFunction, eglDebugMessageControlKHR, glBlobCacheCallbacksANGLE, "
           "eglSetBlobCacheFuncsANDROID and EGL_GetProcAddress: %s\n", yes(null));
}

static struct {
    unsigned calls, others;
    GLuint id;
    char text[64];
    const void *user;
    pthread_t thread;
} heard;

static void GL_APIENTRY hear(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                             const GLchar *text, const void *user) {
    (void)type, (void)severity, (void)length;
    if (source != GL_DEBUG_SOURCE_APPLICATION_KHR) {
        heard.others++;
        return;
    }
    heard.calls++;
    heard.id = id;
    snprintf(heard.text, sizeof heard.text, "%s", text);
    heard.user = user;
    heard.thread = pthread_self();
}

static void debug_output(EGLDisplay display, EGLSurface pbuffer, EGLContext context) {
    printf("debug output:\n");
    static int user, other;
    void *function = &user, *data = &user;
    glGetPointervKHR(GL_DEBUG_CALLBACK_FUNCTION_KHR, &function);
    glGetPointervKHR(GL_DEBUG_CALLBACK_USER_PARAM_KHR, &data);
    printf("  before any callback: function %s, user data %s\n", function == NULL ? "NULL" : "set",
           data == NULL ? "NULL" : "set");
    glDebugMessageCallbackKHR(hear, &user);
    glGetPointervKHR(GL_DEBUG_CALLBACK_FUNCTION_KHR, &function);
    glGetPointervKHR(GL_DEBUG_CALLBACK_USER_PARAM_KHR, &data);
    void *core_function = &other, *core_data = &other;
    glGetPointerv(GL_DEBUG_CALLBACK_FUNCTION, &core_function);
    glGetPointerv(GL_DEBUG_CALLBACK_USER_PARAM, &core_data);
    const GLenum core_error = glGetError();
    printf("  glGetPointervKHR after glDebugMessageCallbackKHR: the app's function %s, its user data %s; "
           "glGetPointerv of ES 3.2: %s, the values kept %s\n",
           yes(function == (void *)hear), yes(data == &user),
           core_error == GL_INVALID_OPERATION ? "GL_INVALID_OPERATION" : "another answer",
           yes(core_function == &other && core_data == &other));
    glEnable(GL_DEBUG_OUTPUT_KHR);
    glDebugMessageInsertKHR(GL_DEBUG_SOURCE_APPLICATION_KHR, GL_DEBUG_TYPE_MARKER_KHR, 42,
                            GL_DEBUG_SEVERITY_NOTIFICATION_KHR, -1, "on the guest's thread");
    printf("  a message on this guest thread: %u call of the app's callback, ID %u, \"%s\", its user data %s, on "
           "this thread %s\n",
           heard.calls, heard.id, heard.text, yes(heard.user == &user),
           yes(heard.calls != 0 && pthread_equal(heard.thread, pthread_self())));
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    const uint64_t done = minato_test(MINATO_TEST_HOST_DEBUG, (uint64_t)(uintptr_t)display,
                                      (uint64_t)(uintptr_t)pbuffer, (uint64_t)(uintptr_t)context);
    eglMakeCurrent(display, pbuffer, pbuffer, context);
    printf("  a message on a thread of the host's own: inserted %s, calls of the app's callback still %u\n",
           yes(done == 1), heard.calls);
    glDebugMessageCallbackKHR(NULL, &other);
    glGetPointervKHR(GL_DEBUG_CALLBACK_FUNCTION_KHR, &function);
    glGetPointervKHR(GL_DEBUG_CALLBACK_USER_PARAM_KHR, &data);
    printf("  after glDebugMessageCallbackKHR(NULL, other data): function %s, the other data %s\n",
           function == NULL ? "NULL" : "set", yes(data == &other));
    glDebugMessageCallback(hear, &user);
    const GLenum error = glGetError();
    glGetPointervKHR(GL_DEBUG_CALLBACK_USER_PARAM_KHR, &data);
    printf("  glDebugMessageCallback of ES 3.2 in an ES 3.0 context: %s, the other data kept %s\n",
           error == GL_INVALID_OPERATION ? "GL_INVALID_OPERATION" : "no error", yes(data == &other));
}

typedef struct ANativeWindow *(*create_window)(uint64_t, int32_t, int32_t);

static void frame(float red) {
    glClearColor(red, 0.5f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void drawable(const char *what) {
    const uint64_t size = minato_test(MINATO_TEST_DRAWABLE_SIZE, 0, 0, 0);
    printf("%s drawable %llu x %llu", what, (unsigned long long)(size >> 32), (unsigned long long)(size & 0xffffffffu));
}

static void window_surfaces(EGLDisplay display, EGLConfig config, EGLContext context) {
    printf("window surfaces:\n");
    const create_window create =
        (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    const uint64_t id = minato_test(MINATO_TEST_WINDOW, 0, 0, 0);
    struct ANativeWindow *window = create != NULL && id != 0 ? create(id, 64, 48) : NULL;
    EGLSurface surface = eglCreateWindowSurface(display, config, window, NULL);
    EGLint width = 0, height = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    printf("  on the window: %s, %d x %d,", surface != EGL_NO_SURFACE ? "a surface" : "EGL_NO_SURFACE", width, height);
    drawable("");
    printf("\n");
    eglMakeCurrent(display, surface, surface, context);

    ANativeWindow_setBuffersGeometry(window, 32, 16, WINDOW_FORMAT_RGBA_8888);
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    printf("  setBuffersGeometry(32, 16): %d x %d;", width, height);
    frame(0.0f);
    const EGLBoolean first = eglSwapBuffers(display, surface);
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    printf(" after the next swap (%s) %d x %d,", first ? "EGL_TRUE" : "EGL_FALSE", width, height);
    drawable("");
    frame(0.25f);
    eglSwapBuffers(display, surface);
    drawable("; after the swap after it");
    printf("\n");

    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    frame(0.5f);
    eglSwapBuffers(display, surface);
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    printf("  setBuffersGeometry(0, 0): after the next swap %d x %d;", width, height);
    frame(0.75f);
    const EGLint damage[] = {0, 0, 8, 8};
    const EGLBoolean damaged = eglSwapBuffersWithDamageKHR(display, surface, damage, 1);
    drawable(" after eglSwapBuffersWithDamageKHR");
    printf(" (%s)\n", damaged ? "EGL_TRUE" : "EGL_FALSE");
    printf("  presents counted: %llu\n", (unsigned long long)minato_test(MINATO_TEST_PRESENTS, 0, 0, 0));
    printf("  eglPresentationTimeANDROID: %s\n",
           eglPresentationTimeANDROID(display, surface, 1000000) ? "EGL_TRUE" : "EGL_FALSE");
    minato_test(MINATO_TEST_VSYNC_GRID, 1000000000, 10000000, 0);
    const PFNEGLPRESENTATIONTIMEANDROIDPROC set_time =
        (PFNEGLPRESENTATIONTIMEANDROIDPROC)eglGetProcAddress("eglPresentationTimeANDROID");
    /* Far on the grid, then near, 2 periods after the grid's tick, then far with the measurement's switch. */
    const struct {
        EGLnsecsANDROID time;
        int without;
        const char *what;
    } cases[] = {{2000000000, 0, "through eglGetProcAddress at 2 s, on a grid of 10 ms from 1 s"},
                 {1020000000, 0, "at 1.02 s, 2 periods after the grid's tick"},
                 {2000000000, 1, "at 2 s with MINATO_PRESENTS_WITHOUT_TIME=1"}};
    for (size_t index = 0; index < sizeof cases / sizeof *cases; index++) {
        if (cases[index].without) {
            setenv("MINATO_PRESENTS_WITHOUT_TIME", "1", 1);
        }
        /* The earlier swaps' drawables first, then the one of this swap, which Metal hands its time (or none) once
           the command buffer is scheduled. */
        glFinish();
        usleep(50000);
        minato_test(MINATO_TEST_PRESENTED_AT, 1, 0, 0);
        const EGLBoolean timed = set_time != NULL && set_time(display, surface, cases[index].time);
        frame(1.0f);
        eglSwapBuffers(display, surface);
        uint64_t at = 0;
        for (int tries = 0; tries < 2000 && (at = minato_test(MINATO_TEST_PRESENTED_AT, 0, 0, 0)) == 0; tries++) {
            usleep(1000);
        }
        char where[48];
        snprintf(where, sizeof where, at == UINT64_MAX ? "without a time" : "at %llu ns", (unsigned long long)at);
        printf("  %s: %s, the drawable %s\n", cases[index].what, timed ? "EGL_TRUE" : "EGL_FALSE", where);
    }
    unsetenv("MINATO_PRESENTS_WITHOUT_TIME");

    static int not_a_window;
    const EGLSurface none = eglCreateWindowSurface(display, config, (EGLNativeWindowType)&not_a_window, NULL);
    printf("  on a pointer that is no window: %s, %s\n", none == EGL_NO_SURFACE ? "EGL_NO_SURFACE" : "a surface",
           egl_error(eglGetError()));
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    printf("  eglDestroySurface: %s\n", eglDestroySurface(display, surface) ? "EGL_TRUE" : "EGL_FALSE");
    ANativeWindow_release(window);
}

static _Atomic uint64_t handled_at;
static void handled(int signal) {
    (void)signal;
    atomic_store(&handled_at, now_ns());
}

struct nudge {
    pthread_t target;
    EGLDisplay display;
    EGLSyncKHR sync;
    uint64_t sent_at;
};

static void *nudge(void *argument) {
    struct nudge *nudge = argument;
    usleep(50000);
    nudge->sent_at = now_ns();
    pthread_kill(nudge->target, SIGUSR1);
    usleep(150000);
    eglSignalSyncKHR(nudge->display, nudge->sync, EGL_SIGNALED_KHR);
    return NULL;
}

/* EGL_ANGLE_metal_shared_event_sync's names, which the NDK's headers lack. */
#define EGL_SYNC_METAL_SHARED_EVENT_ANGLE 0x34D8
#define EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE 0x34D9
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE 0x34DA
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE 0x34DC

struct held {
    pthread_t target;
    uint64_t event, value, sent_at;
};

/* Signals the waiting thread after 50 ms, and sets the host's event 150 ms later. */
static void *release_later(void *argument) {
    struct held *held = argument;
    usleep(50000);
    held->sent_at = now_ns();
    pthread_kill(held->target, SIGUSR1);
    usleep(150000);
    minato_test(MINATO_TEST_EVENT_SIGNAL, held->event, held->value, 0);
    return NULL;
}

/* A fence behind a GPU wait for the host's event to reach `value`: glClientWaitSync's or, with `egl`,
   eglClientWaitSyncKHR's wait for it goes in slices, so a signal's handler runs meanwhile. */
static void held_fence(EGLDisplay display, uint64_t event, uint64_t value, int egl) {
    const EGLAttrib attributes[] = {EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE, (EGLAttrib)event,
                                    EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, (EGLAttrib)value,
                                    EGL_SYNC_CONDITION, EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE, EGL_NONE};
    const EGLSync gpu_wait = eglCreateSync(display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, attributes);
    eglWaitSync(display, gpu_wait, 0);
    frame(0.5f);
    struct held held = {pthread_self(), event, value, 0};
    atomic_store(&handled_at, 0);
    pthread_t thread;
    const uint64_t start = now_ns();
    pthread_create(&thread, NULL, release_later, &held);
    int satisfied;
    if (egl) {
        const EGLSyncKHR fence = eglCreateSyncKHR(display, EGL_SYNC_FENCE_KHR, NULL);
        satisfied = eglClientWaitSyncKHR(display, fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 2000000000u) ==
                    EGL_CONDITION_SATISFIED_KHR;
        eglDestroySyncKHR(display, fence);
    } else {
        const GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        satisfied = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000u) == GL_CONDITION_SATISFIED;
        glDeleteSync(fence);
    }
    const uint64_t took = now_ns() - start;
    pthread_join(thread, NULL);
    const uint64_t handled_ns = atomic_load(&handled_at);
    printf("  %s on a fence behind a GPU wait for an event of the host's: a signal's handler ran within 30 ms of its "
           "sending: %s; the wait went on until the fence signaled: %s\n",
           egl ? "eglClientWaitSyncKHR" : "glClientWaitSync",
           yes(handled_ns >= held.sent_at && handled_ns - held.sent_at < 30000000u),
           yes(gpu_wait != EGL_NO_SYNC && satisfied && took >= 190000000u));
    eglDestroySync(display, gpu_wait);
}

static void waits(EGLDisplay display) {
    printf("waits:\n");
    frame(1.0f);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    const GLenum waited = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000u);
    printf("  glClientWaitSync on a fence: %s\n",
           waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED ? "signaled" : "not signaled");
    glDeleteSync(fence);

    struct sigaction action = {.sa_handler = handled};
    sigaction(SIGUSR1, &action, NULL);
    struct nudge nudging = {pthread_self(), display, eglCreateSyncKHR(display, EGL_SYNC_REUSABLE_KHR, NULL), 0};
    pthread_t thread;
    const uint64_t start = now_ns();
    pthread_create(&thread, NULL, nudge, &nudging);
    const EGLint result = eglClientWaitSyncKHR(display, nudging.sync, 0, 2000000000u);
    const uint64_t took = now_ns() - start;
    pthread_join(thread, NULL);
    const uint64_t handled_ns = atomic_load(&handled_at);
    printf("  eglClientWaitSyncKHR on a reusable sync: a signal's handler ran within 30 ms of its sending: %s; the "
           "wait went on until the sync was signaled: %s\n",
           yes(handled_ns >= nudging.sent_at && handled_ns - nudging.sent_at < 30000000u),
           yes(result == EGL_CONDITION_SATISFIED_KHR && took >= 190000000u));
    eglDestroySyncKHR(display, nudging.sync);
    const uint64_t event = minato_test(MINATO_TEST_EVENT, 0, 0, 0);
    held_fence(display, event, 1, 0);
    held_fence(display, event, 2, 1);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    EGLDisplay display;
    EGLConfig config;
    displays(&display);
    configs(display, &config);
    const EGLint pbuffer_size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const EGLSurface pbuffer = eglCreatePbufferSurface(display, config, pbuffer_size);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    eglMakeCurrent(display, pbuffer, pbuffer, context);
    const char *renderer = (const char *)glGetString(GL_RENDERER), *gl_version = (const char *)glGetString(GL_VERSION);
    printf("  GL_RENDERER is ANGLE's Metal renderer: %s; GL_VERSION begins with OpenGL ES 3.0: %s\n",
           yes(renderer != NULL && strstr(renderer, "ANGLE Metal Renderer") != NULL),
           yes(gl_version != NULL && strncmp(gl_version, "OpenGL ES 3.0", 13) == 0));
    strings(display);
    other_counts(display, config, pbuffer, context);
    procs();
    debug_output(display, pbuffer, context);
    window_surfaces(display, config, context);
    eglMakeCurrent(display, pbuffer, pbuffer, context);
    waits(display);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, pbuffer);
    printf("eglTerminate: %s\n", eglTerminate(display) ? "EGL_TRUE" : "EGL_FALSE");
    return 0;
}
