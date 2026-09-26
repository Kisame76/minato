// Test 2 of ADR-0029: EGL_ANGLE_window_fixed_size on the Metal backend (patch 0001). A window surface
// with EGL_FIXED_SIZE_ANGLE keeps its drawable at the fixed size when the layer's bounds change, and
// eglSurfaceAttrib(EGL_WIDTH/EGL_HEIGHT) changes that size from the next frame on. The CAMetalLayer lies
// in no window; its changes go through explicit transactions, since this thread has no run loop.
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-fixed-size: %s\n", what.c_str());
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

void set_bounds(CAMetalLayer *layer, CGFloat width, CGFloat height) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.bounds = CGRectMake(0, 0, width, height);
    [CATransaction commit];
}

void frame(EGLDisplay display, EGLSurface surface) {
    GL(ClearColor)(0.0f, 1.0f, 0.0f, 1.0f);
    GL(Clear)(GL_COLOR_BUFFER_BIT);
    if (EGL(SwapBuffers)(display, surface) != EGL_TRUE) {
        fail("eglSwapBuffers failed");
    }
}

void expect_size(const char *when, CAMetalLayer *layer, EGLDisplay display, EGLSurface surface, int width,
                 int height) {
    EGLint egl_width = 0;
    EGLint egl_height = 0;
    EGL(QuerySurface)(display, surface, EGL_WIDTH, &egl_width);
    EGL(QuerySurface)(display, surface, EGL_HEIGHT, &egl_height);
    const int drawable_width = static_cast<int>(layer.drawableSize.width);
    const int drawable_height = static_cast<int>(layer.drawableSize.height);
    std::printf("%s: drawableSize %d x %d, EGL_WIDTH x EGL_HEIGHT %d x %d\n", when, drawable_width, drawable_height,
                egl_width, egl_height);
    if (drawable_width != width || drawable_height != height || egl_width != width || egl_height != height) {
        fail(std::string(when) + ": expected " + std::to_string(width) + " x " + std::to_string(height));
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-fixed-size <libGLESv2.dylib>");
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
        const char *extensions = EGL(QueryString)(display, EGL_EXTENSIONS);
        std::printf("EGL_ANGLE_window_fixed_size announced: %s\n",
                    extensions != nullptr && std::string(extensions).find("EGL_ANGLE_window_fixed_size") !=
                                                 std::string::npos
                        ? "yes"
                        : "no");
        const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                            EGL_RED_SIZE,     8,              EGL_GREEN_SIZE,      8,
                                            EGL_BLUE_SIZE,    8,              EGL_ALPHA_SIZE,      8,
                                            EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        if (EGL(ChooseConfig)(display, config_attributes, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            fail("no RGBA8888 window config for ES 3");
        }

        CAMetalLayer *layer = [CAMetalLayer layer];
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contentsScale = 2.0;
        [CATransaction commit];
        set_bounds(layer, 400, 300);

        const EGLint surface_attributes[] = {EGL_FIXED_SIZE_ANGLE, EGL_TRUE, EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE};
        EGLSurface surface = EGL(CreateWindowSurface)(
            display, config, reinterpret_cast<EGLNativeWindowType>((__bridge void *)layer), surface_attributes);
        if (surface == EGL_NO_SURFACE) {
            char error[16];
            std::snprintf(error, sizeof(error), "0x%04X", static_cast<unsigned>(EGL(GetError)()));
            fail(std::string("eglCreateWindowSurface with EGL_FIXED_SIZE_ANGLE failed, error ") + error);
        }
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
        if (context == EGL_NO_CONTEXT || EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
            fail("no ES 3.0 context on the window surface");
        }

        frame(display, surface);
        frame(display, surface);
        set_bounds(layer, 500, 200);
        frame(display, surface);
        expect_size("after the layer's bounds changed", layer, display, surface, 256, 256);

        if (EGL(SurfaceAttrib)(display, surface, EGL_WIDTH, 128) != EGL_TRUE ||
            EGL(SurfaceAttrib)(display, surface, EGL_HEIGHT, 64) != EGL_TRUE) {
            fail("eglSurfaceAttrib(EGL_WIDTH/EGL_HEIGHT) failed");
        }
        frame(display, surface);
        expect_size("after eglSurfaceAttrib 128 x 64", layer, display, surface, 128, 64);

        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(DestroySurface)(display, surface);
        EGL(Terminate)(display);
    }
    return 0;
}
