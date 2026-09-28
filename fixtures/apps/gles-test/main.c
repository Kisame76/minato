// gles-test (ADR-0029, test 8; step B2): a NativeActivity on native_app_glue that draws a fixed sequence of
// twelve GLES 3.0 scenes, each into a 64 x 64 target and into its window of 256 x 256 buffers, reads each back
// with glReadPixels before the swap, and logs with the tag "gles-test" what every conformant GPU gives alike:
// a CRC-32 and four pixels per target, the records of transform feedback, an occlusion query and a fence, and
// whether its swaps succeeded. The scenes use only exact operations: clears and flat colors of 0 and 1,
// rectangles on pixel boundaries, depth and stencil tests with constant values, blending ONE/ZERO and ONE/ONE,
// and integer targets (RGBA8UI) for every other value, which reach the window only as single bits. What
// differs by device (the renderer, the versions, the extensions, the config's Android attributes, the
// window's size, times, the commands) goes to the tag "gles-test-device", which is not compared with Android.
// After the scenes it swaps 120 more frames and calls ANativeActivity_finish.
// The live variant (gles-live; ADR-0032, test 8; ADR-0036 a) is this library started at
// ANativeActivity_onCreateLive. It skips the scenes and draws a frame per AChoreographer callback while it has a
// window and the focus, a clear and a square that moves, until onDestroy. Each window gets a surface at its own
// size, and TERM_WINDOW takes only that. Every 5 s it asks the window for 120 and 60 frames a second in turn
// (ANativeWindow_setFrameRate, by dlsym above the minSdk) and logs the frame intervals of each rate. AKEYCODE_BACK
// finishes it. At onDestroy it logs its frames, windows and failures with the tag "gles-live".
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/choreographer.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "android_native_app_glue.h"

// The log's tags: the live variant has its own.
static const char *tag = "gles-test", *device_tag = "gles-test-device";

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, device_tag, __VA_ARGS__)

enum { kSize = 64, kWindow = 256, kFrames = 120 };

static const char *const command_names[] = {
    "APP_CMD_INPUT_CHANGED", "APP_CMD_INIT_WINDOW", "APP_CMD_TERM_WINDOW", "APP_CMD_WINDOW_RESIZED",
    "APP_CMD_WINDOW_REDRAW_NEEDED", "APP_CMD_CONTENT_RECT_CHANGED", "APP_CMD_GAINED_FOCUS", "APP_CMD_LOST_FOCUS",
    "APP_CMD_CONFIG_CHANGED", "APP_CMD_LOW_MEMORY", "APP_CMD_START", "APP_CMD_RESUME", "APP_CMD_SAVE_STATE",
    "APP_CMD_PAUSE", "APP_CMD_STOP", "APP_CMD_DESTROY",
};

// ---- Shaders. Positions come in units of the 64 x 64 grid, so every edge lies on a pixel boundary in both
// targets; gl_FragCoord.xy turned into integers gives the pixel.

static const char *const grid_vertex =
    "#version 300 es\n"
    "layout(location = 0) in ivec2 position;\n"
    "layout(location = 1) in ivec2 offset;\n"
    "uniform float depth;\n"
    "void main() {\n"
    "    gl_Position = vec4(vec2(position + offset) / 32.0 - 1.0, depth, 1.0);\n"
    "}\n";

static const char *const flat_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec4 color;\n"
    "out vec4 result;\n"
    "void main() { result = color; }\n";

// Integer arithmetic without division, with its constants from a uniform block.
static const char *const arithmetic_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(std140) uniform Values { uvec4 seed; uvec4 factor; };\n"
    "out uvec4 result;\n"
    "void main() {\n"
    "    uvec2 p = uvec2(gl_FragCoord.xy);\n"
    "    uint h = (p.x * factor.x + p.y * factor.y) ^ seed.x;\n"
    "    h ^= h >> 13;\n"
    "    h *= factor.z;\n"
    "    h ^= (h >> 15) + seed.y;\n"
    "    h = (h << 3) | (h >> 29);\n"
    "    result = uvec4(h, h >> 8, h >> 16, h >> 24) & 0xffu;\n"
    "}\n";

// texelFetch from an RGBA8 and an RGBA8UI texture of 16 x 16; the normalized values go back to integers.
static const char *const texel_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D colors;\n"
    "uniform highp usampler2D values;\n"
    "out uvec4 result;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy) >> 2;\n"
    "    uvec4 a = uvec4(round(texelFetch(colors, p, 0) * 255.0));\n"
    "    uvec4 b = texelFetch(values, p ^ ivec2(3, 5), 0);\n"
    "    result = (a ^ b.wzyx) & 0xffu;\n"
    "}\n";

static const char *const compressed_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D image;\n"
    "out uvec4 result;\n"
    "void main() {\n"
    "    result = uvec4(round(texelFetch(image, ivec2(gl_FragCoord.xy) >> 2, 0) * 255.0));\n"
    "}\n";

static const char *const attachments_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(location = 0) out uvec4 out0;\n"
    "layout(location = 1) out uvec4 out1;\n"
    "layout(location = 2) out uvec4 out2;\n"
    "layout(location = 3) out uvec4 out3;\n"
    "void main() {\n"
    "    uvec2 p = uvec2(gl_FragCoord.xy);\n"
    "    uint a = p.x * 64u + p.y;\n"
    "    out0 = uvec4(a, a >> 8, p.x, p.y) & 0xffu;\n"
    "    out1 = uvec4(p.x * p.y, p.x + p.y, p.x | p.y, p.x & p.y) & 0xffu;\n"
    "    out2 = uvec4(a * 2654435761u >> 24, a * 40503u, ~a, 7u) & 0xffu;\n"
    "    out3 = uvec4(p.x ^ 0x2au, p.y << 2, (p.x << 4) | (p.y >> 2), 255u) & 0xffu;\n"
    "}\n";

