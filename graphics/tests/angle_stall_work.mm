// graphics/host's work sources for the runtime's stall watchdog (stall_work.c), on ANGLE's memory counters (patch
// 0015) and a pbuffer context. Before ANGLE's counters are known, and for a window that had no EGL window surface, a
// source answers the runtime's asset part alone. Once a window had one: its work toward a first frame grows with a
// 4 MiB texture and stays at its most after the texture's delete and after a remake of the same size, so a loop that
// uploads the same scene again stays flat; its work after a return grows with the texture, does not fall with the
// delete, and grows by the 4 MiB again at the remake, so a reload after a return counts as work.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "APKRuntime/apkrt.h"
#include "stall_work.h"

namespace {

void *library = nullptr;
bool failed = false;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-stall-work: %s\n", what.c_str());
    std::exit(1);
}

void check(bool condition, const std::string &what) {
    if (!condition) {
        std::fprintf(stderr, "angle-stall-work: FAIL %s\n", what.c_str());
        failed = true;
    }
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

constexpr uint64_t window = 7;     // a window that gets an EGL window surface
constexpr uint64_t other = 8;      // one that does not
constexpr GLsizei side = 1024;     // an RGBA8 texture of 1024 x 1024: 4 MiB
constexpr uint64_t texture_bytes = 4ull * side * side;

size_t (*counters_of)(uint64_t *, size_t) = nullptr;

// What ANGLE holds in textures and buffers: the bytes of patch 0015's classes (values 1, 3, ..., 13).
uint64_t held() {
    std::array<uint64_t, 14> values{};
    counters_of(values.data(), values.size());
    uint64_t bytes = 0;
    for (size_t k = 1; k < values.size(); k += 2) {
        bytes += values[k];
    }
    return bytes;
}

GLuint make_texture(const std::vector<unsigned char> &data) {
    GLuint texture = 0;
    GL(GenTextures)(1, &texture);
    GL(BindTexture)(GL_TEXTURE_2D, texture);
    GL(TexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, side, side);
    GL(TexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, side, side, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    GL(Finish)();
    return texture;
}

// Deletes the texture and waits (at most 2 s) until Metal freed it: ANGLE holds no more than `before` again.
bool delete_texture(GLuint texture, uint64_t before) {
    GL(DeleteTextures)(1, &texture);
    for (int round = 0; round < 40; ++round) {
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        GL(Finish)();
        if (held() <= before) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-stall-work <libGLESv2.dylib>");
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
        counters_of = load<size_t (*)(uint64_t *, size_t)>("ANGLEMetalMemoryCounters");

        // Before ANGLE's counters are known: the asset parts alone, also for a window that has its surface.
        minato_graphics_stall_gl_window(window);
        check(minato_graphics_stall_work(window) == apkrt_stall_asset_work(),
              "without ANGLE's counters, the work is the asset work alone");
        check(minato_graphics_stall_return_work(window) == apkrt_stall_asset_read_cumulative(),
              "without ANGLE's counters, the return work is the asset reads alone");
        minato_graphics_stall_counters(counters_of);

        const EGLAttrib display_attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                                                EGL_NONE};
        EGLDisplay display = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, display_attributes);
        if (display == EGL_NO_DISPLAY || EGL(Initialize)(display, nullptr, nullptr) != EGL_TRUE) {
            fail("no Metal display");
        }
        const EGLint spec[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        if (EGL(ChooseConfig)(display, spec, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            fail("no ES 3 pbuffer config");
        }
        const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLSurface surface = EGL(CreatePbufferSurface)(display, config, size);
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, version);
        if (EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
            fail("no current context");
        }
        GL(Clear)(GL_COLOR_BUFFER_BIT);  // the pbuffer's own texture before the start
        GL(Finish)();

        const uint64_t assets = apkrt_stall_asset_work(), reads = apkrt_stall_asset_read_cumulative();
        check(minato_graphics_stall_work(other) == assets && minato_graphics_stall_return_work(other) == reads,
              "a window without an EGL window surface gets the asset parts alone");
        const uint64_t base = held();
        const uint64_t work0 = minato_graphics_stall_work(window), back0 = minato_graphics_stall_return_work(window);

        std::vector<unsigned char> data(texture_bytes, 0x5a);
        GLuint texture = make_texture(data);
        const uint64_t work1 = minato_graphics_stall_work(window), back1 = minato_graphics_stall_return_work(window);
        check(work1 >= work0 + texture_bytes, "the work grows with the texture's 4 MiB");
        check(back1 >= back0 + texture_bytes, "the return work grows with the texture's 4 MiB");

        check(delete_texture(texture, base), "Metal freed the texture within 2 s");
        const uint64_t work2 = minato_graphics_stall_work(window), back2 = minato_graphics_stall_return_work(window);
        check(work2 >= work1 && work2 < work1 + texture_bytes / 4, "the work stays at its most after the delete");
        check(back2 >= back1 && back2 < back1 + texture_bytes / 4, "the return work does not fall with the delete");

        texture = make_texture(data);
        const uint64_t work3 = minato_graphics_stall_work(window), back3 = minato_graphics_stall_return_work(window);
        check(work3 >= work2 && work3 < work1 + texture_bytes / 4,
              "a remake of the same size leaves the work flat (a loop that uploads the same scene again)");
        check(back3 >= back2 + texture_bytes, "the remake grows the return work by its 4 MiB again (a reload)");
        std::printf("work %llu %llu %llu %llu, return work %llu %llu %llu %llu (bytes over the asset parts)\n",
                    static_cast<unsigned long long>(work0 - assets), static_cast<unsigned long long>(work1 - assets),
                    static_cast<unsigned long long>(work2 - assets), static_cast<unsigned long long>(work3 - assets),
                    static_cast<unsigned long long>(back0 - reads), static_cast<unsigned long long>(back1 - reads),
                    static_cast<unsigned long long>(back2 - reads), static_cast<unsigned long long>(back3 - reads));

        delete_texture(texture, base);
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(DestroySurface)(display, surface);
        EGL(Terminate)(display);
    }
    if (failed) {
        return 1;
    }
    std::printf("angle-stall-work: ok\n");
    return 0;
}
