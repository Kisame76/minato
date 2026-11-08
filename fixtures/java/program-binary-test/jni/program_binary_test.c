// program-binary-test: GL program binaries across two starts, as a game engine's shader cache uses them. A
// NativeActivity whose worker thread, once the window has the focus, makes an ES 3 context on a pbuffer and then:
// - on its first start (no saved programs in its files folder): links four programs like an engine's (a plain one, one
//   with a uniform block, two samplers, a uniform array and instancing, one with transform feedback, and a GLSL ES 1.00
//   one), each with GL_PROGRAM_BINARY_RETRIEVABLE_HINT, draws with each and checks the pixels, then saves each
//   program's binary, its format and its length to programs.bin;
// - on its second start, a new process: loads each saved binary into a new program with glProgramBinary, checks the
//   link status, and draws with it and checks the pixels (an engine links from the source again when a binary is
//   refused, so a refused binary is drawn after that link); deletes programs.bin; then binaries that a driver must
//   refuse without a crash: one of a format not listed, an empty one, one with bytes after its end (which a driver
//   may take), one with its middle overwritten and one cut short; a program object that refused one links from the
//   source after.
// The lines under the tag program-binary-test hold on every Android device (whether a driver takes a saved binary
// back is its own choice); the values (formats, lengths, which binaries linked, errors) go to
// program-binary-test-device.
// Each start ends with "done" and finishes.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "program-binary-test"

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

static const char *gl_error_name(GLenum error) {
    switch (error) {
    case GL_NO_ERROR: return "GL_NO_ERROR";
    case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
    case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
    default: return "another error";
    }
}

/* The first error since the last call, the rest cleared. */
static GLenum take_error(void) {
    const GLenum first = glGetError();
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) {
    }
    return first;
}

/* ---- The programs */

enum { PLAIN, ENGINE, CAPTURE, LEGACY, PROGRAMS };

static const char *const names[PROGRAMS] = {"plain", "engine-like", "transform feedback", "GLSL ES 1.00"};

static const char *const vertex_sources[PROGRAMS] = {
    "#version 300 es\n"
    "layout(location = 0) in vec2 position;\n"
    "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",

    "#version 300 es\n"
    "layout(location = 0) in vec2 position;\n"
    "layout(location = 1) in vec2 uv;\n"
    "layout(std140) uniform Globals { mat4 transform; vec4 tint; };\n"
    "out vec2 v_uv;\n"
    "flat out int v_instance;\n"
    "void main() {\n"
    "  v_uv = uv;\n"
    "  v_instance = gl_InstanceID;\n"
    "  gl_Position = transform * vec4(position, 0.0, 1.0);\n"
    "}\n",

    "#version 300 es\n"
    "layout(location = 0) in vec2 position;\n"
    "uniform float scale;\n"
    "out vec4 captured;\n"
    "void main() {\n"
    "  captured = vec4(position * scale, float(gl_VertexID), 7.0);\n"
    "  gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n",

    "#version 100\n"
    "attribute vec2 position;\n"
    "varying mediump vec2 v_position;\n"
    "void main() { v_position = position; gl_Position = vec4(position, 0.0, 1.0); }\n",
};

static const char *const fragment_sources[PROGRAMS] = {
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "out vec4 result;\n"
    "void main() { result = color; }\n",

    "#version 300 es\n"
    "precision mediump float;\n"
    "layout(std140) uniform Globals { mat4 transform; vec4 tint; };\n"
    "uniform sampler2D albedo;\n"
    "uniform sampler2D mask;\n"
    "uniform vec4 parameters[4];\n"
    "in vec2 v_uv;\n"
    "flat in int v_instance;\n"
    "out vec4 result;\n"
    "void main() {\n"
    "  result = vec4(texture(albedo, v_uv).g * tint.r, texture(mask, v_uv).r * parameters[2].y,\n"
    "                float(v_instance), 1.0);\n"
    "}\n",

    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 result;\n"
    "void main() { result = vec4(0.0, 0.0, 1.0, 1.0); }\n",

    "#version 100\n"
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "varying mediump vec2 v_position;\n"
    "void main() { gl_FragColor = color + vec4(0.0 * v_position, 0.0, 0.0); }\n",
};