static const char *const instances_vertex =
    "#version 300 es\n"
    "layout(location = 0) in ivec2 position;\n"
    "layout(location = 1) in ivec2 offset;\n"
    "flat out uint instance;\n"
    "void main() {\n"
    "    instance = uint(gl_InstanceID);\n"
    "    gl_Position = vec4(vec2(position + offset) / 32.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *const instances_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "flat in uint instance;\n"
    "out uvec4 result;\n"
    "void main() {\n"
    "    uvec2 p = uvec2(gl_FragCoord.xy);\n"
    "    result = uvec4(instance * 16u + 1u, (p.x ^ p.y) & 0xffu, instance ^ 0xa5u, 255u);\n"
    "}\n";

// Transform feedback of integer records, 64 points with the rasterizer off.
static const char *const feedback_vertex =
    "#version 300 es\n"
    "layout(location = 0) in uvec4 value;\n"
    "flat out uvec4 record;\n"
    "void main() {\n"
    "    uint i = uint(gl_VertexID);\n"
    "    uvec4 v = value * 2654435761u + uvec4(i, i << 3, i << 7, i ^ 0x55u);\n"
    "    record = v ^ (v >> 16);\n"
    "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "}\n";

static const char *const feedback_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "out vec4 result;\n"
    "void main() { result = vec4(0.0); }\n";

// The records drawn back as points of one pixel: record i at column i, its row from the record.
static const char *const points_vertex =
    "#version 300 es\n"
    "layout(location = 0) in uvec4 record;\n"
    "flat out uvec4 color;\n"
    "void main() {\n"
    "    vec2 pixel = vec2(float(gl_VertexID), float((record.x >> 3) & 63u)) + 0.5;\n"
    "    gl_Position = vec4(pixel / 32.0 - 1.0, 0.0, 1.0);\n"
    "    gl_PointSize = 1.0;\n"
    "    color = record & 0xffu;\n"
    "}\n";

static const char *const points_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "flat in uvec4 color;\n"
    "out uvec4 result;\n"
    "void main() { result = color; }\n";

// An RGBA8 texture of 0 and 1 drawn 1:1 (shift 0) or four times as large (shift 2).
static const char *const show_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D image;\n"
    "uniform ivec2 origin;\n"
    "uniform int shift;\n"
    "out vec4 result;\n"
    "void main() { result = texelFetch(image, (ivec2(gl_FragCoord.xy) - origin) >> shift, 0); }\n";

// One bit of each channel of an RGBA8UI texture, four times as large: how integer results reach the window.
static const char *const bits_fragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp usampler2D image;\n"
    "uniform uint bit;\n"
    "out vec4 result;\n"
    "void main() { result = vec4((texelFetch(image, ivec2(gl_FragCoord.xy) >> 2, 0) >> bit) & 1u); }\n";

// ---- State

static struct {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    bool ready;        // the scenes' objects exist
    int windows;
    int frames_left;   // of the 120 after the scenes
    int swaps, swapped;
    int64_t frames_start;
} app_state = {EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT, false, 0, 0, 0, 0, 0};

static struct {
    GLuint flat, arithmetic, texel, compressed, attachments, instances, feedback, points, show, bits;
    GLint flat_color, flat_depth, show_origin, show_shift, bits_bit;
    GLuint quads, quad_buffer, instanced, square, offsets, values_array, values, records_array, records;
    GLuint target, target_color, depth_stencil;  // 64 x 64 RGBA8 with D24S8
    GLuint rects, rects_color;                   // scene 2's picture, the source of scene 11
    GLuint integers, integers_color;             // 64 x 64 RGBA8UI
    GLuint four, four_colors[4];                 // four RGBA8UI attachments
    GLuint uniforms, colors, integer_texture, etc2, copy;
    GLuint queries[2];
} gl;

static uint8_t pixels[kWindow * kWindow * 4];
static uint8_t four_pixels[4 * kSize * kSize * 4];
static uint32_t integers[kSize * kSize * 4];

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Bytes from a linear congruential generator, the same everywhere.
static void fill(uint8_t *bytes, size_t size, uint32_t seed) {
    for (size_t index = 0; index < size; ++index) {
        seed = seed * 1664525u + 1013904223u;
        bytes[index] = (uint8_t)(seed >> 24);
    }
}

// ---- Objects

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

static GLuint program(const char *name, const char *vertex_source, const char *fragment_source, const char *record) {
    const GLuint program = glCreateProgram();
    const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source, name);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source, name);
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    if (record != NULL) glTransformFeedbackVaryings(program, 1, &record, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char text[512] = "";
        glGetProgramInfoLog(program, sizeof text, NULL, text);
        LOG("%s: the program does not link: %s", name, text);
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return program;
}

// A texture of one level, sampled only by texelFetch: integer formats need NEAREST to be complete.
static GLuint texture(GLenum format, GLsizei width, GLsizei height) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}

static GLuint framebuffer(const GLuint *colors, int count, GLuint depth_stencil, const char *name) {
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    GLenum buffers[4];
    for (int index = 0; index < count; ++index) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_TEXTURE_2D, colors[index], 0);
        buffers[index] = GL_COLOR_ATTACHMENT0 + index;
    }
    glDrawBuffers(count, buffers);
    if (depth_stencil != 0) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_stencil);
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) LOG("%s: the framebuffer is incomplete: 0x%04x", name, status);
    return framebuffer;
}

