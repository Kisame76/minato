// Patch 0030 on ANGLE's macOS slice: which ANGLE lock a thread parked at a full FIFO still holds, the invariant the
// embedder's park gate rests on (it parks only after onStop and every surfaceDestroyed are answered, as then no other
// thread calls EGL). R (context A) fills the background FIFO (patch 0025, 32 command buffers) and hits the full FIFO
// with one more call, which parks it until the foreground the test brings 1500 ms later; once R is parked, L (context
// B, its own share group, surfaceless) times eglMakeCurrent(B), glFlush on B, eglCreatePbufferSurface +
// eglDestroySurface and eglMakeCurrent(none).
// - swap: R parks in eglSwapBuffers (a CAMetalLayer window surface). It holds ANGLE's global EGL mutex: L's
//   eglMakeCurrent waits until R is back (at least 1000 ms).
// - flush: R parks in glFlush. It holds only its share group's context mutex: every call of L is back within 500 ms.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {
void *library = nullptr;
[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-park-locks: %s\n", what.c_str());
    std::exit(1);
}
template <typename F> F load(const char *name) {
    void *address = dlsym(library, name);
    if (!address) fail(std::string("no ") + name);
    return reinterpret_cast<F>(address);
}
#define EGL(name) load<decltype(&egl##name)>("EGL_" #name)
#define GL(name) load<decltype(&gl##name)>("gl" #name)

std::mutex m;
std::condition_variable cv;
bool background = false, parked = false;
int enter() { std::lock_guard<std::mutex> l(m); return background ? 0 : 1; }
void counted() {}
void gate_unused() { fail("gate called"); }
void wait_foreground() { std::unique_lock<std::mutex> l(m); cv.wait(l, [] { return !background; }); }
int park() {
    std::unique_lock<std::mutex> l(m);
    parked = true;
    cv.notify_all();
    cv.wait(l, [] { return !background; });
    return 1;
}
double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
}  // namespace

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[2], "flush") != 0 && strcmp(argv[2], "swap") != 0)) {
        fail("usage: angle-park-locks <libGLESv2.dylib> flush|swap");
    }
    const bool swap = !strcmp(argv[2], "swap");
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not built, see tools/build-angle.sh\n", argv[1]);
        return 0;
    }
    double times[4] = {0, 0, 0, 0};
    @autoreleasepool {
        library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!library) fail(dlerror());
        load<void (*)(void (*)(void), void (*)(void))>("ANGLEMetalSetCommitGates")(gate_unused, counted);
        load<void (*)(int (*)(void), void (*)(void))>("ANGLEMetalSetCommitDeferral")(enter, wait_foreground);
        load<void (*)(int (*)(void))>("ANGLEMetalSetDeferralFull")(park);
        const EGLAttrib da[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
        EGLDisplay dpy = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, da);
        if (dpy == EGL_NO_DISPLAY || !EGL(Initialize)(dpy, nullptr, nullptr)) fail("no display");
        const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                             EGL_SURFACE_TYPE,    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                             EGL_RED_SIZE,        8,
                             EGL_GREEN_SIZE,      8,
                             EGL_BLUE_SIZE,       8,
                             EGL_ALPHA_SIZE,      8,
                             EGL_NONE};
        EGLConfig cfg = nullptr;
        EGLint n = 0;
        if (!EGL(ChooseConfig)(dpy, ca, &cfg, 1, &n) || n < 1) fail("no config");
        const EGLint xa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        EGLContext a = EGL(CreateContext)(dpy, cfg, EGL_NO_CONTEXT, xa);
        EGLContext b = EGL(CreateContext)(dpy, cfg, EGL_NO_CONTEXT, xa);
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.frame = CGRectMake(0, 0, 64, 64);
        layer.drawableSize = CGSizeMake(64, 64);
        EGLSurface window = EGL_NO_SURFACE;
        if (swap) {
            window = EGL(CreateWindowSurface)(dpy, cfg, (__bridge EGLNativeWindowType)layer, nullptr);
            if (window == EGL_NO_SURFACE) fail("no window surface");
        }
        std::thread r([&] {
            if (!EGL(MakeCurrent)(dpy, window, window, a)) fail("R: make current");
            if (!swap) {  // surfaceless: draw into a texture, or no command buffer is ever made
                GLuint texture = 0, framebuffer = 0;
                GL(GenTextures)(1, &texture);
                GL(BindTexture)(GL_TEXTURE_2D, texture);
                GL(TexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                GL(GenFramebuffers)(1, &framebuffer);
                GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
                GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            }
            GL(ClearColor)(1, 0, 0, 1);
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            if (swap) EGL(SwapBuffers)(dpy, window);
            GL(Finish)();
            { std::lock_guard<std::mutex> l(m); background = true; }
            for (int i = 0; i < 32; i++) {  // the FIFO's 32 command buffers
                GL(ClearColor)(0, static_cast<float>(i) / 32.0f, 0, 1);
                GL(Clear)(GL_COLOR_BUFFER_BIT);
                GL(Flush)();
            }
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            if (swap) EGL(SwapBuffers)(dpy, window); else GL(Flush)();  // the full FIFO: parks here
            EGL(MakeCurrent)(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        });
        {
            std::unique_lock<std::mutex> l(m);
            if (!cv.wait_for(l, std::chrono::seconds(10), [] { return parked; })) fail("R never parked");
        }
        const auto parked_at = std::chrono::steady_clock::now();
        std::thread fg([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            std::lock_guard<std::mutex> l(m);
            background = false;
            cv.notify_all();
        });
        std::thread lt([&] {
            auto t = std::chrono::steady_clock::now();
            EGL(MakeCurrent)(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, b);
            times[0] = ms_since(t);
            t = std::chrono::steady_clock::now();
            GL(Flush)();
            times[1] = ms_since(t);
            const EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
            t = std::chrono::steady_clock::now();
            EGLSurface p = EGL(CreatePbufferSurface)(dpy, cfg, pa);
            EGL(DestroySurface)(dpy, p);
            times[2] = ms_since(t);
            t = std::chrono::steady_clock::now();
            EGL(MakeCurrent)(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            times[3] = ms_since(t);
        });
        lt.join();
        const double done = ms_since(parked_at);
        fg.join();
        r.join();
        std::printf("%s: R parked in %s; L's eglMakeCurrent(B) %.0f ms, glFlush %.0f ms, pbuffer create + destroy "
                    "%.0f ms, eglMakeCurrent(none) %.0f ms; done %.0f ms after the park (foreground at 1500 ms)\n",
                    argv[2], swap ? "eglSwapBuffers" : "glFlush", times[0], times[1], times[2], times[3], done);
        if (swap && times[0] < 1000) {
            fail("a park in eglSwapBuffers must hold the global EGL mutex: another thread's eglMakeCurrent waits");
        }
        if (!swap && (times[0] > 500 || times[1] > 500 || times[2] > 500 || times[3] > 500)) {
            fail("a park in glFlush must hold only its share group's mutex: another group's calls go on");
        }
        if (window != EGL_NO_SURFACE) EGL(DestroySurface)(dpy, window);
        EGL(DestroyContext)(dpy, a);
        EGL(DestroyContext)(dpy, b);
        EGL(Terminate)(dpy);
    }
    return 0;
}