static GLuint shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char text[512] = "";
        glGetShaderInfoLog(shader, sizeof text, NULL, text);
        DEV("a shader did not compile: %s", text);
    }
    return shader;
}

static bool linked(GLuint program) {
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    return status == GL_TRUE;
}

/* Attaches program k's shaders (and its attribute and capture names) to `program` and links it. */
static bool link_from_source(GLuint program, int k) {
    GLuint vertex = shader(GL_VERTEX_SHADER, vertex_sources[k]);
    GLuint fragment = shader(GL_FRAGMENT_SHADER, fragment_sources[k]);
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    if (k == LEGACY) glBindAttribLocation(program, 0, "position");
    if (k == CAPTURE) {
        const char *captured[] = {"captured"};
        glTransformFeedbackVaryings(program, 1, captured, GL_INTERLEAVED_ATTRIBS);
    }
    glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(program);
    glDetachShader(program, vertex);
    glDetachShader(program, fragment);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return linked(program);
}

/* ---- Drawing and its check */

struct scene {
    GLuint framebuffer, target, quad, albedo, mask, globals, captured, feedback;
};

static GLuint texture_1x1(const GLubyte color[4]) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}

static void scene_make(struct scene *scene) {
    glGenFramebuffers(1, &scene->framebuffer);
    glGenTextures(1, &scene->target);
    glBindTexture(GL_TEXTURE_2D, scene->target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, scene->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene->target, 0);
    glViewport(0, 0, 8, 8);
    // A full-screen pair of triangles: position, uv.
    const GLfloat quad[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, -1, 1, 0, 1, 1, -1, 1, 0, 1, 1, 1, 1};
    glGenBuffers(1, &scene->quad);
    glBindBuffer(GL_ARRAY_BUFFER, scene->quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    const GLubyte green[4] = {0, 255, 0, 255}, red[4] = {255, 0, 0, 255};
    scene->albedo = texture_1x1(green);
    scene->mask = texture_1x1(red);
    // Globals: the identity transform and a tint whose red is 1.
    const GLfloat globals[20] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1};
    glGenBuffers(1, &scene->globals);
    glBindBuffer(GL_UNIFORM_BUFFER, scene->globals);
    glBufferData(GL_UNIFORM_BUFFER, sizeof globals, globals, GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, scene->globals);
    glGenBuffers(1, &scene->captured);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, scene->captured);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 6 * 4 * sizeof(GLfloat), NULL, GL_DYNAMIC_READ);
    glGenTransformFeedbacks(1, &scene->feedback);
}

static void scene_free(struct scene *scene) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &scene->framebuffer);
    GLuint textures[] = {scene->target, scene->albedo, scene->mask};
    glDeleteTextures(3, textures);
    GLuint buffers[] = {scene->quad, scene->globals, scene->captured};
    glDeleteBuffers(3, buffers);
    glDeleteTransformFeedbacks(1, &scene->feedback);
}

static bool near(const GLubyte *pixel, int r, int g, int b) {
    return abs(pixel[0] - r) <= 2 && abs(pixel[1] - g) <= 2 && abs(pixel[2] - b) <= 2;
}

/* Draws with program k (linked) after setting its uniforms by name, as an engine does after a load, and checks the
   pixels and, for the capture program, the captured values. */
