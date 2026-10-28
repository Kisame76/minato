/* The scene with GLES 3.0 (ADR-0037, decision 1), the same file in both builds: in the APKs on the NDK's libEGL and
   libGLESv3, Minato's front libs; in the host build on ANGLE's libGLESv2, EGL through its EGL_* entries
   (native/angle_egl.h). A frame: the offscreen target cleared, the background, the meshes (per draw a
   glUniformMatrix4fv, two glUniform4fv and a glDrawElements), the particles, then the post pass into the window. */
#include "bench_gles.h"

#include <GLES3/gl3.h>
#include <stddef.h>
#include <stdlib.h>

#include "scene.h"

struct bench_gles {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    EGLBoolean current;
    EGLint width, height;
    int target_width, target_height;  // the offscreen target's, the window's at scale 100
    float aspect;
    int meshes, check, fill_layers;
    int cold;  // 1 once the check frame was drawn before the first frame
    GLuint background, fill, mesh, particles, post;
    GLint fill_time, fill_layer;
    GLint background_time, mvp, normal, color, particles_time, particles_aspect;
    GLuint sphere, buffers[2], texture, target, depth, framebuffer;
};

/* ---- Shaders: a full-screen triangle, the background, the meshes, the particles and the post pass */

static const char fullscreen_vertex[] =
    "#version 300 es\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    v_uv = corner;\n"
    "    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char background_fragment[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform float u_time;\n"
    "in vec2 v_uv;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    vec3 top = vec3(0.10, 0.16, 0.32) + 0.08 * vec3(sin(u_time * 0.30), sin(u_time * 0.23 + 1.0),\n"
    "                                                    sin(u_time * 0.17 + 2.0));\n"
    "    float band = 0.04 * sin(v_uv.y * 24.0 + u_time * 1.7);\n"
    "    color = vec4(mix(vec3(0.02, 0.03, 0.06), top, v_uv.y) + band, 1.0);\n"
    "}\n";

/* The fill-heavy load's layer (M6's MetalFX plan, step 0), the formula of Vulkan's fill.frag. */
static const char fill_fragment[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform float u_time;\n"
    "uniform float u_layer;\n"
    "uniform sampler2D u_texture;\n"
    "in vec2 v_uv;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    vec2 uv = v_uv;\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int i = 0; i < 16; ++i) {\n"
    "        vec4 texel = textureLod(u_texture, fract(uv * 3.0 + u_layer * 0.13), 0.0);\n"
    "        uv = fract(uv + texel.rg * 0.07 + 0.013 * float(i) + u_time * 0.01);\n"
    "        sum += texel.rgb;\n"
    "    }\n"
    "    color = vec4(sum * (1.0 / 16.0), 0.3);\n"
    "}\n";

/* The meshes: their texture at one level per mesh, the formula of Vulkan's mesh.frag, which says why. */
static const char mesh_vertex[] =
    "#version 300 es\n"
    "layout(location = 0) in vec3 a_position;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "uniform vec4 u_normal[3];\n"
    "out vec3 v_normal;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_normal = mat3(u_normal[0].xyz, u_normal[1].xyz, u_normal[2].xyz) * a_normal;\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "}\n";

