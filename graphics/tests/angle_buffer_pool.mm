// Patch 0029 on ANGLE's macOS slice: the buffer manager's free list stays small after a stay in the background. There
// (patch 0025) no command buffer completes, so each buffer an app makes then is a new one: 20 buffers of 8 MiB, each
// drawn from once and deleted, as an app's display rebuilt in the background makes them (Ren'Py's reset). Back in the
// foreground they complete, and the app's next buffer moves them to the free list. Upstream's collection frees that
// list only after 120 context switches, 5000 commits or 64 MiB more allocated, so the whole background's buffers
// stayed in it (about 1 GiB after two returns of a Ren'Py game in the Simulator, where Metal's buffers are shared
// memory). With the patch the free list keeps at most 64 MiB: after each of three returns and a second of frames that
// each make a small buffer, its bytes (the gl line's pool=) are at most that, and buffers of one size are still reused.
#import <Metal/Metal.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-buffer-pool: %s\n", what.c_str());
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

// The embedder: commits go in the foreground, wait in ANGLE's FIFO in the background (as angle_commit_defer.mm).
std::mutex state_mutex;
std::condition_variable state_changed;
bool background = false;

int enter() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return background ? 0 : 1;
}

void counted() {}

void gate_unused() {
    fail("patch 0003's gate was called although the deferral is set");
}

void wait_foreground() {
    std::unique_lock<std::mutex> lock(state_mutex);
    state_changed.wait(lock, [] { return !background; });
}

void set_background(bool value) {
    std::lock_guard<std::mutex> lock(state_mutex);
    background = value;
    state_changed.notify_all();
}

// The gl line's pool=<buffers>/<MiB> (patch 0015): the buffer manager's free list.
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

// A buffer of `bytes` drawn from once and deleted: the GPU holds it until its command buffer completes.
void draw_from_buffer(size_t bytes, const std::vector<float> &data) {
    GLuint buffer = 0;
    GL(GenBuffers)(1, &buffer);
    GL(BindBuffer)(GL_ARRAY_BUFFER, buffer);
    GL(BufferData)(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), data.data(), GL_STATIC_DRAW);
    GL(VertexAttribPointer)(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    GL(DrawArrays)(GL_TRIANGLES, 0, 3);
    GL(Flush)();
    GL(DeleteBuffers)(1, &buffer);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-buffer-pool <libGLESv2.dylib>");
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
        load<void (*)(void (*)(void), void (*)(void))>("ANGLEMetalSetCommitGates")(gate_unused, counted);
        load<void (*)(int (*)(void), void (*)(void))>("ANGLEMetalSetCommitDeferral")(enter, wait_foreground);

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
        EGLSurface pbuffer = EGL(CreatePbufferSurface)(display, config, size);
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
        if (EGL(MakeCurrent)(display, pbuffer, pbuffer, context) != EGL_TRUE) {
            fail("no current ES 3.0 context");
        }
        const GLuint program = GL(CreateProgram)();
        GL(AttachShader)(program, shader(GL_VERTEX_SHADER, "attribute vec4 a; void main() { gl_Position = a; }"));
        GL(AttachShader)(program, shader(GL_FRAGMENT_SHADER,
                                         "precision mediump float; void main() { gl_FragColor = vec4(1.0); }"));
        GL(BindAttribLocation)(program, 0, "a");
        GL(LinkProgram)(program);
        GL(UseProgram)(program);
        GL(EnableVertexAttribArray)(0);

        enum : size_t { kLarge = 8u << 20, kLargeBuffers = 20, kSmall = 4096, kFrames = 60, kTrips = 3 };
        constexpr unsigned long long kFreeListMiB = 64;
        const std::vector<float> data(kLarge / sizeof(float), 0.0f);
        GL(Finish)();
        bool kept_small = true;
        for (size_t trip = 1; trip <= kTrips; trip++) {
            set_background(true);
            for (size_t index = 0; index < kLargeBuffers; index++) {
                draw_from_buffer(kLarge, data);
            }
            set_background(false);
            GL(Finish)();
            for (size_t frame = 0; frame < kFrames; frame++) {
                draw_from_buffer(kSmall, data);
            }
            GL(Finish)();
            const Pool held = pool();
            std::printf("trip %zu: after the return and %zu frames the free list holds %llu buffers, %llu MiB\n", trip,
                        static_cast<size_t>(kFrames), held.buffers, held.mib);
            if (held.mib > kFreeListMiB) {
                kept_small = false;
            }
        }
        if (!kept_small) {
            fail("the free list kept more than 64 MiB of the background's buffers after a return");
        }
        // Buffers of one size are still reused: two more rounds of small frames leave the free list as it was.
        const Pool before = pool();
        for (size_t frame = 0; frame < 2 * kFrames; frame++) {
            draw_from_buffer(kSmall, data);
        }
        GL(Finish)();
        const Pool after = pool();
        std::printf("small frames reuse their buffers: free list %llu, then %llu buffers\n", before.buffers,
                    after.buffers);
        if (after.buffers == 0 || after.buffers > before.buffers + kFrames) {
            fail("the small buffers are not reused from the free list");
        }
        std::printf("the free list stays at most %llu MiB after each return\n", kFreeListMiB);

        GL(DeleteProgram)(program);
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(DestroySurface)(display, pbuffer);
        EGL(Terminate)(display);
    }
    return 0;
}
