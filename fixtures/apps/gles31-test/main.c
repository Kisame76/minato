// gles31-test (ADR-0044, test 2): a NativeActivity on native_app_glue that draws four fixed GLES 3.1 scenes into
// 64 x 64 targets, reads each back and logs with the tag "gles31-test" how much of it holds the exact values every
// conformant ES 3.1 implementation gives:
// 1. a compute shader writes an integer per invocation into a storage buffer and into an r32ui image;
// 2. a draw reads both, the buffer as a storage block in the fragment shader and the image by texelFetch, into an
//    RGBA8UI target, after the barriers ES 3.1 asks for;
// 3. two triangles with a flat integer per vertex: ES takes the last vertex's (the provoking vertex);
// 4. a triangle fan around the center, whose triangles take their last vertex's flat value too.
// Android 15's emulator stops at ES 3.0 on this Mac (ADR-0029), so these exact values are the reference. What
// differs by device (the renderer, the versions, the limits, times) goes to the tag "gles31-test-device", which is
// not compared. Each scene swaps once into the window of 256 x 256 buffers; then 120 more frames and
// ANativeActivity_finish. Without an ES 3.1 context it logs why and finishes at once. Beside the context's version
// it logs ro.opengles.version, which follows the session's backend (ADR-0044 d)).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "gles31-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "gles31-test-device", __VA_ARGS__)

enum { kSize = 64, kCount = kSize * kSize, kWindow = 256, kFrames = 120, kScenes = 4 };

// ---- Shaders

static const char *const compute_source =
    "#version 310 es\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(std430, binding = 0) buffer Values { uint values[]; };\n"
    "layout(r32ui, binding = 0) writeonly uniform highp uimage2D image;\n"
    "void main() {\n"
    "    uvec2 at = gl_GlobalInvocationID.xy;\n"
    "    values[at.y * 64u + at.x] = (at.x * 7u + at.y * 13u) ^ 0x5a5u;\n"
    "    imageStore(image, ivec2(at), uvec4(at.x * at.y + 3u, 0u, 0u, 0u));\n"
    "}\n";

// A quad over the whole target from gl_VertexID alone, as a strip of four.
static const char *const quad_vertex =
    "#version 310 es\n"
    "void main() {\n"
    "    vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));\n"
    "    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);\n"
    "}\n";

static const char *const read_fragment =
    "#version 310 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(std430, binding = 0) readonly buffer Values { uint values[]; };\n"
    "uniform highp usampler2D image;\n"
    "layout(location = 0) out uvec4 color;\n"
    "void main() {\n"
    "    ivec2 at = ivec2(gl_FragCoord.xy);\n"
    "    uint value = values[uint(at.y) * 64u + uint(at.x)];\n"
    "    uint texel = texelFetch(image, at, 0).r;\n"
    "    color = uvec4(value & 255u, (value >> 8) & 255u, texel & 255u, (texel >> 8) & 255u);\n"
    "}\n";

static const char *const flat_vertex =
    "#version 310 es\n"
    "layout(location = 0) in vec2 position;\n"
    "layout(location = 1) in uint value;\n"
    "flat out uint vertex_value;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    vertex_value = value;\n"
    "}\n";

static const char *const flat_fragment =
    "#version 310 es\n"
    "precision highp int;\n"
    "flat in uint vertex_value;\n"
    "layout(location = 0) out uvec4 color;\n"
    "void main() {\n"
    "    color = uvec4(vertex_value, 0u, 0u, 255u);\n"
    "}\n";

// ---- State

static struct {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    bool ready;  // the scenes ran
    int windows, frames_left, swaps, swapped;
} app_state = {EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT, false, 0, 0, 0, 0};

static GLuint values_buffer, image_texture, target_texture, target, attributes_buffer, vertex_array;
static GLuint integers[kCount * 4];
static uint8_t bytes[kCount * 4];

static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t buffer_value(uint32_t x, uint32_t y) {
    return (x * 7u + y * 13u) ^ 0x5a5u;
}

static uint32_t image_value(uint32_t x, uint32_t y) {
    return x * y + 3u;
}

static GLuint compile(GLenum type, const char *source, const char *name) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char text[512] = "";
        glGetShaderInfoLog(shader, sizeof text, NULL, text);
        LOG("%s: a shader does not compile: %s", name, text);
    }
    return shader;
}

static GLuint link(const char *name, GLuint first, GLuint second) {
    const GLuint program = glCreateProgram();
    glAttachShader(program, first);
    if (second != 0) glAttachShader(program, second);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char text[512] = "";
        glGetProgramInfoLog(program, sizeof text, NULL, text);
        LOG("%s: the program does not link: %s", name, text);
    }
    glDeleteShader(first);
    if (second != 0) glDeleteShader(second);
    return program;
}

