// Test 5 of ADR-0029, for ADR-0041 decision 11: EGL_ANDROID_presentation_time on the Metal backend (patch 0005).
// After eglPresentationTimeANDROID, the surface's next swap presents its drawable with presentDrawable:atTime: at
// the time in seconds, since the guest's CLOCK_MONOTONIC is the host's uptime clock, Core Animation's host time
// (F10). The swap after it presents at once again, as ANGLE's Vulkan backend consumes its time. The test sees what
// Metal calls on the drawables: present for a swap without a time, presentAtTime: with one. The CAMetalLayer lies in
// no window, as in test 2.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-presentation-time: %s\n", what.c_str());
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

// What Metal called on a drawable: present, or presentAtTime: with its time (negative for present).
std::mutex lock;
std::condition_variable called;
std::vector<double> presents;
IMP present_original = nullptr;
IMP present_at_time_original = nullptr;
thread_local int inside = 0;

void note(double time) {
    {
        std::lock_guard<std::mutex> guard(lock);
        presents.push_back(time);
    }
    called.notify_all();
}

// Only the outermost call counts, in case one method calls the other.
void hooked_present(id self, SEL selector) {
    if (inside++ == 0) {
        note(-1.0);
    }
    reinterpret_cast<void (*)(id, SEL)>(present_original)(self, selector);
    inside--;
}

void hooked_present_at_time(id self, SEL selector, CFTimeInterval time) {
    if (inside++ == 0) {
        note(time);
    }
    reinterpret_cast<void (*)(id, SEL, CFTimeInterval)>(present_at_time_original)(self, selector, time);
    inside--;
}

void hook_drawables() {
    CAMetalLayer *scratch = [CAMetalLayer layer];
    scratch.device = MTLCreateSystemDefaultDevice();
    scratch.drawableSize = CGSizeMake(16, 16);
    Class drawable = [[scratch nextDrawable] class];
    if (drawable == nil) {
        fail("no drawable to find the drawables' class");
    }
    present_original = method_setImplementation(class_getInstanceMethod(drawable, @selector(present)),
                                                reinterpret_cast<IMP>(hooked_present));
    present_at_time_original = method_setImplementation(
        class_getInstanceMethod(drawable, @selector(presentAtTime:)), reinterpret_cast<IMP>(hooked_present_at_time));
}

// Metal presents from the command buffer's scheduled handler, on a thread of its own.
double wait_for_present(size_t index) {
    std::unique_lock<std::mutex> guard(lock);
    if (!called.wait_for(guard, std::chrono::seconds(5), [index] { return presents.size() > index; })) {
        fail("swap " + std::to_string(index + 1) + " presented nothing within 5 s");
    }
    return presents[index];
}

void swap(EGLDisplay display, EGLSurface surface) {
    GL(ClearColor)(0.0f, 0.0f, 1.0f, 1.0f);
    GL(Clear)(GL_COLOR_BUFFER_BIT);
    if (EGL(SwapBuffers)(display, surface) != EGL_TRUE) {
        fail("eglSwapBuffers failed");
    }
}

void expect_present(size_t index) {
    const double time = wait_for_present(index);
    if (time >= 0.0) {
        fail("swap " + std::to_string(index + 1) + " presented at a time, " + std::to_string(time) + " s");
    }
    std::printf("swap %zu: present\n", index + 1);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-presentation-time <libGLESv2.dylib>");
    }
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not built, see tools/build-angle.sh\n", argv[1]);
        return 0;
    }
    @autoreleasepool {
        hook_drawables();
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
        const bool announced =
            extensions != nullptr && std::string(extensions).find("EGL_ANDROID_presentation_time") != std::string::npos;
        std::printf("EGL_ANDROID_presentation_time announced: %s\n", announced ? "yes" : "no");
        if (!announced) {
            fail("the Metal display does not announce EGL_ANDROID_presentation_time");
        }
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
        layer.bounds = CGRectMake(0, 0, 64, 64);
        [CATransaction commit];
        EGLSurface surface = EGL(CreateWindowSurface)(
            display, config, reinterpret_cast<EGLNativeWindowType>((__bridge void *)layer), nullptr);
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
            fail("no ES 3.0 context on a window surface");
        }

        swap(display, surface);
        expect_present(0);

        // 100 ms ahead on the uptime clock, in nanoseconds, as a guest's CLOCK_MONOTONIC gives it.
        const auto set_time = load<PFNEGLPRESENTATIONTIMEANDROIDPROC>("EGL_PresentationTimeANDROID");
        const EGLnsecsANDROID time =
            static_cast<EGLnsecsANDROID>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) + 100 * 1000 * 1000;
        if (set_time(display, surface, time) != EGL_TRUE) {
            char error[16];
            std::snprintf(error, sizeof(error), "0x%04X", static_cast<unsigned>(EGL(GetError)()));
            fail(std::string("eglPresentationTimeANDROID failed, error ") + error);
        }
        const double now = CACurrentMediaTime();
        swap(display, surface);
        const double presented_at = wait_for_present(1);
        const double wanted = static_cast<double>(time) / 1e9;
        std::printf("swap 2: presentAtTime: %+.3f ms from the swap, %.3f us off the time set\n",
                    (presented_at - now) * 1e3, (presented_at - wanted) * 1e6);
        if (presented_at < 0.0) {
            fail("swap 2 presented at once, not at the time set");
        }
        if (presented_at - wanted > 1e-6 || wanted - presented_at > 1e-6) {
            fail("swap 2 presented at another time than the one set");
        }

        swap(display, surface);
        expect_present(2);

        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(DestroySurface)(display, surface);
        EGL(Terminate)(display);
    }
    return 0;
}