static void make_objects(void) {
    gl.flat = program("flat", grid_vertex, flat_fragment, NULL);
    gl.arithmetic = program("integer arithmetic", grid_vertex, arithmetic_fragment, NULL);
    gl.texel = program("texelFetch", grid_vertex, texel_fragment, NULL);
    gl.compressed = program("ETC2", grid_vertex, compressed_fragment, NULL);
    gl.attachments = program("four attachments", grid_vertex, attachments_fragment, NULL);
    gl.instances = program("instancing", instances_vertex, instances_fragment, NULL);
    gl.feedback = program("transform feedback", feedback_vertex, feedback_fragment, "record");
    gl.points = program("points", points_vertex, points_fragment, NULL);
    gl.show = program("show", grid_vertex, show_fragment, NULL);
    gl.bits = program("bits", grid_vertex, bits_fragment, NULL);
    gl.flat_color = glGetUniformLocation(gl.flat, "color");
    gl.flat_depth = glGetUniformLocation(gl.flat, "depth");
    gl.show_origin = glGetUniformLocation(gl.show, "origin");
    gl.show_shift = glGetUniformLocation(gl.show, "shift");
    gl.bits_bit = glGetUniformLocation(gl.bits, "bit");
    glUseProgram(gl.texel);
    glUniform1i(glGetUniformLocation(gl.texel, "colors"), 0);
    glUniform1i(glGetUniformLocation(gl.texel, "values"), 1);
    glUseProgram(0);

    // Rectangles: one quad of two triangles, written before each draw.
    glGenVertexArrays(1, &gl.quads);
    glBindVertexArray(gl.quads);
    glGenBuffers(1, &gl.quad_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, gl.quad_buffer);
    glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(GLshort), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 2, GL_SHORT, 0, NULL);

    // Instancing: an 8 x 8 square, moved per instance by an attribute with divisor 1.
    static const GLshort square[12] = {0, 0, 8, 0, 8, 8, 0, 0, 8, 8, 0, 8};
    GLshort offsets[32];
    for (int instance = 0; instance < 16; ++instance) {
        offsets[2 * instance] = (GLshort)((instance & 3) * 16 + 4);
        offsets[2 * instance + 1] = (GLshort)((instance >> 2) * 16 + 4);
    }
    glGenVertexArrays(1, &gl.instanced);
    glBindVertexArray(gl.instanced);
    glGenBuffers(1, &gl.square);
    glBindBuffer(GL_ARRAY_BUFFER, gl.square);
    glBufferData(GL_ARRAY_BUFFER, sizeof square, square, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 2, GL_SHORT, 0, NULL);
    glGenBuffers(1, &gl.offsets);
    glBindBuffer(GL_ARRAY_BUFFER, gl.offsets);
    glBufferData(GL_ARRAY_BUFFER, sizeof offsets, offsets, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 2, GL_SHORT, 0, NULL);
    glVertexAttribDivisor(1, 1);

    // Transform feedback: 64 records of four uints in, 64 out, then drawn as points.
    uint8_t bytes[1024];
    fill(bytes, sizeof bytes, 4);
    glGenVertexArrays(1, &gl.values_array);
    glBindVertexArray(gl.values_array);
    glGenBuffers(1, &gl.values);
    glBindBuffer(GL_ARRAY_BUFFER, gl.values);
    glBufferData(GL_ARRAY_BUFFER, sizeof bytes, bytes, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 4, GL_UNSIGNED_INT, 0, NULL);
    glGenVertexArrays(1, &gl.records_array);
    glBindVertexArray(gl.records_array);
    glGenBuffers(1, &gl.records);
    glBindBuffer(GL_ARRAY_BUFFER, gl.records);
    glBufferData(GL_ARRAY_BUFFER, sizeof bytes, NULL, GL_DYNAMIC_READ);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 4, GL_UNSIGNED_INT, 0, NULL);
    glBindVertexArray(gl.quads);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // The uniform block of scene 5.
    static const uint32_t values[8] = {0x9e3779b9u, 0x7f4a7c15u, 0, 0, 0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu, 0};
    glGenBuffers(1, &gl.uniforms);
    glBindBuffer(GL_UNIFORM_BUFFER, gl.uniforms);
    glBufferData(GL_UNIFORM_BUFFER, sizeof values, values, GL_STATIC_DRAW);
    glUniformBlockBinding(gl.arithmetic, glGetUniformBlockIndex(gl.arithmetic, "Values"), 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, gl.uniforms);

    // Textures of 16 x 16: RGBA8, RGBA8UI and ETC2 RGB8, from the generator; the target of the copy.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    fill(bytes, sizeof bytes, 1);
    gl.colors = texture(GL_RGBA8, 16, 16);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
    fill(bytes, sizeof bytes, 2);
    gl.integer_texture = texture(GL_RGBA8UI, 16, 16);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, bytes);
    fill(bytes, 128, 3);
    gl.etc2 = texture(GL_COMPRESSED_RGB8_ETC2, 16, 16);
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_COMPRESSED_RGB8_ETC2, 128, bytes);
    gl.copy = texture(GL_RGBA8, 32, 32);

    // The targets.
    gl.target_color = texture(GL_RGBA8, kSize, kSize);
    glGenRenderbuffers(1, &gl.depth_stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, gl.depth_stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kSize, kSize);
    gl.target = framebuffer(&gl.target_color, 1, gl.depth_stencil, "RGBA8 with depth and stencil");
    gl.rects_color = texture(GL_RGBA8, kSize, kSize);
    gl.rects = framebuffer(&gl.rects_color, 1, 0, "RGBA8");
    gl.integers_color = texture(GL_RGBA8UI, kSize, kSize);
    gl.integers = framebuffer(&gl.integers_color, 1, 0, "RGBA8UI");
    for (int index = 0; index < 4; ++index) gl.four_colors[index] = texture(GL_RGBA8UI, kSize, kSize);
    gl.four = framebuffer(gl.four_colors, 4, 0, "four RGBA8UI");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGenQueries(2, gl.queries);
    glDisable(GL_DITHER);
}

