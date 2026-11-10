// The memory counters of patch 0015: ANGLEMetalMemoryCounters and ANGLEMetalMemoryLine count what ANGLE holds in
// Metal by class until Metal frees it. On a pbuffer context: an RGBA8 and an ETC2 texture, a DEPTH24_STENCIL8 and a
// 4x RGBA8 renderbuffer, cleared, and a 1 MiB buffer each add one to their class with at least their own bytes (the
// renderbuffers in private storage), the device's allocation is known, and once they are deleted and Metal is done
// with them the texture classes come back to where they were.
#import <Metal/Metal.h>

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

namespace {

void *library = nullptr;
bool failed = false;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-memory-counters: %s\n", what.c_str());
    std::exit(1);
}

void check(bool condition, const std::string &what) {
    if (!condition) {
        std::fprintf(stderr, "angle-memory-counters: FAIL %s\n", what.c_str());
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

// The counters' order (patch 0015): count and bytes of compressed, color, depth/stencil, multisample and memoryless
// textures and of shared and private buffers, then the textures' private and shared bytes, the free buffers and their
// bytes, and the device's allocation.
enum { COMPRESSED, COLOR, DEPTH, MULTISAMPLE, MEMORYLESS, BUFFER_SHARED, BUFFER_PRIVATE };
using Counters = std::array<uint64_t, 19>;
size_t (*counters_of)(uint64_t *, size_t) = nullptr;

Counters read() {
    Counters values{};
    if (counters_of(values.data(), values.size()) < values.size()) {
        fail("fewer than 19 counters");
    }
    return values;
}

uint64_t count(const Counters &values, int kind) { return values[static_cast<size_t>(2 * kind)]; }
uint64_t bytes(const Counters &values, int kind) { return values[static_cast<size_t>(2 * kind + 1)]; }

// Whether the texture classes hold no more than they held before: Metal freed what the test made.
bool back(const Counters &before, const Counters &after) {
    return count(after, COLOR) <= count(before, COLOR) && count(after, COMPRESSED) == count(before, COMPRESSED) &&
           count(after, DEPTH) <= count(before, DEPTH) && count(after, MULTISAMPLE) <= count(before, MULTISAMPLE);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-memory-counters <libGLESv2.dylib>");
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
        auto line_of = load<int (*)(char *, size_t)>("ANGLEMetalMemoryLine");

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

        // The pbuffer's own texture first, so that the counts before hold it.
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        GL(Finish)();
        const Counters before = read();
        static std::array<unsigned char, 1 << 20> data{};
        std::array<GLuint, 2> textures{};
        GL(GenTextures)(2, textures.data());
        GL(BindTexture)(GL_TEXTURE_2D, textures[0]);
        GL(TexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, 512, 512);
        GL(TexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 512, 512, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
        GL(BindTexture)(GL_TEXTURE_2D, textures[1]);
        GL(TexStorage2D)(GL_TEXTURE_2D, 1, GL_COMPRESSED_RGB8_ETC2, 512, 512);
        GL(CompressedTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 512, 512, GL_COMPRESSED_RGB8_ETC2, 512 * 512 / 2,
                                    data.data());
        // A framebuffer each, as samples must match: the 4x color one and the depth/stencil one.
        std::array<GLuint, 2> renderbuffers{};
        std::array<GLuint, 2> framebuffers{};
        GL(GenRenderbuffers)(2, renderbuffers.data());
        GL(GenFramebuffers)(2, framebuffers.data());
        GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffers[0]);
        GL(BindRenderbuffer)(GL_RENDERBUFFER, renderbuffers[0]);
        GL(RenderbufferStorageMultisample)(GL_RENDERBUFFER, 4, GL_RGBA8, 256, 256);
        GL(FramebufferRenderbuffer)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffers[0]);
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffers[1]);
        GL(BindRenderbuffer)(GL_RENDERBUFFER, renderbuffers[1]);
        GL(RenderbufferStorage)(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 256, 256);
        GL(FramebufferRenderbuffer)(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, renderbuffers[1]);
        GL(Clear)(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        GLuint buffer = 0;
        GL(GenBuffers)(1, &buffer);
        GL(BindBuffer)(GL_ARRAY_BUFFER, buffer);
        GL(BufferData)(GL_ARRAY_BUFFER, 1 << 20, data.data(), GL_STATIC_DRAW);
        GL(Finish)();
        check(GL(GetError)() == GL_NO_ERROR, "a GL error while making them");

        const Counters made = read();
        std::array<char, 512> line{};
        const int length = line_of(line.data(), line.size());
        std::printf("made: %s\n", line.data());
        check(length > 0 && std::string(line.data()).rfind("gl tex=", 0) == 0, "the line starts with gl tex=");
        check(line_of(line.data(), 8) == 0, "a line that does not fit gives 0");
        check(count(made, COLOR) >= count(before, COLOR) + 1 && bytes(made, COLOR) >= bytes(before, COLOR) + (1 << 20),
              "the RGBA8 texture counts as color with its 1 MiB");
        check(count(made, COMPRESSED) == count(before, COMPRESSED) + 1 &&
                  bytes(made, COMPRESSED) >= bytes(before, COMPRESSED) + 512 * 512 / 2,
              "the ETC2 texture counts as compressed with its 128 KiB");
        check(count(made, DEPTH) >= count(before, DEPTH) + 1 &&
                  bytes(made, DEPTH) >= bytes(before, DEPTH) + 256 * 256 * 4,
              "the DEPTH24_STENCIL8 renderbuffer counts as depth/stencil with at least 256 KiB");
        check(count(made, MULTISAMPLE) >= count(before, MULTISAMPLE) + 1 &&
                  bytes(made, MULTISAMPLE) >= bytes(before, MULTISAMPLE) + 256 * 256 * 4 * 4,
              "the 4x renderbuffer counts as multisample with at least 1 MiB");
        check(made[14] >= before[14] + 256 * 256 * 4 * 5, "the renderbuffers are private");
        check(made[15] >= before[15] + (1 << 20), "the RGBA8 texture is shared");
        check(bytes(made, BUFFER_SHARED) + bytes(made, BUFFER_PRIVATE) >=
                  bytes(before, BUFFER_SHARED) + bytes(before, BUFFER_PRIVATE) + (1 << 20),
              "the buffer counts with its 1 MiB");
        check(made[18] > 0, "the device's allocation is known");

        GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
        GL(DeleteFramebuffers)(2, framebuffers.data());
        GL(DeleteRenderbuffers)(2, renderbuffers.data());
        GL(DeleteTextures)(2, textures.data());
        GL(DeleteBuffers)(1, &buffer);
        Counters after{};
        for (int round = 0; round < 40; ++round) {
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            GL(Finish)();
            after = read();
            if (back(before, after)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        line_of(line.data(), line.size());
        std::printf("deleted: %s\n", line.data());
        check(back(before, after), "the texture classes come back once Metal freed them");

        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(DestroySurface)(display, surface);
        EGL(Terminate)(display);
    }
    if (failed) {
        return 1;
    }
    std::printf("angle-memory-counters: ok\n");
    return 0;
}