static GLuint program(const char *name, const char *vertex, const char *fragment) {
    return link(name, compile(GL_VERTEX_SHADER, vertex, name), compile(GL_FRAGMENT_SHADER, fragment, name));
}

static void check_errors(int scene, const char *name) {
    GLenum first = GL_NO_ERROR, error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        if (first == GL_NO_ERROR) first = error;
    }
    if (first != GL_NO_ERROR) LOG("scene %d %s: GL error 0x%04x", scene, name, first);
}

// The 64 x 64 RGBA8UI target, as integers in `integers`.
static void read_target(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glReadPixels(0, 0, kSize, kSize, GL_RGBA_INTEGER, GL_UNSIGNED_INT, integers);
}

static void use_target(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, kSize, kSize);
    static const GLuint zero[4] = {0, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 0, zero);
}

static GLuint red_at(int x, int y) {
    return integers[((size_t)y * kSize + x) * 4];
}

// Each scene shows in the window as a clear of its own color, and swaps.
static void swap(int scene) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, kWindow, kWindow);
    glClearColor((float)(scene & 1), (float)((scene >> 1) & 1), (float)((scene >> 2) & 1), 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ++app_state.swaps;
    if (eglSwapBuffers(app_state.display, app_state.surface)) {
        ++app_state.swapped;
    } else {
        DEVICE("swap %d failed: EGL error 0x%04x", app_state.swaps, eglGetError());
    }
}

// ---- The scenes

static void scene_compute(void) {
    const GLuint compute = link("compute", compile(GL_COMPUTE_SHADER, compute_source, "compute"), 0);
    glGenBuffers(1, &values_buffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, values_buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, kCount * sizeof(GLuint), NULL, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, values_buffer);
    glGenTextures(1, &image_texture);
    glBindTexture(GL_TEXTURE_2D, image_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, kSize, kSize);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindImageTexture(0, image_texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
    glUseProgram(compute);
    glDispatchCompute(kSize / 8, kSize / 8, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT |
                    GL_FRAMEBUFFER_BARRIER_BIT);

    int buffer_right = 0;
    const GLuint *mapped =
        glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, kCount * sizeof(GLuint), GL_MAP_READ_BIT);
    if (mapped != NULL) {
        for (int index = 0; index < kCount; ++index) {
            buffer_right += mapped[index] == buffer_value((uint32_t)(index % kSize), (uint32_t)(index / kSize));
        }
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    // The image through a framebuffer of its own: integer formats read back as RGBA_INTEGER.
    GLuint image_framebuffer = 0;
    glGenFramebuffers(1, &image_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, image_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image_texture, 0);
    glReadPixels(0, 0, kSize, kSize, GL_RGBA_INTEGER, GL_UNSIGNED_INT, integers);
    int image_right = 0;
    for (int index = 0; index < kCount; ++index) {
        image_right += integers[index * 4] == image_value((uint32_t)(index % kSize), (uint32_t)(index / kSize));
    }
    glDeleteFramebuffers(1, &image_framebuffer);
    glDeleteProgram(compute);
    check_errors(1, "compute");
    LOG("scene 1 compute: buffer %d of %d, image %d of %d as expected", buffer_right, kCount, image_right, kCount);
}

static void scene_read(void) {
    GLint blocks = 0;
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &blocks);
    DEVICE("GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS: %d", blocks);
    const GLuint read = program("draw", quad_vertex, read_fragment);
    use_target();
    glUseProgram(read);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, image_texture);
    glUniform1i(glGetUniformLocation(read, "image"), 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, values_buffer);
    glBindVertexArray(vertex_array);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    read_target();
    int right = 0;
    for (int index = 0; index < kCount; ++index) {
        const uint32_t x = (uint32_t)(index % kSize), y = (uint32_t)(index / kSize);
        const uint32_t value = buffer_value(x, y), texel = image_value(x, y);
        const GLuint *pixel = integers + index * 4;
        right += pixel[0] == (value & 255u) && pixel[1] == ((value >> 8) & 255u) && pixel[2] == (texel & 255u) &&
                 pixel[3] == ((texel >> 8) & 255u);
        for (int channel = 0; channel < 4; ++channel) bytes[index * 4 + channel] = (uint8_t)pixel[channel];
    }
    glDeleteProgram(read);
    check_errors(2, "draw from buffer and image");
    LOG("scene 2 draw from buffer and image: %d of %d as expected, crc %08x", right, kCount,
        crc32(bytes, sizeof bytes));
}