// ---- Drawing

static void rect(int x0, int y0, int x1, int y1) {
    const GLshort corners[12] = {(GLshort)x0, (GLshort)y0, (GLshort)x1, (GLshort)y0, (GLshort)x1, (GLshort)y1,
                                 (GLshort)x0, (GLshort)y0, (GLshort)x1, (GLshort)y1, (GLshort)x0, (GLshort)y1};
    glBindVertexArray(gl.quads);
    glBindBuffer(GL_ARRAY_BUFFER, gl.quad_buffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof corners, corners);
    // A draw with the instanced array leaves attribute 1's current value undefined.
    glVertexAttribI4i(1, 0, 0, 0, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void flat(float red, float green, float blue, float alpha, float depth) {
    glUseProgram(gl.flat);
    glUniform4f(gl.flat_color, red, green, blue, alpha);
    glUniform1f(gl.flat_depth, depth);
}

// Scene 1, in a target of `size` pixels.
static void draw_clears(int size) {
    const int scale = size / kSize;
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(8 * scale, 16 * scale, 32 * scale, 40 * scale);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(32 * scale, 4 * scale, 28 * scale, 24 * scale);
    glClearColor(0, 1, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

static void draw_rectangles(void) {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    flat(1, 0, 0, 1, 0);
    rect(4, 4, 36, 28);
    flat(0, 1, 0, 1, 0);
    rect(20, 20, 60, 60);
    flat(1, 1, 1, 1, 0);
    rect(8, 40, 28, 60);
    flat(1, 0, 1, 0, 0);
    rect(40, 4, 60, 16);
}

// Window depths 0.25, 0.5 and 0.75 differ in every depth format; the stencil holds 0 or 1.
static void draw_depth_and_stencil(void) {
    glClearColor(0, 0, 0, 1);
    glClearDepthf(1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    flat(1, 0, 0, 1, 0);
    rect(4, 4, 40, 40);
    flat(0, 1, 0, 1, 0.5f);
    rect(20, 20, 60, 60);
    flat(0, 0, 1, 1, -0.5f);
    rect(30, 8, 50, 30);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    flat(0, 0, 0, 0, 0);
    rect(8, 44, 56, 52);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_EQUAL, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    flat(1, 1, 1, 1, 0);
    rect(0, 0, 64, 64);
    glDisable(GL_STENCIL_TEST);
}

static void draw_blending(void) {
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    flat(1, 0, 0, 1, 0);
    rect(4, 4, 44, 44);
    glBlendFunc(GL_ONE, GL_ONE);
    flat(0, 1, 0, 1, 0);
    rect(20, 20, 60, 60);
    flat(0, 0, 1, 0, 0);
    rect(12, 36, 52, 56);
    glDisable(GL_BLEND);
}

// Scene 12: with `queries`, the occluded and the visible square each inside an occlusion query.
static void draw_occlusion(bool queries) {
    glClearColor(0, 0, 0, 1);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    flat(1, 0, 0, 1, -0.5f);
    rect(8, 8, 56, 56);
    if (queries) glBeginQuery(GL_ANY_SAMPLES_PASSED, gl.queries[0]);
    flat(0, 1, 0, 1, 0.5f);
    rect(16, 16, 48, 48);
    if (queries) glEndQuery(GL_ANY_SAMPLES_PASSED);
    if (queries) glBeginQuery(GL_ANY_SAMPLES_PASSED, gl.queries[1]);
    flat(0, 0, 1, 1, -0.75f);
    rect(40, 40, 62, 62);
    if (queries) glEndQuery(GL_ANY_SAMPLES_PASSED);
    glDisable(GL_DEPTH_TEST);
}

static void full(GLuint program) {
    glUseProgram(program);
    rect(0, 0, kSize, kSize);
}

// Bit `bit` of an RGBA8UI texture into the bound framebuffer, four times as large.
static void show_bits(GLuint texture, unsigned bit) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUseProgram(gl.bits);
    glUniform1ui(gl.bits_bit, bit);
    rect(0, 0, kSize, kSize);
}

// ---- Reading back and logging

// Four pixels of the 64 grid, taken in the middle of their block in the window.
static const int places[4][2] = {{1, 1}, {62, 5}, {21, 42}, {48, 30}};

static void pixel_text(char *text, size_t size, const uint8_t *image, int width, int index, int x, int y) {
    const uint8_t *pixel = image + ((size_t)y * width + x) * 4;
    snprintf(text, size, "%s(%u,%u,%u,%u)", index == 0 ? "" : " ", pixel[0], pixel[1], pixel[2], pixel[3]);
}

static void report(int scene, const char *name, const char *where, const uint8_t *image, int size) {
    const int scale = size / kSize;
    char text[4][24];
    for (int index = 0; index < 4; ++index) {
        pixel_text(text[index], sizeof text[index], image, size, index, places[index][0] * scale + scale / 2,
                   places[index][1] * scale + scale / 2);
    }
    LOG("scene %d %s: %s crc %08x, %s%s%s%s", scene, name, where, crc32(image, (size_t)size * size * 4), text[0],
        text[1], text[2], text[3]);
}

static void read_colors(int size) {
    glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

// An RGBA8UI attachment as bytes.
static void read_integers(GLenum attachment, uint8_t *bytes) {
    glReadBuffer(attachment);
    glReadPixels(0, 0, kSize, kSize, GL_RGBA_INTEGER, GL_UNSIGNED_INT, integers);
    for (int index = 0; index < kSize * kSize * 4; ++index) bytes[index] = (uint8_t)integers[index];
    glReadBuffer(GL_COLOR_ATTACHMENT0);
}

static void check_errors(int scene, const char *name) {
    GLenum first = GL_NO_ERROR, error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        if (first == GL_NO_ERROR) first = error;
    }
    if (first != GL_NO_ERROR) LOG("scene %d %s: GL error 0x%04x", scene, name, first);
}

static void swap(void) {
    ++app_state.swaps;
    if (eglSwapBuffers(app_state.display, app_state.surface)) {
        ++app_state.swapped;
    } else {
        DEVICE("swap %d failed: EGL error 0x%04x", app_state.swaps, eglGetError());
    }
}

// ---- The scenes

enum target_kind { kColors, kIntegers, kFour };

// Draws scene `scene` into its 64 x 64 target, which it returns bound for reading.
static enum target_kind draw_target(int scene) {
    glBindFramebuffer(GL_FRAMEBUFFER, gl.target);
    glViewport(0, 0, kSize, kSize);
    switch (scene) {
        case 1:
            draw_clears(kSize);
            return kColors;
        case 2:
            glBindFramebuffer(GL_FRAMEBUFFER, gl.rects);
            draw_rectangles();
            return kColors;
        case 3:
            draw_depth_and_stencil();
            return kColors;
        case 4:
            draw_blending();
            return kColors;
        case 5:
            glBindFramebuffer(GL_FRAMEBUFFER, gl.integers);
            full(gl.arithmetic);
            return kIntegers;
        case 6:
            glBindFramebuffer(GL_FRAMEBUFFER, gl.integers);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gl.colors);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, gl.integer_texture);
            full(gl.texel);
            glActiveTexture(GL_TEXTURE0);
            return kIntegers;
        case 7:
            glBindFramebuffer(GL_FRAMEBUFFER, gl.integers);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gl.etc2);
            full(gl.compressed);
            return kIntegers;
        case 8:
            glBindFramebuffer(GL_FRAMEBUFFER, gl.four);
            full(gl.attachments);
            return kFour;
        case 9: {
            static const GLuint zero[4] = {0, 0, 0, 0};
            glBindFramebuffer(GL_FRAMEBUFFER, gl.integers);
            glClearBufferuiv(GL_COLOR, 0, zero);
            glUseProgram(gl.instances);
            glBindVertexArray(gl.instanced);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 16);
            glBindVertexArray(gl.quads);
            return kIntegers;
        }
        case 10: {
            glUseProgram(gl.feedback);
            glBindVertexArray(gl.values_array);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, gl.records);
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, 64);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
            glBindBuffer(GL_COPY_READ_BUFFER, gl.records);
            const uint32_t *records = glMapBufferRange(GL_COPY_READ_BUFFER, 0, 1024, GL_MAP_READ_BIT);
            if (records != NULL) {
                LOG("scene 10 transform feedback: 64 records crc %08x, first (%08x,%08x,%08x,%08x), last "
                    "(%08x,%08x,%08x,%08x)",
                    crc32((const uint8_t *)records, 1024), records[0], records[1], records[2], records[3],
                    records[252], records[253], records[254], records[255]);
                glUnmapBuffer(GL_COPY_READ_BUFFER);
            } else {
                LOG("scene 10 transform feedback: the records cannot be mapped");
            }
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
            static const GLuint zero[4] = {0, 0, 0, 0};
            glBindFramebuffer(GL_FRAMEBUFFER, gl.integers);
            glClearBufferuiv(GL_COLOR, 0, zero);
            glUseProgram(gl.points);
            glBindVertexArray(gl.records_array);
            glDrawArrays(GL_POINTS, 0, 64);
            glBindVertexArray(gl.quads);
            return kIntegers;
        }
        case 11:
            // A quarter of scene 2's picture blitted 1:1, another copied into a texture and drawn back.
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.rects);
            glBlitFramebuffer(0, 0, 32, 32, 32, 32, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gl.copy);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, 32, 32);
            glBindFramebuffer(GL_FRAMEBUFFER, gl.target);
            glUseProgram(gl.show);
            glUniform2i(gl.show_origin, 0, 32);
            glUniform1i(gl.show_shift, 0);
            rect(0, 32, 32, 64);
            return kColors;
        default: {
            draw_occlusion(true);
            const GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFinish();
            GLuint occluded = 2, visible = 2;
            glGetQueryObjectuiv(gl.queries[0], GL_QUERY_RESULT, &occluded);
            glGetQueryObjectuiv(gl.queries[1], GL_QUERY_RESULT, &visible);
            GLint status = 0;
            glGetSynciv(fence, GL_SYNC_STATUS, 1, NULL, &status);
            const GLenum waited = glClientWaitSync(fence, 0, 0);
            glDeleteSync(fence);
            LOG("scene 12 query and fence: occluded %u, visible %u, fence %s, wait %s", occluded, visible,
                status == GL_SIGNALED ? "GL_SIGNALED" : "GL_UNSIGNALED",
                waited == GL_ALREADY_SIGNALED ? "GL_ALREADY_SIGNALED" : "not GL_ALREADY_SIGNALED");
            return kColors;
        }
    }
}

