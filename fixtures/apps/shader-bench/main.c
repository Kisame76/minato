// shader-bench (M6, the shader cache plan): a NativeActivity on native_app_glue that builds two sets of GLSL ES 3.0
// programs on the window's context and logs with the tag "shader-bench" how long it took:
// - the fixed set, 40 programs with one to six lights and a shadow filter loop each, the same sources in every
//   launch: a second launch builds them faster when the system keeps the compiled shaders between launches;
// - the fresh set, 20 programs of the same kind whose sources carry a number taken from the clock, which no earlier
//   launch has compiled: the cost of a compile that nothing could have cached.
// Each line holds the links and the first draws (with glFinish) in milliseconds. Times differ by device and run, so
// the log is not compared with Android's; the numbers are read by hand. Then it swaps 3 frames and calls
// ANativeActivity_finish.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "shader-bench", __VA_ARGS__)

enum { kFixed = 40, kFresh = 20, kSource = 4096 };

static const char *const vertex_source =
    "#version 300 es\n"
    "layout(location = 0) in vec3 p; layout(location = 1) in vec3 n; layout(location = 2) in vec2 t;\n"
    "uniform mat4 m; out vec3 vn; out vec2 uv; out vec3 vp;\n"
    "void main() { vn = n; uv = t; vp = p; gl_Position = m * vec4(p, 1.0); }\n";

static double now_ms(void) {
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    return at.tv_sec * 1000.0 + at.tv_nsec / 1e6;
}

// A fragment shader whose light count, filter taps and constants follow `i`.
static void fragment_source(char *out, int i) {
    int lights = 1 + i % 6, taps = 2 + i % 5, at = 0;
    at += snprintf(out + at, kSource - at,
                   "#version 300 es\nprecision highp float;\nin vec3 vn; in vec2 uv; in vec3 vp;\n"
                   "uniform sampler2D tex; uniform sampler2D nmap; uniform sampler2D shadow;\n"
                   "uniform vec4 lightpos[8]; uniform vec4 lightcol[8]; uniform float k;\nout vec4 color;\n"
                   "float pcf(vec2 p, float d) { float s = 0.0;\n"
                   "  for (int x = 0; x < %d; x++) for (int y = 0; y < %d; y++)\n"
                   "    s += texture(shadow, p + vec2(float(x), float(y)) * 0.002).r < d ? 0.0 : 1.0;\n"
                   "  return s / %d.0; }\n"
                   "void main() {\n  vec3 n = normalize(vn + (texture(nmap, uv).xyz * 2.0 - 1.0) * 0.3);\n"
                   "  vec3 acc = vec3(0.0);\n",
                   taps, taps, taps * taps);
    for (int l = 0; l < lights; l++) {
        at += snprintf(out + at, kSource - at,
                       "  { vec3 d = lightpos[%d].xyz - vp; float dist = length(d); d /= dist;\n"
                       "    float att = 1.0 / (1.0 + dist * dist * lightpos[%d].w);\n"
                       "    float nd = max(dot(n, d), 0.0); vec3 h = normalize(d + vec3(0.0, 0.0, 1.0));\n"
                       "    float sp = pow(max(dot(n, h), 0.0), %d.0);\n"
                       "    acc += lightcol[%d].rgb * (nd + sp * %d.%d) * att\n"
                       "         * pcf(uv * 0.5, vp.z * 0.01 + %d.0 * 0.001); }\n",
                       l, l, 8 + 4 * (i % 7), l, i % 3, 1 + i % 9, i);
    }
    snprintf(out + at, kSource - at, "  color = vec4(acc * texture(tex, uv).rgb * k, 1.0);\n}\n");
}

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    return shader;
}

// Builds programs first to last, one draw each; adds the links' and the draws' milliseconds to the totals.
static bool build_set(int first, int count, double *links, double *draws) {
    static char source[kSource];
    static const float triangle[] = {-1, -1, 0, 0, 0, 1, 0, 0, 1, -1, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0.5f, 1};
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
    for (int a = 0; a < 3; a++) glEnableVertexAttribArray(a);
    for (int i = first; i < first + count; i++) {
        double begin = now_ms();
        fragment_source(source, i);
        GLuint program = glCreateProgram();
        glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
        glAttachShader(program, compile(GL_FRAGMENT_SHADER, source));
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            LOG("program %d does not link", i);
            return false;
        }
        double linked_at = now_ms();
        glUseProgram(program);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        *links += linked_at - begin;
        *draws += now_ms() - linked_at;
    }
    return true;
}

static bool run(struct android_app *app) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) return false;
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                        EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count < 1) return false;
    EGLSurface surface = eglCreateWindowSurface(display, config, app->window, NULL);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
        return false;
    }
    LOG("renderer %s", (const char *)glGetString(GL_RENDERER));
    double fixed_links = 0, fixed_draws = 0, fresh_links = 0, fresh_draws = 0;
    if (!build_set(0, kFixed, &fixed_links, &fixed_draws)) return false;
    LOG("fixed %d programs: links %.1f ms, first draws %.1f ms", kFixed, fixed_links, fixed_draws);
    // 100000 and more are sources no earlier launch has seen: the clock's seconds make the first one.
    int seed = 100000 + (int)(time(NULL) % 100000) * kFresh;
    if (!build_set(seed, kFresh, &fresh_links, &fresh_draws)) return false;
    LOG("fresh %d programs: links %.1f ms, first draws %.1f ms", kFresh, fresh_links, fresh_draws);
    for (int frame = 0; frame < 3; frame++) {
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display, surface);
    }
    return true;
}

static void handle_command(struct android_app *app, int32_t command) {
    if (command != APP_CMD_INIT_WINDOW || app->window == NULL) return;
    LOG("%s", run(app) ? "done" : "failed");
    LOG("ANativeActivity_finish");
    ANativeActivity_finish(app->activity);
}

void android_main(struct android_app *app) {
    LOG("android_main");
    app->onAppCmd = handle_command;
    while (!app->destroyRequested) {
        int events;
        struct android_poll_source *source;
        while (ALooper_pollOnce(-1, NULL, &events, (void **)&source) >= 0) {
            if (source != NULL) source->process(app, source);
            if (app->destroyRequested) break;
        }
    }
    LOG("android_main returns");
}