// Draws `count` vertices of (x, y, value) as `mode` with the flat program.
static void draw_flat(GLenum mode, const float (*vertices)[3], int count) {
    float positions[16][2];
    GLuint values[16];
    for (int index = 0; index < count; ++index) {
        positions[index][0] = vertices[index][0];
        positions[index][1] = vertices[index][1];
        values[index] = (GLuint)vertices[index][2];
    }
    glBindVertexArray(vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, attributes_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof positions + sizeof values, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof positions, positions);
    glBufferSubData(GL_ARRAY_BUFFER, sizeof positions, sizeof values, values);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, 0, (const void *)sizeof positions);
    glDrawArrays(mode, 0, count);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

static void scene_flat(GLuint flat) {
    // Two triangles that halve the target along its diagonal; each vertex has its own value.
    static const float vertices[6][3] = {{-1, -1, 11}, {1, -1, 12}, {-1, 1, 13},
                                         {1, -1, 21}, {1, 1, 22}, {-1, 1, 23}};
    use_target();
    glUseProgram(flat);
    draw_flat(GL_TRIANGLES, vertices, 6);
    read_target();
    check_errors(3, "flat");
    LOG("scene 3 flat: triangle 1 gives %u, triangle 2 gives %u", red_at(10, 10), red_at(53, 53));
}

static void scene_fan(GLuint flat) {
    // A fan around the center over the target's corners: bottom, right, top and left triangle.
    static const float vertices[6][3] = {{0, 0, 1}, {-1, -1, 2}, {1, -1, 3}, {1, 1, 4}, {-1, 1, 5}, {-1, -1, 6}};
    use_target();
    glUseProgram(flat);
    draw_flat(GL_TRIANGLE_FAN, vertices, 6);
    read_target();
    check_errors(4, "fan");
    LOG("scene 4 fan: bottom %u, right %u, top %u, left %u", red_at(32, 4), red_at(60, 32), red_at(32, 60),
        red_at(4, 32));
}

static void scenes(void) {
    glGenVertexArrays(1, &vertex_array);
    glGenBuffers(1, &attributes_buffer);
    glGenTextures(1, &target_texture);
    glBindTexture(GL_TEXTURE_2D, target_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, kSize, kSize);
    glGenFramebuffers(1, &target);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0);

    scene_compute();
    swap(1);
    scene_read();
    swap(2);
    const GLuint flat = program("flat", flat_vertex, flat_fragment);
    scene_flat(flat);
    swap(3);
    scene_fan(flat);
    swap(4);
    glDeleteProgram(flat);
}

// ---- EGL

static void device_lines(const char *what, const char *text) {
    if (text == NULL) {
        DEVICE("%s: NULL", what);
        return;
    }
    const size_t length = strlen(text);
    for (size_t start = 0; start == 0 || start < length; start += 800) DEVICE("%s: %.800s", what, text + start);
}

static bool choose_config(EGLConfig *chosen) {
    static const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 0, EGL_NONE};
    EGLConfig configs[64];
    EGLint count = 0;
    if (!eglChooseConfig(app_state.display, attributes, configs, 64, &count)) return false;
    for (EGLint index = 0; index < count; ++index) {
        static const EGLint wanted[][2] = {{EGL_RED_SIZE, 8}, {EGL_GREEN_SIZE, 8}, {EGL_BLUE_SIZE, 8},
                                           {EGL_ALPHA_SIZE, 8}, {EGL_DEPTH_SIZE, 24}, {EGL_STENCIL_SIZE, 8}};
        bool matches = true;
        for (size_t attribute = 0; attribute < sizeof wanted / sizeof wanted[0]; ++attribute) {
            EGLint value = -1;
            eglGetConfigAttrib(app_state.display, configs[index], wanted[attribute][0], &value);
            matches = matches && value == wanted[attribute][1];
        }
        if (matches) {
            *chosen = configs[index];
            return true;
        }
    }
    return false;
}