// Draws scene `scene` into the window's framebuffer.
static void draw_window(int scene) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, kWindow, kWindow);
    switch (scene) {
        case 1:
            draw_clears(kWindow);
            break;
        case 2:
            draw_rectangles();
            break;
        case 3:
            draw_depth_and_stencil();
            break;
        case 4:
            draw_blending();
            break;
        case 5:
        case 6:
        case 7:
            show_bits(gl.integers_color, 7);
            break;
        case 8:
            show_bits(gl.four_colors[1], 0);
            break;
        case 9:
            show_bits(gl.integers_color, 4);
            break;
        case 10:
            show_bits(gl.integers_color, 0);
            break;
        case 11:
            // The target four times as large, and scene 2's picture blitted 1:1 into its corner.
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gl.target_color);
            glUseProgram(gl.show);
            glUniform2i(gl.show_origin, 0, 0);
            glUniform1i(gl.show_shift, 2);
            rect(0, 0, kSize, kSize);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.rects);
            glBlitFramebuffer(0, 0, kSize, kSize, 0, 0, kSize, kSize, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            break;
        default:
            draw_occlusion(false);
            break;
    }
}

static const char *const scene_names[] = {
    "", "clears and scissor", "rectangles", "depth and stencil", "blending", "integer arithmetic",
    "texelFetch", "ETC2", "four attachments", "instancing", "transform feedback", "blit and copy",
    "query and fence",
};

