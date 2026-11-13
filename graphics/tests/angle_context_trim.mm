// ANGLE's macOS slice: a context that is current on no thread keeps no free buffers. Ren'Py's race (graphics/egl's
// backup share) leaves a context D behind at each return that has loaded a whole scene and is never made current
// again; its buffer manager's free list, up to patch 0029's 64 MiB, stayed with it (about 48 MiB per race in the
// Simulator, where Metal's buffers are shared memory). Here a context C stays small; D, made with C as its share
// context, draws from 16 buffers of 8 MiB held at once, waits for the GPU and deletes them all: with no allocation
// after them no collection runs, so D's free list holds at least 32 MiB whatever the GPU's timing. Then C is made
// current again (SDL's restore). Once another context is current, D's free list must be gone: the gl line's pool= at
// most 1 MiB. D made current again works: its next buffers are new, and a clear in it reads back. A context released
// at each frame's end (no context current) and made current again keeps its free list: it fills again, D goes to no
// context and back, the free lists still hold at least 32 MiB; then no context and C: D's are gone.
#import <Metal/Metal.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-context-trim: %s\n", what.c_str());
    std::exit(1);
}

template <typename Function>
Function load(const char *name) {
    void *address = dlsym(library, name);
    if (address == nullptr) {
        fail(std::string("libGLESv2 does not export ") + name);
    }
    return reinterpret_cast<Function>(address);
}

#define EGL(name) load<decltype(&egl##name)>("EGL_" #name)
#define GL(name) load<decltype(&gl##name)>("gl" #name)

// The gl line's pool=<buffers>/<MiB> (patch 0015): the buffer managers' free lists, all contexts together.
struct Pool {
    unsigned long long buffers = 0, mib = 0;
};

Pool pool() {
    char line[512] = {0};
    load<int (*)(char *, size_t)>("ANGLEMetalMemoryLine")(line, sizeof line);
    Pool read;
    const char *at = std::strstr(line, " pool=");
    if (at == nullptr || std::sscanf(at, " pool=%llu/%llu", &read.buffers, &read.mib) != 2) {
        fail(std::string("no pool= in the gl line: ") + line);
    }
    return read;
}

GLuint shader(GLenum kind, const char *source) {
    const GLuint made = GL(CreateShader)(kind);
    GL(ShaderSource)(made, 1, &source, nullptr);
    GL(CompileShader)(made);
    return made;
}

// A program drawing from attribute 0, current in the calling context.
GLuint use_program() {
    const GLuint program = GL(CreateProgram)();
    GL(AttachShader)(program, shader(GL_VERTEX_SHADER, "attribute vec4 a; void main() { gl_Position = a; }"));
    GL(AttachShader)(program, shader(GL_FRAGMENT_SHADER,
                                     "precision mediump float; void main() { gl_FragColor = vec4(1.0); }"));
    GL(BindAttribLocation)(program, 0, "a");
    GL(LinkProgram)(program);
    GL(UseProgram)(program);
    GL(EnableVertexAttribArray)(0);
    return program;
}

// A buffer of `bytes`, drawn from once and left alive.
GLuint draw_from_new_buffer(size_t bytes, const std::vector<float> &data) {
    GLuint buffer = 0;
    GL(GenBuffers)(1, &buffer);
    GL(BindBuffer)(GL_ARRAY_BUFFER, buffer);
    GL(BufferData)(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), data.data(), GL_STATIC_DRAW);
    GL(VertexAttribPointer)(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    GL(DrawArrays)(GL_TRIANGLES, 0, 3);
    GL(Flush)();
    return buffer;
}