static bool start_egl(ANativeWindow *window) {
    app_state.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(app_state.display, NULL, NULL)) {
        LOG("eglInitialize failed: 0x%04x", eglGetError());
        return false;
    }
    EGLConfig config;
    if (!choose_config(&config)) {
        LOG("no RGBA8888 config with depth 24 and stencil 8");
        return false;
    }
    LOG("config: RGBA 8 8 8 8, depth 24, stencil 8");
    EGLint visual = 0;
    eglGetConfigAttrib(app_state.display, config, EGL_NATIVE_VISUAL_ID, &visual);
    ANativeWindow_setBuffersGeometry(window, kWindow, kWindow, visual);
    app_state.surface = eglCreateWindowSurface(app_state.display, config, window, NULL);
    static const EGLint version[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
    app_state.context = eglCreateContext(app_state.display, config, EGL_NO_CONTEXT, version);
    if (app_state.surface == EGL_NO_SURFACE || app_state.context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(app_state.display, app_state.surface, app_state.surface, app_state.context)) {
        LOG("no window surface or ES 3.1 context: 0x%04x", eglGetError());
        return false;
    }
    EGLint width = 0, height = 0;
    eglQuerySurface(app_state.display, app_state.surface, EGL_WIDTH, &width);
    eglQuerySurface(app_state.display, app_state.surface, EGL_HEIGHT, &height);
    LOG("window surface: %d x %d", width, height);
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    LOG("context: ES %d.%d", major, minor);
    // The version that ActivityManager's reqGlEsVersion gives the app, which follows the session's backend
    // (ADR-0044 d)): ES 3.1 on ANGLE's Vulkan backend.
    char property[PROP_VALUE_MAX] = "";
    __system_property_get("ro.opengles.version", property);
    const long gles_version = strtol(property, NULL, 10);
    LOG("ro.opengles.version: %s, ES %ld.%ld", property, gles_version >> 16, gles_version & 0xffff);
    device_lines("GL_VENDOR", (const char *)glGetString(GL_VENDOR));
    device_lines("GL_RENDERER", (const char *)glGetString(GL_RENDERER));
    device_lines("GL_VERSION", (const char *)glGetString(GL_VERSION));
    device_lines("GL_SHADING_LANGUAGE_VERSION", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    device_lines("EGL_VERSION", eglQueryString(app_state.display, EGL_VERSION));
    return true;
}

static bool stop_egl(void) {
    bool all = true;
    if (app_state.display != EGL_NO_DISPLAY) {
        all &= eglMakeCurrent(app_state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
        if (app_state.context != EGL_NO_CONTEXT) {
            all &= eglDestroyContext(app_state.display, app_state.context) == EGL_TRUE;
        }
        if (app_state.surface != EGL_NO_SURFACE) {
            all &= eglDestroySurface(app_state.display, app_state.surface) == EGL_TRUE;
        }
        all &= eglTerminate(app_state.display) == EGL_TRUE;
    }
    app_state.display = EGL_NO_DISPLAY;
    app_state.surface = EGL_NO_SURFACE;
    app_state.context = EGL_NO_CONTEXT;
    app_state.ready = false;
    return all;
}

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static void on_command(struct android_app *app, int32_t command) {
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (++app_state.windows != 1) break;
            if (!start_egl(app->window)) {
                LOG("ANativeActivity_finish");
                ANativeActivity_finish(app->activity);
                break;
            }
            {
                const int64_t start = now();
                scenes();
                DEVICE("%d scenes in %.1f ms", kScenes, (double)(now() - start) / 1e6);
            }
            app_state.ready = true;
            app_state.frames_left = kFrames;
            break;
        case APP_CMD_TERM_WINDOW:
            if (app_state.display != EGL_NO_DISPLAY) {
                LOG("APP_CMD_TERM_WINDOW: EGL released: %s", stop_egl() ? "yes" : "no");
            }
            app_state.frames_left = 0;
            break;
        default:
            break;
    }
}

// One of the 120 frames: a clear of a color that changes each frame.
static void frame(struct android_app *app) {
    const int index = kFrames - app_state.frames_left;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, kWindow, kWindow);
    glClearColor((float)(index & 1), (float)((index >> 1) & 1), (float)((index >> 2) & 1), 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ++app_state.swaps;
    if (eglSwapBuffers(app_state.display, app_state.surface)) {
        ++app_state.swapped;
    } else {
        DEVICE("swap %d failed: EGL error 0x%04x", app_state.swaps, eglGetError());
    }
    if (--app_state.frames_left == 0) {
        LOG("swaps: %d of %d succeeded", app_state.swapped, app_state.swaps);
        LOG("ANativeActivity_finish");
        ANativeActivity_finish(app->activity);
    }
}

void android_main(struct android_app *app) {
    app->onAppCmd = on_command;
    LOG("android_main");
    while (!app->destroyRequested) {
        const bool drawing = app_state.ready && app_state.frames_left > 0;
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(drawing ? 0 : -1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        if (app_state.ready && app_state.frames_left > 0) frame(app);
    }
    LOG("android_main returns");
}