static void scene(int number) {
    const char *name = scene_names[number];
    switch (draw_target(number)) {
        case kColors:
            read_colors(kSize);
            report(number, name, "64 x 64", pixels, kSize);
            break;
        case kIntegers:
            read_integers(GL_COLOR_ATTACHMENT0, pixels);
            report(number, name, "64 x 64", pixels, kSize);
            break;
        case kFour: {
            // One CRC over the four attachments, and one pixel of each.
            const size_t one = (size_t)kSize * kSize * 4;
            for (int index = 0; index < 4; ++index) {
                read_integers(GL_COLOR_ATTACHMENT0 + index, four_pixels + index * one);
            }
            char text[4][24];
            for (int index = 0; index < 4; ++index) {
                pixel_text(text[index], sizeof text[index], four_pixels + index * one, kSize, index,
                           places[index][0], places[index][1]);
            }
            LOG("scene %d %s: 4 x 64 x 64 crc %08x, %s%s%s%s", number, name, crc32(four_pixels, 4 * one), text[0],
                text[1], text[2], text[3]);
            break;
        }
    }
    glUseProgram(0);
    draw_window(number);
    read_colors(kWindow);
    report(number, name, "window", pixels, kWindow);
    glUseProgram(0);
    check_errors(number, name);
    swap();
}

// ---- EGL and the window

static const char *yes(bool value) {
    return value ? "yes" : "no";
}

// Long strings in lines of at most 800 bytes.
static void device_lines(const char *what, const char *text) {
    if (text == NULL) {
        DEVICE("%s: NULL", what);
        return;
    }
    const size_t length = strlen(text);
    for (size_t start = 0; start == 0 || start < length; start += 800) {
        DEVICE("%s: %.800s", what, text + start);
    }
}

// An RGBA8888 config with depth 24 and stencil 8, without multisampling, as the ADR asks.
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
    EGLint visual = 0, recordable = -1, target = -1;
    eglGetConfigAttrib(app_state.display, config, EGL_NATIVE_VISUAL_ID, &visual);
    eglGetConfigAttrib(app_state.display, config, EGL_RECORDABLE_ANDROID, &recordable);
    eglGetConfigAttrib(app_state.display, config, EGL_FRAMEBUFFER_TARGET_ANDROID, &target);
    DEVICE("config: EGL_NATIVE_VISUAL_ID %d, EGL_RECORDABLE_ANDROID %d, EGL_FRAMEBUFFER_TARGET_ANDROID %d", visual,
           recordable, target);
    DEVICE("window: %d x %d, format %d", ANativeWindow_getWidth(window), ANativeWindow_getHeight(window),
           ANativeWindow_getFormat(window));
    const int32_t geometry = ANativeWindow_setBuffersGeometry(window, kWindow, kWindow, visual);
    DEVICE("window after setBuffersGeometry (%d): %d x %d, format %d", geometry, ANativeWindow_getWidth(window),
           ANativeWindow_getHeight(window), ANativeWindow_getFormat(window));
    app_state.surface = eglCreateWindowSurface(app_state.display, config, window, NULL);
    static const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    app_state.context = eglCreateContext(app_state.display, config, EGL_NO_CONTEXT, version);
    if (app_state.surface == EGL_NO_SURFACE || app_state.context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(app_state.display, app_state.surface, app_state.surface, app_state.context)) {
        LOG("no window surface or context: 0x%04x", eglGetError());
        return false;
    }
    EGLint width = 0, height = 0;
    eglQuerySurface(app_state.display, app_state.surface, EGL_WIDTH, &width);
    eglQuerySurface(app_state.display, app_state.surface, EGL_HEIGHT, &height);
    LOG("window surface: %d x %d", width, height);
    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    LOG("context: ES %d", major);
    device_lines("GL_VENDOR", (const char *)glGetString(GL_VENDOR));
    device_lines("GL_RENDERER", (const char *)glGetString(GL_RENDERER));
    device_lines("GL_VERSION", (const char *)glGetString(GL_VERSION));
    device_lines("GL_SHADING_LANGUAGE_VERSION", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    device_lines("GL_EXTENSIONS", (const char *)glGetString(GL_EXTENSIONS));
    device_lines("EGL_VERSION", eglQueryString(app_state.display, EGL_VERSION));
    device_lines("EGL_EXTENSIONS", eglQueryString(app_state.display, EGL_EXTENSIONS));
    return true;
}

