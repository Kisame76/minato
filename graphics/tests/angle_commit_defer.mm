// Patches 0025 and 0027 on ANGLE's macOS slice: deferred commits in the background (ADR-0029, decision 15), and no
// wait there. The test plays the
// embedder: its `enter` lets a commit go while it is in the foreground and holds it back while it is "in the
// background", and its `waitForeground` waits until it is in the foreground again. A context with no surface:
// - background: a texture upload, a clear of a framebuffer on that texture and glFlush return at once, and so does
//   eglMakeCurrent with no context (its flush waits until scheduled); nothing is committed meanwhile
//   (ANGLEMetalWaitUntilScheduled answers 1 at once), and the park line's counters show the deferred buffers;
// - glFinish, which wants a result, returns at once in the background without it (patch 0027: no GL call parks its
//   thread there), counted as skipped; back in the foreground a readback commits the deferred work first, and the
//   texture holds what the background drew;
// - a FIFO that is full (32 command buffers) commits its oldest instead of a wait (iOS refuses that commit in the
//   background: its work is lost, as a dropped frame's), so 40 flushes return at once; the park line counts them.
// Every commit `enter` let go reached the embedder's second function once.
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
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-commit-defer: %s\n", what.c_str());
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

std::mutex state_mutex;
std::condition_variable state_changed;
bool background = false;
int entered = 0;
int waits = 0;
std::atomic<int> counted{0};

int enter() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (background) {
        return 0;
    }
    ++entered;
    return 1;
}

void counted_now() {
    ++counted;
}

void gate_unused() {
    fail("patch 0003's gate was called although the deferral is set");
}

// The threads that waited for the foreground: ANGLE's own submitter may, the app's GL threads must not (patch 0027).
std::set<std::thread::id> waiters;

bool waited_on(std::thread::id thread) {
    std::lock_guard<std::mutex> lock(state_mutex);
    return waiters.count(thread) != 0;
}

void wait_foreground() {
    std::unique_lock<std::mutex> lock(state_mutex);
    ++waits;
    waiters.insert(std::this_thread::get_id());
    state_changed.wait(lock, [] { return !background; });
}

void set_background(bool value) {
    std::lock_guard<std::mutex> lock(state_mutex);
    background = value;
    state_changed.notify_all();
}

void (*deferred_counters)(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *) = nullptr;
uint64_t (*deferred_skipped)(void) = nullptr;

struct Counters {
    uint64_t now = 0, bytes = 0, peak = 0, total = 0, full = 0;
};

Counters counters() {
    Counters c;
    deferred_counters(&c.now, &c.bytes, &c.peak, &c.total, &c.full);
    return c;
}