static const char mesh_fragment[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_texture;\n"
    "uniform mat4 u_mvp;\n"
    "uniform vec4 u_color;\n"
    "uniform float u_height;\n"
    "in vec3 v_normal;\n"
    "in vec2 v_uv;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    vec3 normal = normalize(v_normal);\n"
    "    vec3 light = normalize(vec3(0.4, 0.7, 0.6));\n"
    "    float diffuse = max(dot(normal, light), 0.0);\n"
    "    float specular = pow(max(dot(normal, normalize(light + vec3(0.0, 0.0, 1.0))), 0.0), 32.0);\n"
    "    float scale = length(vec3(u_mvp[0][3], u_mvp[1][3], u_mvp[2][3]));\n"
    "    float radius = 1.7320508 * scale / u_mvp[3][3] * 0.5 * u_height;\n"
    "    float level = clamp(log2(512.0 / 3.14159265 / radius), 0.0, 8.0);\n"
    "    vec3 albedo = textureLod(u_texture, v_uv * vec2(4.0, 2.0), level).rgb * u_color.rgb;\n"
    "    color = vec4(albedo * (0.2 + 0.8 * diffuse) + vec3(0.35 * specular), 1.0);\n"
    "}\n";

static const char particle_vertex[] =
    "#version 300 es\n"
    "uniform float u_time;\n"
    "uniform float u_aspect;\n"
    "out vec2 v_corner;\n"
    "out vec4 v_color;\n"
    "uint hash(uint x) {\n"
    "    x ^= x >> 16u;\n"
    "    x *= 0x7feb352du;\n"
    "    x ^= x >> 15u;\n"
    "    x *= 0x846ca68bu;\n"
    "    x ^= x >> 16u;\n"
    "    return x;\n"
    "}\n"
    "float unit(uint value) { return float(value >> 8u) / 16777216.0; }\n"
    "void main() {\n"
    "    const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0),\n"
    "                                    vec2(1.0, 1.0), vec2(-1.0, 1.0));\n"
    "    uint id = uint(gl_InstanceID) * 4u;\n"
    "    uint a = hash(id), b = hash(id + 1u), c = hash(id + 2u), d = hash(id + 3u);\n"
    "    vec2 velocity = (vec2(unit(c), unit(d)) - 0.5) * 0.12;\n"
    "    vec2 center = fract(vec2(unit(a), unit(b)) + velocity * u_time) * 2.2 - 1.1;\n"
    "    float size = 0.006 + 0.016 * unit(hash(a));\n"
    "    v_corner = corners[gl_VertexID];\n"
    "    v_color = vec4(0.5 + 0.5 * unit(b ^ c), 0.5 + 0.5 * unit(c ^ d), 0.6 + 0.4 * unit(d ^ a), 0.4);\n"
    "    gl_Position = vec4(center + v_corner * vec2(size / u_aspect, size), unit(hash(b)) * 1.8 - 0.9, 1.0);\n"
    "}\n";

static const char particle_fragment[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_corner;\n"
    "in vec4 v_color;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    float fade = 1.0 - dot(v_corner, v_corner);\n"
    "    if (fade < 0.0) discard;\n"
    "    color = vec4(v_color.rgb, fade * fade * v_color.a);\n"
    "}\n";

static const char post_fragment[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_scene;\n"
    "in vec2 v_uv;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    vec2 offset = vec2(0.0015, 0.0);\n"
    "    vec3 scene = vec3(texture(u_scene, v_uv + offset).r, texture(u_scene, v_uv).g,\n"
    "                      texture(u_scene, v_uv - offset).b);\n"
    "    vec2 centered = v_uv - 0.5;\n"
    "    scene *= 1.0 - 0.6 * dot(centered, centered);\n"
    "    color = vec4(scene * scene * (3.0 - 2.0 * scene), 1.0);\n"
    "}\n";

static GLuint shader(GLenum kind, const char *source) {
    const GLuint made = glCreateShader(kind);
    glShaderSource(made, 1, &source, NULL);
    glCompileShader(made);
    GLint compiled = GL_FALSE;
    glGetShaderiv(made, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[1024] = "";
        glGetShaderInfoLog(made, (GLsizei)sizeof log, NULL, log);
        bench_log("a shader does not compile: %s", log);
    }
    return made;
}

/* A program of the two shaders, 0 when it does not link. */
static GLuint program(const char *vertex, const char *fragment) {
    const GLuint made = glCreateProgram();
    const GLuint shaders[2] = {shader(GL_VERTEX_SHADER, vertex), shader(GL_FRAGMENT_SHADER, fragment)};
    glAttachShader(made, shaders[0]);
    glAttachShader(made, shaders[1]);
    glLinkProgram(made);
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    GLint linked = GL_FALSE;
    glGetProgramiv(made, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = "";
        glGetProgramInfoLog(made, (GLsizei)sizeof log, NULL, log);
        bench_log("a program does not link: %s", log);
        glDeleteProgram(made);
        return 0;
    }
    return made;
}

/* ---- The objects */

static int make_programs(struct bench_gles *scene) {
    scene->background = program(fullscreen_vertex, background_fragment);
    // The fill load's program only for that load, so the others pay nothing for it.
    if (scene->fill_layers > 0) scene->fill = program(fullscreen_vertex, fill_fragment);
    scene->mesh = program(mesh_vertex, mesh_fragment);
    scene->particles = program(particle_vertex, particle_fragment);
    scene->post = program(fullscreen_vertex, post_fragment);
    if (scene->background == 0 || (scene->fill_layers > 0 && scene->fill == 0) || scene->mesh == 0 ||
        scene->particles == 0 || scene->post == 0) {
        return -1;
    }
    scene->background_time = glGetUniformLocation(scene->background, "u_time");
    scene->mvp = glGetUniformLocation(scene->mesh, "u_mvp");
    scene->normal = glGetUniformLocation(scene->mesh, "u_normal");
    scene->color = glGetUniformLocation(scene->mesh, "u_color");
    scene->particles_time = glGetUniformLocation(scene->particles, "u_time");
    scene->particles_aspect = glGetUniformLocation(scene->particles, "u_aspect");
    glUseProgram(scene->mesh);
    glUniform1i(glGetUniformLocation(scene->mesh, "u_texture"), 0);
    glUniform1f(glGetUniformLocation(scene->mesh, "u_height"), (float)scene->target_height);
    if (scene->fill != 0) {
        scene->fill_time = glGetUniformLocation(scene->fill, "u_time");
        scene->fill_layer = glGetUniformLocation(scene->fill, "u_layer");
        glUseProgram(scene->fill);
        glUniform1i(glGetUniformLocation(scene->fill, "u_texture"), 0);
    }
    glUseProgram(scene->post);
    glUniform1i(glGetUniformLocation(scene->post, "u_scene"), 0);
    return 0;
}

static void make_sphere(struct bench_gles *scene) {
    struct bench_vertex vertices[BENCH_VERTICES];
    uint16_t indices[BENCH_INDICES];
    bench_sphere(vertices, indices);
    glGenVertexArrays(1, &scene->sphere);
    glBindVertexArray(scene->sphere);
    glGenBuffers(2, scene->buffers);
    glBindBuffer(GL_ARRAY_BUFFER, scene->buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene->buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    const GLsizei stride = (GLsizei)sizeof(struct bench_vertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(struct bench_vertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(struct bench_vertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(struct bench_vertex, uv));
    glBindVertexArray(0);
}

static int make_texture(struct bench_gles *scene) {
    uint8_t *pixels = malloc(bench_texture_bytes());
    if (pixels == NULL) return -1;
    bench_texture(pixels);
    glGenTextures(1, &scene->texture);
    glBindTexture(GL_TEXTURE_2D, scene->texture);
    glTexStorage2D(GL_TEXTURE_2D, BENCH_TEXTURE_LEVELS, GL_RGBA8, BENCH_TEXTURE_SIZE, BENCH_TEXTURE_SIZE);
    size_t offset = 0;
    for (int level = 0; level < BENCH_TEXTURE_LEVELS; ++level) {
        const GLsizei side = BENCH_TEXTURE_SIZE >> level;
        glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, side, side, GL_RGBA, GL_UNSIGNED_BYTE, pixels + offset);
        offset += 4 * (size_t)side * (size_t)side;
    }
    free(pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return 0;
}

/* The offscreen target at the window's size, or at bench.conf's scale of it: RGBA8 color, 24-bit depth. */
static int make_target(struct bench_gles *scene) {
    glGenTextures(1, &scene->target);
    glBindTexture(GL_TEXTURE_2D, scene->target);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, scene->target_width, scene->target_height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &scene->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, scene->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, scene->target_width,
                          scene->target_height);
    glGenFramebuffers(1, &scene->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, scene->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene->target, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, scene->depth);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        bench_log("the offscreen target is incomplete: 0x%x", status);
        return -1;
    }
    return 0;
}

struct bench_gles *bench_gles_start(EGLDisplay display, EGLNativeWindowType window, const EGLint *surface_attributes,
                                    int meshes, int check, int scale,
                                    int fill_layers) {
    struct bench_gles *scene = calloc(1, sizeof *scene);
    if (scene == NULL) return NULL;
    scene->display = display;
    scene->surface = EGL_NO_SURFACE;
    scene->context = EGL_NO_CONTEXT;
    scene->meshes = meshes;
    scene->check = check;
    scene->fill_layers = fill_layers;
    static const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
                                               EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                               EGL_ALPHA_SIZE, 8, EGL_NONE};
    static const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLConfig config = NULL;
    EGLint configs = 0;
    if (!eglInitialize(display, NULL, NULL) || !eglChooseConfig(display, config_attributes, &config, 1, &configs) ||
        configs < 1 ||
        (scene->surface = eglCreateWindowSurface(display, config, window, surface_attributes)) == EGL_NO_SURFACE ||
        (scene->context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes)) == EGL_NO_CONTEXT ||
        !(scene->current = eglMakeCurrent(display, scene->surface, scene->surface, scene->context)) ||
        !eglQuerySurface(display, scene->surface, EGL_WIDTH, &scene->width) ||
        !eglQuerySurface(display, scene->surface, EGL_HEIGHT, &scene->height)) {
        bench_log("EGL failed: 0x%x", eglGetError());
        bench_gles_stop(scene);
        return NULL;
    }
    scene->aspect = (float)scene->width / (float)scene->height;
    bench_target_size(scene->width, scene->height, scale, &scene->target_width, &scene->target_height);
    make_sphere(scene);
    if (make_programs(scene) != 0 || make_texture(scene) != 0 || make_target(scene) != 0) {
        bench_gles_stop(scene);
        return NULL;
    }
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        bench_log("GL failed: 0x%x", error);
        bench_gles_stop(scene);
        return NULL;
    }
    bench_log("GLES: %d x %d", scene->width, scene->height);
    bench_log("target: %d x %d (%d %%)", scene->target_width, scene->target_height, scale);
    const GLuint programs[5] = {scene->background, scene->fill, scene->mesh, scene->particles, scene->post};
    int built = 0;
    for (int index = 0; index < 5; ++index) built += programs[index] != 0;
    bench_log("pipelines: %d", built);
    return scene;
}

/* ---- A frame */

/* The CRC of the window's pixels; `cold` for the check frame drawn before the first frame. */
static void log_crc(const struct bench_gles *scene, int frame, int cold) {
    const size_t size = 4 * (size_t)scene->width * (size_t)scene->height;
    uint8_t *pixels = malloc(size);
    if (pixels == NULL) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, scene->width, scene->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    bench_log("crc frame %d%s: %08x", frame, cold ? " cold" : "", bench_crc32(0, pixels, size));
    free(pixels);
}

static EGLint draw(struct bench_gles *scene, int frame, int cold) {
    const float time = bench_time(frame);
    glBindFramebuffer(GL_FRAMEBUFFER, scene->framebuffer);
    glViewport(0, 0, scene->target_width, scene->target_height);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);

    glDisable(GL_DEPTH_TEST);
    glUseProgram(scene->background);
    glUniform1f(scene->background_time, time);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (scene->fill_layers > 0) {
        // Full-screen alpha layers over the background, M6's MetalFX plan, step 0.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(scene->fill);
        glBindTexture(GL_TEXTURE_2D, scene->texture);
        glUniform1f(scene->fill_time, time);
        for (int layer = 0; layer < scene->fill_layers; ++layer) {
            glUniform1f(scene->fill_layer, (float)layer);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_BLEND);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glUseProgram(scene->mesh);
    glBindVertexArray(scene->sphere);
    glBindTexture(GL_TEXTURE_2D, scene->texture);
    for (int index = 0; index < scene->meshes; ++index) {
        struct bench_mesh mesh;
        bench_mesh(index, frame, scene->aspect, &mesh);
        glUniformMatrix4fv(scene->mvp, 1, GL_FALSE, mesh.mvp);
        glUniform4fv(scene->normal, 3, mesh.normal);
        glUniform4fv(scene->color, 1, mesh.color);
        glDrawElements(GL_TRIANGLES, BENCH_INDICES, GL_UNSIGNED_SHORT, NULL);
    }
    glBindVertexArray(0);

    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(scene->particles);
    glUniform1f(scene->particles_time, time);
    glUniform1f(scene->particles_aspect, scene->aspect);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, scene->meshes);
    glDisable(GL_BLEND);
    const GLenum depth = GL_DEPTH_ATTACHMENT;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, scene->width, scene->height);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(scene->post);
    glBindTexture(GL_TEXTURE_2D, scene->target);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (frame == scene->check) log_crc(scene, frame, cold);
    return eglSwapBuffers(scene->display, scene->surface) ? EGL_SUCCESS : eglGetError();
}

