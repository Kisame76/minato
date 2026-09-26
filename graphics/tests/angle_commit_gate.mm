// Test 4 of ADR-0029: the commit gate of patch 0003. A command buffer that waits on an unsignaled
// MTLSharedEvent (EGL_ANGLE_metal_shared_event_sync) cannot be scheduled once it is committed. With a
// gate that holds, a glFlush of such a buffer on a second thread waits in the gate, and
// ANGLEMetalWaitUntilScheduled, called from a thread without a context, still returns at once: the
// held buffer is not committed. Once the gate opens, the same call times out until the event is
// signaled, and then returns.
#import <Metal/Metal.h>

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
#include <mutex>
#include <string>
#include <thread>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-commit-gate: %s\n", what.c_str());
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

// The gate: holds every commit while `closed`, and counts the commits that reached it.
std::mutex gate_mutex;
std::condition_variable gate_changed;
bool closed = false;
int arrived = 0;

void gate() {
    std::unique_lock<std::mutex> lock(gate_mutex);
    ++arrived;
    gate_changed.notify_all();
    gate_changed.wait(lock, [] { return !closed; });
}

void set_closed(bool value) {
    std::lock_guard<std::mutex> lock(gate_mutex);
    closed = value;
    gate_changed.notify_all();
}

bool wait_for_arrivals(int count) {
    std::unique_lock<std::mutex> lock(gate_mutex);
    return gate_changed.wait_for(lock, std::chrono::seconds(5), [count] { return arrived >= count; });
}

int arrivals() {
    std::lock_guard<std::mutex> lock(gate_mutex);
    return arrived;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-commit-gate <libGLESv2.dylib>");
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
        auto set_commit_gate = load<void (*)(void (*)(void))>("ANGLEMetalSetCommitGate");
        auto wait_until_scheduled = load<int (*)(double)>("ANGLEMetalWaitUntilScheduled");
        set_commit_gate(gate);

        const EGLAttrib display_attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                                                EGL_NONE};
        EGLDisplay display = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, display_attributes);
        if (display == EGL_NO_DISPLAY || EGL(Initialize)(display, nullptr, nullptr) != EGL_TRUE) {
            fail("no Metal display");
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLSharedEvent> event = [device newSharedEvent];

        // The context lives on the render thread; the main thread steers and checks.
        std::atomic<int> step{0};
        std::atomic<bool> flushed{false};
        std::atomic<bool> failed{false};
        std::thread render([&] {
            const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                                EGL_OPENGL_ES3_BIT, EGL_NONE};
            EGLConfig config = nullptr;
            EGLint configs = 0;
            const EGLint surface_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
            const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
            if (EGL(ChooseConfig)(display, config_attributes, &config, 1, &configs) != EGL_TRUE || configs < 1) {
                failed = true;
                step = 1;
                return;
            }
            EGLSurface surface = EGL(CreatePbufferSurface)(display, config, surface_attributes);
            EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
            if (EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
                failed = true;
                step = 1;
                return;
            }
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            GL(Finish)();
            step = 1;  // the gate is open: the first work is committed and done

            while (step != 2) {
                std::this_thread::yield();
            }
            // A GPU wait for the event, then work behind it: once committed, it cannot be scheduled.
            const EGLAttrib sync_attributes[] = {
                EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE, reinterpret_cast<EGLAttrib>((__bridge void *)event),
                EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, 1,
                EGL_SYNC_CONDITION, EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE,
                EGL_NONE};
            EGLSync sync = EGL(CreateSync)(display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, sync_attributes);
            if (sync == EGL_NO_SYNC || EGL(WaitSync)(display, sync, 0) != EGL_TRUE) {
                failed = true;
                flushed = true;
                return;
            }
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            GL(Flush)();  // waits in the closed gate
            flushed = true;

            while (step != 3) {
                std::this_thread::yield();
            }
            GL(Finish)();
            EGL(DestroySync)(display, sync);
            EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            EGL(DestroyContext)(display, context);
            EGL(DestroySurface)(display, surface);
        });

        while (step != 1) {
            std::this_thread::yield();
        }
        if (failed) {
            fail("no ES 3.0 context on a pbuffer");
        }
        const int before = arrivals();
        std::printf("gate open: the first work passed the gate %d time(s)\n", before);

        set_closed(true);
        step = 2;
        if (!wait_for_arrivals(before + 1)) {
            fail(failed ? "no EGL_ANGLE_metal_shared_event_sync wait" : "glFlush never reached the gate");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (flushed) {
            fail("glFlush returned while the gate was closed");
        }
        const auto start = std::chrono::steady_clock::now();
        if (wait_until_scheduled(1.0) != 1) {
            fail("ANGLEMetalWaitUntilScheduled timed out: the command buffer held at the gate was committed");
        }
        const auto waited =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::printf("gate closed: glFlush waits in it, uncommitted; ANGLEMetalWaitUntilScheduled returned in %lld ms\n",
                    static_cast<long long>(waited));

        set_closed(false);
        while (!flushed) {
            std::this_thread::yield();
        }
        std::printf("gate opened: glFlush returned\n");
        if (wait_until_scheduled(0.3) != 0) {
            fail("ANGLEMetalWaitUntilScheduled did not time out behind a wait on an unsignaled event");
        }
        std::printf("committed behind an unsignaled event: ANGLEMetalWaitUntilScheduled timed out\n");
        event.signaledValue = 1;
        if (wait_until_scheduled(2.0) != 1) {
            fail("ANGLEMetalWaitUntilScheduled timed out after the event was signaled");
        }
        std::printf("event signaled: every committed command buffer is scheduled\n");

        step = 3;
        render.join();
        EGL(Terminate)(display);
    }
    return 0;
}