// The NDK sample's teardown: 1 once every call succeeded.
static bool stop_egl(void) {
    bool all = true;
    if (app_state.display != EGL_NO_DISPLAY) {
        all &= eglMakeCurrent(app_state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
        if (app_state.context != EGL_NO_CONTEXT) all &= eglDestroyContext(app_state.display, app_state.context) == EGL_TRUE;
        if (app_state.surface != EGL_NO_SURFACE) all &= eglDestroySurface(app_state.display, app_state.surface) == EGL_TRUE;
        all &= eglTerminate(app_state.display) == EGL_TRUE;
    }
    app_state.display = EGL_NO_DISPLAY;
    app_state.surface = EGL_NO_SURFACE;
    app_state.context = EGL_NO_CONTEXT;
    app_state.ready = false;
    return all;
}

// ---- The live variant (gles-live)

enum { kPhaseSeconds = 5, kMaxIntervals = 4096 };

static struct {
    bool on;       // started at ANativeActivity_onCreateLive
    AChoreographer *choreographer;
    EGLConfig config;
    bool ready;    // the window's surface is current
    bool focused;
    bool lost;     // a swap found the window gone: no frames until the next window
    int windows, frames, failures, losses;
    float rate;            // the frames a second the window is asked for: 120 or 60
    int64_t phase_start;   // the frame time the rate's phase began, 0 before the first callback
    int64_t previous;      // the frame time of the frame before, 0 after a break
    int interval_count;
    double intervals[kMaxIntervals];  // ms
} live = {.rate = 120};

// The live variant's entry, which its manifest names (android.app.func_name).
__attribute__((visibility("default"))) void ANativeActivity_onCreateLive(ANativeActivity *activity, void *saved_state,
                                                                         size_t saved_state_size) {
    live.on = true;
    tag = "gles-live";
    device_tag = "gles-live-device";
    ANativeActivity_onCreate(activity, saved_state, saved_state_size);
}

static int compare_doubles(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// The frame intervals of the rate's phase: the median and the 99th percentile by nearest rank, and the longest.
static void log_phase(void) {
    const int count = live.interval_count;
    if (count == 0) return;
    qsort(live.intervals, (size_t)count, sizeof live.intervals[0], compare_doubles);
    DEVICE("rate %.0f: %d frame intervals, median %.2f ms, p99 %.2f ms, max %.2f ms", live.rate, count,
           live.intervals[(count + 1) / 2 - 1], live.intervals[(99 * count + 99) / 100 - 1], live.intervals[count - 1]);
    live.interval_count = 0;
}

typedef int32_t (*set_frame_rate_function)(ANativeWindow *window, float rate, int8_t compatibility);

// ANativeWindow_setFrameRate came with API 30, above the minSdk: from libnativewindow by dlsym.
static void ask_rate(ANativeWindow *window) {
    static set_frame_rate_function set_frame_rate;
    static bool looked;
    if (!looked) {
        looked = true;
        void *library = dlopen("libnativewindow.so", RTLD_NOW);
        if (library != NULL) set_frame_rate = (set_frame_rate_function)dlsym(library, "ANativeWindow_setFrameRate");
        DEVICE("ANativeWindow_setFrameRate: %s", set_frame_rate != NULL ? "there" : "not there");
    }
    if (set_frame_rate != NULL) {
        DEVICE("ANativeWindow_setFrameRate(%.0f): %d", live.rate,
               set_frame_rate(window, live.rate, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT));
    }
}

// A window: EGL's display, config and context with the first, and for each a surface at the window's own size.
static void live_window(ANativeWindow *window) {
    ++live.windows;
    live.lost = false;
    static const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    if (app_state.context == EGL_NO_CONTEXT) {
        app_state.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!eglInitialize(app_state.display, NULL, NULL) || !choose_config(&live.config) ||
            (app_state.context = eglCreateContext(app_state.display, live.config, EGL_NO_CONTEXT, version)) ==
                EGL_NO_CONTEXT) {
            ++live.failures;
            DEVICE("no EGL display, config or context: 0x%04x", eglGetError());
            return;
        }
    }
    EGLint visual = 0, width = 0, height = 0;
    eglGetConfigAttrib(app_state.display, live.config, EGL_NATIVE_VISUAL_ID, &visual);
    const int32_t geometry = ANativeWindow_setBuffersGeometry(window, 0, 0, visual);
    app_state.surface = eglCreateWindowSurface(app_state.display, live.config, window, NULL);
    if (app_state.surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(app_state.display, app_state.surface, app_state.surface, app_state.context)) {
        ++live.failures;
        DEVICE("window %d: no surface: 0x%04x", live.windows, eglGetError());
        return;
    }
    live.ready = true;
    eglQuerySurface(app_state.display, app_state.surface, EGL_WIDTH, &width);
    eglQuerySurface(app_state.display, app_state.surface, EGL_HEIGHT, &height);
    DEVICE("window %d: setBuffersGeometry %d, surface %d x %d", live.windows, geometry, width, height);
    ask_rate(window);
}

// The window goes, and its surface with it; the display and the context stay.
static void live_window_gone(void) {
    live.ready = false;
    live.previous = 0;
    if (app_state.surface == EGL_NO_SURFACE) return;
    const bool released = eglMakeCurrent(app_state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) &&
                          eglDestroySurface(app_state.display, app_state.surface);
    if (!released) {
        ++live.failures;
        DEVICE("window %d: the surface's release failed: 0x%04x", live.windows, eglGetError());
    }
    app_state.surface = EGL_NO_SURFACE;
}

// A frame: a dark clear and a square that moves with the frame count, swapped.
static void live_frame(void) {
    EGLint width = 0, height = 0;
    eglQuerySurface(app_state.display, app_state.surface, EGL_WIDTH, &width);
    eglQuerySurface(app_state.display, app_state.surface, EGL_HEIGHT, &height);
    const int size = (width < height ? width : height) / 4;
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(width > size ? (live.frames * 8) % (width - size) : 0, (height - size) / 2, size, size);
    glClearColor(1, 0.6f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        ++live.failures;
        DEVICE("frame %d: GL error 0x%04x", live.frames, error);
    }
    if (eglSwapBuffers(app_state.display, app_state.surface)) {
        ++live.frames;
        return;
    }
    const EGLint swap_error = eglGetError();
    if (swap_error == EGL_BAD_NATIVE_WINDOW || swap_error == EGL_BAD_SURFACE) {
        ++live.losses;
        live.lost = true;
        DEVICE("frame %d: the window is gone (0x%04x)", live.frames, swap_error);
    } else {
        ++live.failures;
        DEVICE("frame %d: eglSwapBuffers failed: 0x%04x", live.frames, swap_error);
    }
}

// A frame per callback while the app has a window and the focus; every 5 s of frame time the other rate.
static void on_frame(int64_t frame_time, void *data) {
    struct android_app *app = data;
    if (app->destroyRequested) return;
    AChoreographer_postFrameCallback64(live.choreographer, on_frame, app);
    if (live.phase_start == 0) live.phase_start = frame_time;
    if (frame_time - live.phase_start >= kPhaseSeconds * 1000000000LL) {
        log_phase();
        live.rate = live.rate == 120 ? 60 : 120;
        live.phase_start = frame_time;
        live.previous = 0;
        if (live.ready) ask_rate(app->window);
    }
    if (!live.ready || !live.focused || live.lost) {
        live.previous = 0;
        return;
    }
    if (live.previous != 0 && live.interval_count < kMaxIntervals) {
        live.intervals[live.interval_count++] = (double)(frame_time - live.previous) / 1e6;
    }
    live.previous = frame_time;
    live_frame();
}

// BACK finishes the live variant: from Android 12 on, BACK alone only moves a root activity to the back.
static int32_t on_input(struct android_app *app, AInputEvent *event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY || AKeyEvent_getKeyCode(event) != AKEYCODE_BACK) return 0;
    if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP) {
        LOG("AKEYCODE_BACK: ANativeActivity_finish");
        ANativeActivity_finish(app->activity);
    }
    return 1;
}