EGLint bench_gles_frame(struct bench_gles *scene, int frame) {
    // The check frame once before the first frame too, while the GPU is cold: its CRC must not depend on what the
    // GPU drew before (graphics.bench).
    if (scene->check >= 0 && !scene->cold) {
        scene->cold = 1;
        const EGLint result = draw(scene, scene->check, 1);
        if (result != EGL_SUCCESS) return result;
    }
    return draw(scene, frame, 0);
}

void bench_gles_drain(struct bench_gles *scene) {
    if (scene != NULL && scene->current) glFinish();
}

void bench_gles_stop(struct bench_gles *scene) {
    if (scene == NULL) return;
    if (scene->current) {
        const GLuint programs[5] = {scene->background, scene->fill, scene->mesh, scene->particles, scene->post};
        for (int index = 0; index < 5; ++index) glDeleteProgram(programs[index]);
        const GLuint textures[2] = {scene->texture, scene->target};
        glDeleteTextures(2, textures);
        glDeleteBuffers(2, scene->buffers);
        glDeleteVertexArrays(1, &scene->sphere);
        glDeleteRenderbuffers(1, &scene->depth);
        glDeleteFramebuffers(1, &scene->framebuffer);
        eglMakeCurrent(scene->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (scene->surface != EGL_NO_SURFACE) eglDestroySurface(scene->display, scene->surface);
    if (scene->context != EGL_NO_CONTEXT) eglDestroyContext(scene->display, scene->context);
    eglTerminate(scene->display);
    eglReleaseThread();
    free(scene);
}