// A buffer of `bytes` drawn from once and deleted: it goes to the context's free list once its command buffer is done.
void draw_from_buffer(size_t bytes, const std::vector<float> &data) {
    GLuint buffer = draw_from_new_buffer(bytes, data);
    GL(DeleteBuffers)(1, &buffer);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-context-trim <libGLESv2.dylib>");
    }
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not built, see tools/build-angle.sh\n", argv[1]);
        return 0;
    }
    @autoreleasepool {
        library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            fail(std::string("dlopen: ") + dlerror());
        }
        const EGLAttrib display_attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                                                EGL_NONE};
        EGLDisplay display = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, display_attributes);
        if (display == EGL_NO_DISPLAY || EGL(Initialize)(display, nullptr, nullptr) != EGL_TRUE) {
            fail("no Metal display");
        }
        const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
                                            EGL_PBUFFER_BIT, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        if (EGL(ChooseConfig)(display, config_attributes, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            fail("no ES 3.0 pbuffer config");
        }
        const EGLint size[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
        EGLSurface in_c = EGL(CreatePbufferSurface)(display, config, size);
        EGLSurface in_d = EGL(CreatePbufferSurface)(display, config, size);
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        EGLContext c = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
        EGLContext d = EGL(CreateContext)(display, config, c, context_attributes);
        if (c == EGL_NO_CONTEXT || d == EGL_NO_CONTEXT) {
            fail("no ES 3.0 contexts");
        }

        enum : size_t { kLarge = 8u << 20, kLargeBuffers = 16, kSmall = 4096, kFrames = 8 };
        constexpr unsigned long long kFilledMiB = 32, kLeftMiB = 1;
        const std::vector<float> data(kLarge / sizeof(float), 0.0f);

        // C: small buffers only, as an app's frames.
        if (EGL(MakeCurrent)(display, in_c, in_c, c) != EGL_TRUE) {
            fail("C not made current");
        }
        const GLuint program_c = use_program();
        for (size_t frame = 0; frame < kFrames; frame++) {
            draw_from_buffer(kSmall, data);
        }
        GL(Finish)();

        // D: the scene's large buffers, all alive until the GPU is done with them, then deleted together (straight to
        // D's free list), then small frames.
        if (EGL(MakeCurrent)(display, in_d, in_d, d) != EGL_TRUE) {
            fail("D not made current");
        }
        use_program();
        const auto fill_large = [&data]() {
            GLuint large[kLargeBuffers] = {};
            for (GLuint &buffer : large) {
                buffer = draw_from_new_buffer(kLarge, data);
            }
            GL(Finish)();
            GL(DeleteBuffers)(static_cast<GLsizei>(kLargeBuffers), large);
        };
        fill_large();
        for (size_t frame = 0; frame < kFrames; frame++) {
            draw_from_buffer(kSmall, data);
        }
        GL(Finish)();
        const Pool filled = pool();
        std::printf("D current: the free lists hold %llu MiB (%s)\n", filled.mib,
                    filled.mib >= kFilledMiB ? "at least 32" : "LESS THAN 32");
        if (filled.mib < kFilledMiB) {
            fail("D's free list did not fill: the test proves nothing");
        }

        // SDL's restore: C current again, D current nowhere.
        if (EGL(MakeCurrent)(display, in_c, in_c, c) != EGL_TRUE) {
            fail("C not made current again");
        }
        const Pool left = pool();
        std::printf("C current again, D current nowhere: the free lists hold %llu MiB (%s)\n", left.mib,
                    left.mib <= kLeftMiB ? "at most 1" : "MORE THAN 1, D's kept");
        bool good = left.mib <= kLeftMiB;

        // D again: it draws and clears as before.
        if (EGL(MakeCurrent)(display, in_d, in_d, d) != EGL_TRUE) {
            fail("D not made current again");
        }
        for (size_t frame = 0; frame < kFrames; frame++) {
            draw_from_buffer(kSmall, data);
        }
        GL(ClearColor)(0.0f, 1.0f, 0.0f, 1.0f);
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        GLubyte pixel[4] = {0, 0, 0, 0};
        GL(ReadPixels)(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        const bool cleared = pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255;
        std::printf("D made current again: draws, and its clear reads back %s\n", cleared ? "green" : "WRONG");
        good = good && cleared;

        // A frame's end that lets the context go: D fills again, no context, D again.
        fill_large();
        if (EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE ||
            EGL(MakeCurrent)(display, in_d, in_d, d) != EGL_TRUE) {
            fail("D not released and made current again");
        }
        const Pool kept = pool();
        std::printf("D released and made current again: the free lists hold %llu MiB (%s)\n", kept.mib,
                    kept.mib >= kFilledMiB ? "at least 32, kept" : "LESS THAN 32, D's dropped");
        good = good && kept.mib >= kFilledMiB;

        // No context, then C: D's free list goes.
        if (EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE ||
            EGL(MakeCurrent)(display, in_c, in_c, c) != EGL_TRUE) {
            fail("C not made current after no context");
        }
        const Pool after_none = pool();
        std::printf("no context, then C: the free lists hold %llu MiB (%s)\n", after_none.mib,
                    after_none.mib <= kLeftMiB ? "at most 1" : "MORE THAN 1, D's kept");
        good = good && after_none.mib <= kLeftMiB;

        EGL(MakeCurrent)(display, in_c, in_c, c);
        GL(DeleteProgram)(program_c);
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, d);
        EGL(DestroyContext)(display, c);
        EGL(DestroySurface)(display, in_d);
        EGL(DestroySurface)(display, in_c);
        EGL(Terminate)(display);
        if (!good) {
            fail("free buffers kept or dropped at the wrong switch");
        }
        std::printf("a context keeps its free buffers until another context is current\n");
    }
    return 0;
}