long long ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-commit-defer <libGLESv2.dylib>");
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
        auto set_gates = load<void (*)(void (*)(void), void (*)(void))>("ANGLEMetalSetCommitGates");
        auto set_deferral = load<void (*)(int (*)(void), void (*)(void))>("ANGLEMetalSetCommitDeferral");
        auto wait_until_scheduled = load<int (*)(double)>("ANGLEMetalWaitUntilScheduled");
        deferred_counters =
            load<void (*)(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *)>("ANGLEMetalDeferredCounters");
        deferred_skipped = load<uint64_t (*)(void)>("ANGLEMetalDeferredSkipped");
        set_gates(gate_unused, counted_now);
        set_deferral(enter, wait_foreground);

        const EGLAttrib display_attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                                                EGL_NONE};
        EGLDisplay display = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, display_attributes);
        if (display == EGL_NO_DISPLAY || EGL(Initialize)(display, nullptr, nullptr) != EGL_TRUE) {
            fail("no Metal display");
        }
        const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        if (EGL(ChooseConfig)(display, config_attributes, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            fail("no ES 3.0 config");
        }
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
        if (EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
            fail("no surfaceless ES 3.0 context");
        }
        enum { kSide = 64 };
        GLuint texture = 0, framebuffer = 0;
        GL(GenTextures)(1, &texture);
        GL(BindTexture)(GL_TEXTURE_2D, texture);
        GL(TexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA, kSide, kSide, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        GL(GenFramebuffers)(1, &framebuffer);
        GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
        GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        GL(ClearColor)(1, 0, 0, 1);
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        // A scissored clear draws, and its pipeline's first build takes a while: once here, not in the background.
        GL(Enable)(GL_SCISSOR_TEST);
        GL(Scissor)(0, 0, 1, 1);
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        GL(Disable)(GL_SCISSOR_TEST);
        GL(Finish)();
        std::printf("foreground: a surfaceless context's first work committed and done\n");

        // Background: an upload, a clear and glFlush; then the unbind with its flush that waits until scheduled.
        set_background(true);
        std::vector<GLubyte> pixels(kSide * kSide * 4);
        for (size_t index = 0; index < pixels.size(); index += 4) {
            pixels[index] = 0, pixels[index + 1] = 0, pixels[index + 2] = 255, pixels[index + 3] = 255;
        }
        auto start = std::chrono::steady_clock::now();
        GL(TexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, kSide, kSide, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        GL(Flush)();
        GL(Enable)(GL_SCISSOR_TEST);
        GL(Scissor)(0, 0, kSide / 2, kSide);
        GL(ClearColor)(0, 1, 0, 1);
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        GL(Disable)(GL_SCISSOR_TEST);
        GL(Flush)();
        const long long flushed = ms_since(start);
        start = std::chrono::steady_clock::now();
        const EGLBoolean unbound = EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        const long long unbind = ms_since(start);
        const int scheduled = wait_until_scheduled(0.0);
        const Counters held = counters();
        std::printf("background: upload, clear and glFlush returned at once: %s; eglMakeCurrent(none): %s, at once: "
                    "%s; nothing committed meanwhile: %s; deferred now: %s\n",
                    flushed < 100 ? "yes" : "no", unbound ? "EGL_TRUE" : "EGL_FALSE", unbind < 100 ? "yes" : "no",
                    scheduled == 1 ? "yes" : "no", held.now >= 1 && held.total >= 1 ? "1 or more" : "none");
        if (flushed >= 100 || !unbound || unbind >= 100 || scheduled != 1 || held.now < 1) {
            fail("the background's work did not return at once, or was committed");
        }

        // glFinish wants a result: in the background it returns at once without it (patch 0027).
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
        const uint64_t skipped = deferred_skipped();
        start = std::chrono::steady_clock::now();
        GL(Finish)();
        const long long finish = ms_since(start);
        const bool skips = deferred_skipped() > skipped;
        set_background(false);
        GLubyte left[4] = {0, 0, 0, 0}, right[4] = {0, 0, 0, 0};
        GL(ReadPixels)(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
        GL(ReadPixels)(kSide - 1, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
        std::printf("glFinish in the background returned at once: %s, its wait skipped: %s; back in the foreground the "
                    "texture holds the background's clear %u %u %u %u and upload %u %u %u %u; deferred now: %llu\n",
                    finish < 100 && !waited_on(std::this_thread::get_id()) ? "yes" : "no", skips ? "yes" : "no",
                    left[0], left[1],
                    left[2], left[3], right[0], right[1], right[2], right[3],
                    static_cast<unsigned long long>(counters().now));
        const GLubyte green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
        if (finish >= 100 || waited_on(std::this_thread::get_id()) || !skips || memcmp(left, green, 4) != 0 ||
            memcmp(right, blue, 4) != 0 || counters().now != 0) {
            fail("glFinish waited in the background, or the background's work is lost");
        }

        // A full FIFO: its oldest command buffer is committed instead of a wait, so every flush returns.
        set_background(true);
        std::atomic<int> done{0};
        {
            // A thread's id may be an ended thread's again: only waits from now on count.
            std::lock_guard<std::mutex> lock(state_mutex);
            waiters.clear();
        }
        std::thread::id flushing;
        std::thread flusher([&] {
            flushing = std::this_thread::get_id();
            EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
            for (int index = 0; index < 40; index++) {
                GL(ClearColor)(0, 0, static_cast<float>(index % 2), 1);
                GL(Clear)(GL_COLOR_BUFFER_BIT);
                GL(Flush)();
                done = index + 1;
            }
            GL(Finish)();
            EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        });
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const Counters full = counters();
        const int before = done;
        set_background(false);
        flusher.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // ANGLE's submitter commits the FIFO
        std::printf("a full FIFO: %d flushes and glFinish returned in the background, none waited: %s; deferred at "
                    "the most: %llu, commits that found it full: %s; after the foreground: deferred now %llu\n",
                    before, !waited_on(flushing) ? "yes" : "no", static_cast<unsigned long long>(full.now),
                    full.full >= 1 ? "1 or more" : "none", static_cast<unsigned long long>(counters().now));
        if (before != 40 || waited_on(flushing) || full.full < 1 || full.now != 32 || counters().now != 0) {
            fail("a full FIFO made a commit wait, or kept more than 32");
        }

        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
        GL(DeleteFramebuffers)(1, &framebuffer);
        GL(DeleteTextures)(1, &texture);
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(Terminate)(display);
        if (counted.load() != entered) {
            fail("the second function came " + std::to_string(counted.load()) + " times for " +
                 std::to_string(entered) + " commits let go");
        }
        std::printf("each of the %d commits let go reached the second function\n", entered);
    }
    return 0;
}