static bool draw_checks(const struct scene *scene, GLuint program, int k) {
    glUseProgram(program);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLubyte pixel[4] = {0, 0, 0, 0};
    bool right = false;
    switch (k) {
    case PLAIN:
    case LEGACY: {
        const GLint color = glGetUniformLocation(program, "color");
        glUniform4f(color, 1.0f, 0.0f, 1.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        right = color >= 0 && near(pixel, 255, 0, 255);
        break;
    }
    case ENGINE: {
        const GLuint block = glGetUniformBlockIndex(program, "Globals");
        if (block != GL_INVALID_INDEX) glUniformBlockBinding(program, block, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene->albedo);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, scene->mask);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(glGetUniformLocation(program, "albedo"), 0);
        glUniform1i(glGetUniformLocation(program, "mask"), 3);
        const GLfloat parameters[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, 0};
        glUniform4fv(glGetUniformLocation(program, "parameters"), 4, parameters);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 2);
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        // Albedo's green times the tint's red, the mask's red times parameters[2].y, the last instance's ID.
        right = block != GL_INVALID_INDEX && near(pixel, 255, 128, 255);
        break;
    }
    case CAPTURE: {
        glUniform1f(glGetUniformLocation(program, "scale"), 2.0f);
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, scene->feedback);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, scene->captured);
        glBeginTransformFeedback(GL_TRIANGLES);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glEndTransformFeedback();
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        const GLfloat *captured = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 6 * 4 * sizeof(GLfloat),
                                                   GL_MAP_READ_BIT);
        // Vertex 5 is (1, 1): captured (2, 2, 5, 7).
        right = captured != NULL && captured[20] == 2.0f && captured[21] == 2.0f && captured[22] == 5.0f &&
                captured[23] == 7.0f && near(pixel, 0, 0, 255);
        if (captured != NULL) glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
        break;
    }
    default:
        break;
    }
    glUseProgram(0);
    if (!right) DEV("%s drew %d %d %d", names[k], pixel[0], pixel[1], pixel[2]);
    return right && take_error() == GL_NO_ERROR;
}

/* ---- The saved binaries: per program its format, its length and its bytes */

struct saved {
    GLenum format;
    GLsizei length;
    void *bytes;
};

static void path_of(char *path, size_t size) {
    snprintf(path, size, "%s/programs.bin", g_activity->internalDataPath);
}

static bool load_saved(struct saved saved[PROGRAMS]) {
    char path[512];
    path_of(path, sizeof path);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    bool whole = true;
    for (int k = 0; k < PROGRAMS && whole; k++) {
        uint32_t header[2];
        whole = fread(header, sizeof header, 1, file) == 1 && header[1] < (64u << 20);
        if (!whole) break;
        saved[k].format = header[0];
        saved[k].length = (GLsizei)header[1];
        saved[k].bytes = malloc(header[1] + 1);
        whole = saved[k].bytes != NULL && fread(saved[k].bytes, 1, header[1], file) == header[1];
    }
    fclose(file);
    return whole;
}

static bool write_saved(const struct saved saved[PROGRAMS]) {
    char path[512];
    path_of(path, sizeof path);
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool whole = true;
    for (int k = 0; k < PROGRAMS; k++) {
        const uint32_t header[2] = {saved[k].format, (uint32_t)saved[k].length};
        whole = whole && fwrite(header, sizeof header, 1, file) == 1 &&
                fwrite(saved[k].bytes, 1, (size_t)saved[k].length, file) == (size_t)saved[k].length;
    }
    return fclose(file) == 0 && whole;
}

/* ---- The two starts */

static bool format_listed(GLenum format) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
    if (count <= 0) return false;
    GLint *formats = calloc((size_t)count, sizeof *formats);
    glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, formats);
    bool found = false;
    for (int i = 0; i < count; i++) found = found || (GLenum)formats[i] == format;
    free(formats);
    return found;
}

static void first_start(const struct scene *scene) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
    DEV("program binary formats: %d", count);
    LOG("first start: no saved programs");
    struct saved saved[PROGRAMS];
    memset(saved, 0, sizeof saved);
    bool all_given = true;
    for (int k = 0; k < PROGRAMS; k++) {
        GLuint program = glCreateProgram();
        const bool made = link_from_source(program, k);
        const bool right = made && draw_checks(scene, program, k);
        LOG("%s: linked from its source %s, drawn right %s", names[k], yes(made), yes(right));
        GLint length = 0;
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        if (length > 0) {
            saved[k].bytes = malloc((size_t)length);
            GLsizei written = 0;
            glGetProgramBinary(program, length, &written, &saved[k].format, saved[k].bytes);
            saved[k].length = written;
        }
        const GLenum error = take_error();
        DEV("%s: binary length %d, format 0x%x, %s", names[k], saved[k].length, saved[k].format,
            gl_error_name(error));
        // A device without binary formats gives none; one with them gives a binary of a listed format.
        all_given = all_given && error == GL_NO_ERROR &&
                    (count == 0 ? saved[k].length == 0 : saved[k].length > 0 && format_listed(saved[k].format));
        glDeleteProgram(program);
    }
    LOG("each program gave its binary in a listed format, where the device lists formats: %s", yes(all_given));
    LOG("saved: %s", yes(write_saved(saved)));
    for (int k = 0; k < PROGRAMS; k++) free(saved[k].bytes);
}

