// A 4x multisampled window surface on Metal (patch 0016): its multisample color lives on the GPU's tiles only
// (memoryless), as an Android tiler keeps an EGL surface's samples, and the drawable holds the resolved frame. A frame
// that leaves the window for a framebuffer object in its middle and comes back keeps what it drew before (ANGLE loads
// the resolved color back into the samples). Checked with patch 0015's counters: the window's multisample color
// counts as memoryless, and the multisample class holds no more than the window's depth/stencil samples.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void *library = nullptr;
bool failed = false;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-window-msaa: %s\n", what.c_str());
    std::exit(1);
}

void check(bool condition, const std::string &what) {
    if (!condition) {
        std::fprintf(stderr, "angle-window-msaa: FAIL %s\n", what.c_str());
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

constexpr int kSide = 256;

// Patch 0015's counters: count and bytes per class; multisample is class 3, memoryless class 4.
std::array<uint64_t, 19> counters() {
    std::array<uint64_t, 19> values{};
    load<size_t (*)(uint64_t *, size_t)>("ANGLEMetalMemoryCounters")(values.data(), values.size());
    return values;
}

void clear_half(bool left, float r, float g, float b) {
    GL(Enable)(GL_SCISSOR_TEST);
    GL(Scissor)(left ? 0 : kSide / 2, 0, kSide / 2, kSide);
    GL(ClearColor)(r, g, b, 1.0f);
    GL(Clear)(GL_COLOR_BUFFER_BIT);
    GL(Disable)(GL_SCISSOR_TEST);
}

std::array<GLubyte, 4> pixel(int x, int y) {
    std::array<GLubyte, 4> value{};
    GL(ReadPixels)(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, value.data());
    return value;
}

bool is(const std::array<GLubyte, 4> &value, int r, int g, int b) {
    return std::abs(value[0] - r) < 8 && std::abs(value[1] - g) < 8 && std::abs(value[2] - b) < 8;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-window-msaa <libGLESv2.dylib>");
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
        const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_SAMPLE_BUFFERS, 1,
                                            EGL_SAMPLES, 4, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        if (EGL(ChooseConfig)(display, config_attributes, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            fail("no 4x RGBA8888 window config with depth and stencil for ES 3");
        }
        CAMetalLayer *layer = [CAMetalLayer layer];
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contentsScale = 1.0;
        layer.bounds = CGRectMake(0, 0, kSide, kSide);
        [CATransaction commit];
        const EGLint surface_attributes[] = {EGL_FIXED_SIZE_ANGLE, EGL_TRUE, EGL_WIDTH, kSide, EGL_HEIGHT, kSide,
                                             EGL_NONE};
        EGLSurface surface = EGL(CreateWindowSurface)(
            display, config, reinterpret_cast<EGLNativeWindowType>((__bridge void *)layer), surface_attributes);
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
            fail("no ES 3.0 context on a 4x window surface");
        }

        // A framebuffer object to leave the window for in the middle of a frame.
        GLuint texture = 0;
        GLuint framebuffer = 0;
        GL(GenTextures)(1, &texture);
        GL(BindTexture)(GL_TEXTURE_2D, texture);
        GL(TexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
        GL(GenFramebuffers)(1, &framebuffer);
        GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
        GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

        for (int frame = 0; frame < 2; ++frame) {
            GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
            GL(Viewport)(0, 0, kSide, kSide);
            GL(ClearColor)(0, 0, 0, 1);
            GL(Clear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            clear_half(true, 1, 0, 0);
            GL(Flush)();
            GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
            GL(ClearColor)(0, 0, 1, 1);
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
            clear_half(false, 0, 1, 0);
            const auto left = pixel(kSide / 4, kSide / 2);
            const auto right = pixel(3 * kSide / 4, kSide / 2);
            check(is(left, 255, 0, 0), "frame " + std::to_string(frame) + ": the left half drawn before leaving the "
                                                                          "window is still red");
            check(is(right, 0, 255, 0), "frame " + std::to_string(frame) + ": the right half is green");
            check(GL(GetError)() == GL_NO_ERROR, "no GL error in frame " + std::to_string(frame));
            check(EGL(SwapBuffers)(display, surface) == EGL_TRUE, "the swap of frame " + std::to_string(frame));
        }

        const auto values = counters();
        const uint64_t samples_depth_stencil = uint64_t{kSide} * kSide * 8 * 4;
        std::printf("multisample %llu (%llu bytes), memoryless %llu\n", (unsigned long long)values[6],
                    (unsigned long long)values[7], (unsigned long long)values[8]);
        check(values[8] >= 1, "the window's multisample color counts as memoryless");
        check(values[7] <= samples_depth_stencil + samples_depth_stencil / 8,
              "the multisample class holds no more than the window's depth/stencil samples");

        GL(DeleteFramebuffers)(1, &framebuffer);
        GL(DeleteTextures)(1, &texture);
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(DestroySurface)(display, surface);
        EGL(Terminate)(display);
    }
    if (failed) {
        return 1;
    }
    std::printf("angle-window-msaa: ok\n");
    return 0;
}