static void on_live_command(struct android_app *app, int32_t command) {
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            live_window(app->window);
            break;
        case APP_CMD_TERM_WINDOW:
            live_window_gone();
            break;
        case APP_CMD_GAINED_FOCUS:
            live.focused = true;
            break;
        case APP_CMD_LOST_FOCUS:
            live.focused = false;
            live.previous = 0;  // a break, no frame interval
            break;
        case APP_CMD_DESTROY:
            log_phase();
            if (!stop_egl()) ++live.failures;
            LOG("onDestroy: frames %d, windows %d, failures %d, lost %d", live.frames, live.windows, live.failures,
                live.losses);
            break;
        default:
            break;
    }
}

// The live variant's loop: the frames come from the choreographer's callbacks inside ALooper_pollOnce.
static void live_main(struct android_app *app) {
    app->onInputEvent = on_input;
    live.choreographer = AChoreographer_getInstance();
    if (live.choreographer == NULL) {
        ++live.failures;
        LOG("no AChoreographer");
    } else {
        AChoreographer_postFrameCallback64(live.choreographer, on_frame, app);
    }
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(-1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
    }
}

static void on_command(struct android_app *app, int32_t command) {
    if (command >= 0 && command < (int32_t)(sizeof command_names / sizeof command_names[0])) {
        DEVICE("%s", command_names[command]);
    }
    if (live.on) {
        on_live_command(app, command);
        return;
    }
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (++app_state.windows == 1 && start_egl(app->window)) {
                const int64_t start = now();
                make_objects();
                app_state.ready = true;
                for (int number = 1; number <= 12; ++number) scene(number);
                DEVICE("12 scenes in %.1f ms", (double)(now() - start) / 1e6);
                app_state.frames_left = kFrames;
                app_state.frames_start = now();
            }
            break;
        case APP_CMD_TERM_WINDOW:
            if (app_state.display != EGL_NO_DISPLAY) LOG("APP_CMD_TERM_WINDOW: EGL released: %s", yes(stop_egl()));
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
    swap();
    if (--app_state.frames_left == 0) {
        DEVICE("%d frames in %.1f ms", kFrames, (double)(now() - app_state.frames_start) / 1e6);
        LOG("swaps: %d of %d succeeded", app_state.swapped, app_state.swaps);
        LOG("ANativeActivity_finish");
        ANativeActivity_finish(app->activity);
    }
}

void android_main(struct android_app *app) {
    app->onAppCmd = on_command;
    LOG("android_main");
    if (live.on) live_main(app);
    while (!app->destroyRequested) {
        const bool drawing = app_state.ready && app_state.frames_left > 0;
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(drawing ? 0 : -1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        if (app_state.ready && app_state.frames_left > 0) frame(app);
    }
    LOG("android_main returns");
}