/* A binary given to a new program: whether it linked and the error, then (refused) a link from the source in the same
   program object. Returns whether the program then drew right. */
static bool load(const struct scene *scene, const char *what, int k, GLenum format, const void *bytes, GLsizei length,
                 bool *took) {
    GLuint program = glCreateProgram();
    glProgramBinary(program, format, bytes, length);
    const GLenum error = take_error();
    *took = linked(program);
    DEV("%s: linked %s, %s", what, yes(*took), gl_error_name(error));
    const bool usable = *took || link_from_source(program, k);
    const bool right = usable && draw_checks(scene, program, k);
    glDeleteProgram(program);
    return right;
}

static void second_start(const struct scene *scene, struct saved saved[PROGRAMS]) {
    LOG("second start: saved programs found");
    for (int k = 0; k < PROGRAMS; k++) {
        bool took = false;
        char what[96];
        snprintf(what, sizeof what, "%s's saved binary", names[k]);
        const bool right = load(scene, what, k, saved[k].format, saved[k].bytes, saved[k].length, &took);
        LOG("%s: drawn right after its saved binary %s", names[k], yes(right));
    }
    // Deleted before the binaries a driver must refuse, so that a start that stops at one leaves no saved programs.
    char path[512];
    path_of(path, sizeof path);
    LOG("saved programs deleted: %s", yes(unlink(path) == 0));
    // Binaries a driver must refuse, built from the engine-like program's.
    const struct saved base = saved[ENGINE];
    const GLsizei length = base.length;
    unsigned char *bytes = malloc((size_t)length + 64);
    bool took = false, right = false;

    memcpy(bytes, base.bytes, (size_t)length);
    right = load(scene, "a format not listed", ENGINE, 0x7fff1234u, bytes, length, &took);
    LOG("a binary of a format not listed: linked %s, drawn right after a link from its source %s", yes(took),
        yes(right));
    right = load(scene, "empty", ENGINE, base.format, bytes, 0, &took);
    LOG("an empty binary: linked %s, drawn right after a link from its source %s", yes(took), yes(right));
    memset(bytes + length, 0x5a, 64);
    right = load(scene, "64 bytes after its end", ENGINE, base.format, bytes, length + 64, &took);
    // Whether a driver takes trailing bytes is its own choice; drawn right either way.
    LOG("a binary with 64 bytes after its end: drawn right %s", yes(right));
    // The damaged ones last: the emulator's GL translator stops at them (the deviation in the expected log).
    for (GLsizei i = length / 3; i < 2 * length / 3; i++) bytes[i] ^= 0xa5;
    right = load(scene, "middle overwritten", ENGINE, base.format, bytes, length, &took);
    LOG("a binary with its middle overwritten: linked %s, drawn right after a link from its source %s", yes(took),
        yes(right));
    memcpy(bytes, base.bytes, (size_t)length);
    right = load(scene, "cut short", ENGINE, base.format, bytes, length / 2, &took);
    LOG("a binary cut to half its length: linked %s, drawn right after a link from its source %s", yes(took),
        yes(right));
    free(bytes);
}

static void *probes(void *argument) {
    (void)argument;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    const EGLint spec[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLConfig config = NULL;
    EGLint count = 0;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    if (eglInitialize(display, NULL, NULL) && eglChooseConfig(display, spec, &config, 1, &count) && count > 0) {
        surface = eglCreatePbufferSurface(display, config, size);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    }
    const bool current = context != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, context);
    LOG("an ES 3 context: %s", yes(current));
    if (current) {
        DEV("renderer: %s, %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
        const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
        DEV("GL_OES_get_program_binary listed: %s",
            yes(extensions != NULL && strstr(extensions, "GL_OES_get_program_binary") != NULL));
        struct scene scene;
        memset(&scene, 0, sizeof scene);
        scene_make(&scene);
        struct saved saved[PROGRAMS];
        memset(saved, 0, sizeof saved);
        if (load_saved(saved)) {
            second_start(&scene, saved);
        } else {
            first_start(&scene);
        }
        for (int k = 0; k < PROGRAMS; k++) free(saved[k].bytes);
        scene_free(&scene);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    eglReleaseThread();
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
