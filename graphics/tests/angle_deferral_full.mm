// Patch 0030 on ANGLE's macOS slice: what a full FIFO in the background does with the work kept for the foreground.
// In the background (patch 0025) a texture is drawn once (cleared to blue, the app's one upload) and then 40 other
// frames are flushed, more than the FIFO's 32 command buffers. Two runs:
// - drop: no embedder answer for a full FIFO, and MINATO_ANGLE_DROP_DISCARDS=1, which does to a dropped command buffer
//   what iOS does to a background commit (nothing reaches the GPU). The FIFO's oldest goes (dfull above 0), the blue
//   is lost: back in the foreground the texture still reads red. This is the loss the iPhone shows and the Mac's Metal,
//   which runs the commit, would hide.
// - park: the embedder's answer (ANGLEMetalSetDeferralFull) parks the committing thread until the foreground; a thread
//   of the test brings the foreground 300 ms later. Nothing is dropped (dfull 0, parked above 0), and back in the
//   foreground the texture reads blue, as on Android, whose GPU runs in the background.
#import <Metal/Metal.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-deferral-full: %s\n", what.c_str());
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
std::atomic<int> parks{0};

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

// The embedder's answer to a full FIFO in the park run: parked until the foreground.
int park_until_foreground() {
    ++parks;
    wait_foreground();
    return 1;
}

void set_background(bool value) {
    std::lock_guard<std::mutex> lock(state_mutex);
    background = value;
    state_changed.notify_all();
}

GLuint target(GLuint *texture) {
    GL(GenTextures)(1, texture);
    GL(BindTexture)(GL_TEXTURE_2D, *texture);
    GL(TexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLuint framebuffer = 0;
    GL(GenFramebuffers)(1, &framebuffer);
    GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
    GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
    return framebuffer;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3 || (std::strcmp(argv[2], "drop") != 0 && std::strcmp(argv[2], "park") != 0)) {
        fail("usage: angle-deferral-full <libGLESv2.dylib> drop|park");
    }
    const bool park = std::strcmp(argv[2], "park") == 0;
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not built, see tools/build-angle.sh\n", argv[1]);
        return 0;
    }
    if (!park) {
        // Read once, at ANGLE's first full FIFO; a value set outside stays (=0 shows the Mac running the drop).
        setenv("MINATO_ANGLE_DROP_DISCARDS", "1", 0);
    }
    @autoreleasepool {
        library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            fail(std::string("dlopen: ") + dlerror());
        }
        load<void (*)(void (*)(void), void (*)(void))>("ANGLEMetalSetCommitGates")(gate_unused, counted);
        load<void (*)(int (*)(void), void (*)(void))>("ANGLEMetalSetCommitDeferral")(enter, wait_foreground);
        load<void (*)(int (*)(void))>("ANGLEMetalSetDeferralFull")(park ? park_until_foreground : nullptr);
        auto deferred_counters =
            load<void (*)(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *)>("ANGLEMetalDeferredCounters");
        auto deferred_parked = load<uint64_t (*)(void)>("ANGLEMetalDeferredParked");

        const EGLAttrib display_attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                                                EGL_NONE};
        EGLDisplay display = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, display_attributes);
        if (display == EGL_NO_DISPLAY || EGL(Initialize)(display, nullptr, nullptr) != EGL_TRUE) {
            fail("no Metal display");
        }
        const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        if (EGL(ChooseConfig)(display, config_attributes, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            fail("no ES 3.0 config");
        }
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
        if (EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
            fail("no surfaceless ES 3.0 context");
        }
        GLuint texture = 0, other = 0;
        const GLuint upload = target(&texture);
        const GLuint frames = target(&other);
        GL(BindFramebuffer)(GL_FRAMEBUFFER, upload);
        GL(ClearColor)(1, 0, 0, 1);
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        GL(Finish)();

        set_background(true);
        std::thread foreground;
        if (park) {
            foreground = std::thread([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                set_background(false);
            });
        }
        // The one upload: the texture cleared to blue, in a command buffer of its own.
        GL(BindFramebuffer)(GL_FRAMEBUFFER, upload);
        GL(ClearColor)(0, 0, 1, 1);
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        GL(Flush)();
        // 40 frames more than the FIFO holds (32 command buffers).
        GL(BindFramebuffer)(GL_FRAMEBUFFER, frames);
        for (int frame = 0; frame < 40; frame++) {
            GL(ClearColor)(0, static_cast<float>(frame) / 40.0f, 0, 1);
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            GL(Flush)();
        }
        uint64_t now = 0, bytes = 0, peak = 0, total = 0, full = 0;
        deferred_counters(&now, &bytes, &peak, &total, &full);
        const uint64_t parked = deferred_parked();
        if (park) {
            foreground.join();
        } else {
            set_background(false);
        }

        GL(BindFramebuffer)(GL_FRAMEBUFFER, upload);
        GLubyte pixel[4] = {0, 0, 0, 0};
        GL(ReadPixels)(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        const bool blue = pixel[0] == 0 && pixel[2] == 255;
        std::printf("%s: full FIFOs %llu, parked %llu (the embedder's answer called %d times); back in the foreground "
                    "the background's upload reads %u %u %u %u: %s\n",
                    park ? "park" : "drop", static_cast<unsigned long long>(full),
                    static_cast<unsigned long long>(parked), parks.load(), pixel[0], pixel[1], pixel[2], pixel[3],
                    blue ? "kept" : "lost");
        if (park && (full != 0 || parked == 0 || !blue)) {
            fail("parking at a full FIFO must keep the background's work: nothing dropped, the upload blue");
        }
        if (!park && (full == 0 || blue)) {
            fail("a drop without the commit must lose the oldest command buffer: dfull above 0, the upload not blue");
        }

        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(Terminate)(display);
    }
    return 0;
}
