// Texture parking (patch 0017): near the process's memory limit ANGLE's Metal backend keeps textures that were not
// used for a while as a compressed CPU copy and restores them on their next use, with identical pixels. Like a frame
// sequence in a visual novel: 240 textures of 1920 x 1080 RGBA8 with picture-like content (gradients, edges and fine
// noise, which packs about as well as real art: 1.6x with LZ4) are made one per frame and drawn into a 1920 x 1080
// framebuffer, then shown a second time in the same order. With MINATO_ANGLE_PARK_LIMIT_MB set to the process's
// footprint plus 1.75 GiB, the footprint stays below that limit (without parking it grows by 2 GiB), every frame
// reads back the exact texels uploaded (hash), and no frame stalls for long (GL work per frame: p50 below 25 ms,
// p99 below 250 ms). Each frame ends with a swap of a window surface, as an app presents. Patch 0018:
// ANGLEMetalParkingLine names the least and the last available memory and the time between presents, and parking
// starts below 1.5 GiB available (stops at 2 GiB), above the ~1 GiB where the iPhone starts its memory warnings:
// --warning-band puts the limit 2.5 GiB above the start, as on the device (a ~6 GB limit), and prints the least
// memory that stayed available. Patch 0019: --in-use makes the 240 frames once and then draws the first 128 of them
// in every frame for 3 s, as an app whose working set comes back: all of them are restored and stay in use, so none
// can be parked again, and their packed copies, kept to make a later park free, must go in the emergency band (below
// 384 MiB available): at the end no copy is kept unless at least that much is available (0018 kept ~250 MiB at ~280
// MiB available). How much stays available depends on how far the background repacks of the other textures got,
// which a loaded Mac delays; it is printed. Patch 0020: the first frame restores all 128 at once, whose storage and
// staging buffers (8 MiB each) reach the footprint only as their command buffer runs, so the memory available lags;
// at most 64 MiB of staging may be on its way while parking is needed, and the first restore in the emergency band
// lets go the copies kept before: while they are restored (the footprint sampled after every draw of the first
// frames) at least 256 MiB stay available. Its CTest run turns off ASan's quarantine, which would hold freed memory
// in the footprint.
// Patch 0021: --store makes and shows the 240 frames as the plain run does, waits until every idle texture is parked
// and repacked, and then the packed copies must not be in the footprint: it may hold the frames still on the GPU and
// 128 MiB more, not the ~1 GiB of packed copies (the iPhone's malloc climb, 983 to 1813 MiB in 15 min). They live in
// a file the kernel may evict and read back: a third round restores every frame from it, bit for bit. --store-full
// lets the file grow to 64 MiB only (RLIMIT_FSIZE): the writes past it fail, those copies stay in host memory, and
// every frame still comes back bit for bit. Patch 0024: in --store the file is written about only the copies that
// stay (the LZFSE repacks); each park's LZ4 copy is held in host memory until then, at most 128 MiB.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr size_t kTextures = 240;
constexpr uint64_t kMiB = 1024 * 1024;
constexpr uint64_t kHeadroom = 1792 * kMiB;
// --warning-band: room for the 2 GiB of frames and half a GiB more.
constexpr uint64_t kBandHeadroom = 2560 * kMiB;
// --in-use: the textures drawn in every frame once all are made, and for how many frames.
constexpr size_t kInUse = 128;
constexpr int kInUseFrames = 180;
// The emergency band of texture parking (mtl_texture_parking.mm), where --in-use keeps no packed copy, and what its
// restores may take below it: the 64 MiB of staging on their way, their textures' storage and the samples between.
constexpr uint64_t kEmergencyBelow = 384 * kMiB;
constexpr uint64_t kRestoringFloor = 256 * kMiB;
// --in-use samples the footprint after every draw in its first frames, which restore the working set.
constexpr int kRestoringFrames = 3;
// --store: what the footprint may hold above the frames still on the GPU (the readback and staging buffers ANGLE
// keeps, at most 64 MiB each, the screen, ASan's own: ~240 MiB on the Mac with or without the park file), and how
// much must be packed to tell.
constexpr uint64_t kStoreSlack = 320 * kMiB;
constexpr uint64_t kStoreFullFile = 64 * kMiB;
constexpr double kStoreAtLeastMiB = 256.0;

void *library = nullptr;
bool failed = false;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-texture-parking: %s\n", what.c_str());
    std::exit(1);
}

// A number of ANGLEMetalParkingLine, "<key>=<value>", or -1 when the line has no such key.
double field(const std::string &line, const std::string &key) {
    const size_t at = line.find(" " + key + "=");
    return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + key.size() + 2);
}

void check(bool condition, const std::string &what) {
    if (!condition) {
        std::fprintf(stderr, "angle-texture-parking: FAIL %s\n", what.c_str());
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

// What jetsam measures on iOS: the task's physical footprint, GPU memory included.
uint64_t footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        fail("task_info");
    }
    return info.phys_footprint;
}

// Picture-like texels for texture <index>: a smooth two-axis gradient, a few hard-edged shapes and two bits of seeded
// noise, opaque.
void fill(std::vector<uint32_t> &texels, int index) {
    uint32_t seed = 0x9e3779b9u * static_cast<uint32_t>(index + 1);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            seed = seed * 1664525u + 1013904223u;
            const uint32_t noise = seed >> 30;
            const bool shape = ((x + index * 37) / 160 + (y + index * 11) / 120) % 5 == 0;
            const uint32_t red = ((x * 255 / kWidth + index * 7) & 255) ^ (shape ? 0x60 : 0) ^ noise;
            const uint32_t green = ((y * 255 / kHeight + index * 3) & 255) ^ noise;
            const uint32_t blue = (((x + y) * 255 / (kWidth + kHeight) + index * 5) & 255) ^ (shape ? 0x30 : 0);
            const size_t at = static_cast<size_t>(y) * kWidth + static_cast<size_t>(x);
            texels[at] = red | green << 8 | blue << 16 | 0xff000000u;
        }
    }
}

// FNV-1a over the texels: equal sums for equal frames, so a parked texture must come back bit for bit.
uint64_t sum(const std::vector<uint32_t> &texels) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t texel : texels) {
        hash = (hash ^ texel) * 0x100000001b3ull;
    }
    return hash;
}

GLuint program_for_copy() {
    const char *vertex =
        "#version 300 es\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
        "  uv = corner;\n"
        "  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const char *fragment =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform highp sampler2D frame;\n"
        "in vec2 uv;\n"
        "out vec4 color;\n"
        "void main() { color = texelFetch(frame, ivec2(gl_FragCoord.xy), 0); }\n";
    auto compile = [](GLenum type, const char *source) {
        GLuint shader = GL(CreateShader)(type);
        GL(ShaderSource)(shader, 1, &source, nullptr);
        GL(CompileShader)(shader);
        GLint ok = 0;
        GL(GetShaderiv)(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            fail("shader does not compile");
        }
        return shader;
    };
    GLuint program = GL(CreateProgram)();
    GL(AttachShader)(program, compile(GL_VERTEX_SHADER, vertex));
    GL(AttachShader)(program, compile(GL_FRAGMENT_SHADER, fragment));
    GL(LinkProgram)(program);
    GLint ok = 0;
    GL(GetProgramiv)(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        fail("program does not link");
    }
    return program;
}

double percentile(std::vector<double> values, double share) {
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(share * static_cast<double>(values.size() - 1) + 0.5)];
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fail("usage: angle-texture-parking <libGLESv2.dylib> [--without-parking | --far-limit | --warning-band | "
             "--in-use | --store | --store-full | --warning-band-now]");
    }
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not built, see tools/build-angle.sh\n", argv[1]);
        return 0;
    }
    @autoreleasepool {
        // The limit is relative to what this process holds before ANGLE loads; ANGLE reads it at display start.
        std::vector<uint32_t> texels(static_cast<size_t>(kWidth) * kHeight);
        std::vector<uint32_t> readback(texels.size());
        std::fill(texels.begin(), texels.end(), 0u);
        std::fill(readback.begin(), readback.end(), 0u);
        // For comparison: --without-parking draws the same frames with no limit (no memory pressure at all),
        // --far-limit with a limit far away (the pressure check runs every frame, as on the device).
        const std::string mode = argc == 3 ? argv[2] : "";
        const bool far = mode == "--far-limit";
        const bool band = mode == "--warning-band";
        const bool in_use = mode == "--in-use";
        const bool store_full = mode == "--store-full";
        const bool store = mode == "--store" || store_full;
        // --warning-band-now (patch 0022): the limit far away, so nothing parks, until an iOS memory warning
        // (ANGLEMetalMemoryWarning) comes: within its second the idle frames park, as in the emergency band.
        const bool warning_now = mode == "--warning-band-now";
        if (store_full) {
            // A write past the limit fails with EFBIG instead of a signal.
            signal(SIGXFSZ, SIG_IGN);
            const struct rlimit file_size = {kStoreFullFile, kStoreFullFile};
            if (setrlimit(RLIMIT_FSIZE, &file_size) != 0) {
                fail("setrlimit RLIMIT_FSIZE");
            }
        }
        const uint64_t limit = mode == "--without-parking" || far || warning_now ? (uint64_t{1} << 20) * kMiB
                               : band                             ? footprint() + 128 * kMiB + kBandHeadroom
                               : in_use                           ? footprint() + 192 * kMiB + kHeadroom
                                                                  : footprint() + 128 * kMiB + kHeadroom;
        if (mode == "--without-parking") {
            unsetenv("MINATO_ANGLE_PARK_LIMIT_MB");
        } else {
            setenv("MINATO_ANGLE_PARK_LIMIT_MB", std::to_string(limit / kMiB).c_str(), 1);
        }

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
        const EGLint spec[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        if (EGL(ChooseConfig)(display, spec, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            fail("no ES 3 window config");
        }
        // A small window, whose swaps are the app's presents; the frames are drawn into the screen framebuffer.
        CAMetalLayer *layer = [CAMetalLayer layer];
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contentsScale = 1.0;
        layer.bounds = CGRectMake(0, 0, 16, 16);
        [CATransaction commit];
        const EGLint size[] = {EGL_FIXED_SIZE_ANGLE, EGL_TRUE, EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLSurface surface = EGL(CreateWindowSurface)(
            display, config, reinterpret_cast<EGLNativeWindowType>((__bridge void *)layer), size);
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, version);
        if (EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
            fail("no current context");
        }

        // The screen: a 1920 x 1080 texture behind a framebuffer, which is never parked.
        GLuint screen = 0;
        GLuint framebuffer = 0;
        GL(GenTextures)(1, &screen);
        GL(BindTexture)(GL_TEXTURE_2D, screen);
        GL(TexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        GL(GenFramebuffers)(1, &framebuffer);
        GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
        GL(FramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, screen, 0);
        if (GL(CheckFramebufferStatus)(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fail("the screen framebuffer is incomplete");
        }
        GL(Viewport)(0, 0, kWidth, kHeight);
        GL(UseProgram)(program_for_copy());
        GLuint vertex_array = 0;
        GL(GenVertexArrays)(1, &vertex_array);
        GL(BindVertexArray)(vertex_array);
        GL(PixelStorei)(GL_UNPACK_ALIGNMENT, 4);
        GL(PixelStorei)(GL_PACK_ALIGNMENT, 4);

        std::array<GLuint, kTextures> frames{};
        std::array<uint64_t, kTextures> sums{};
        GL(GenTextures)(kTextures, frames.data());
        // --store: what the process holds before the frames.
        const uint64_t before_frames = footprint();
        uint64_t peak = 0;
        int mismatches = 0;
        std::vector<double> frame_ms;
        // The intervals between the frames' swaps, as the test sees them, for the line's present_p50.
        std::vector<double> swap_ms;
        std::chrono::steady_clock::time_point last_swap;
        // One frame per 1/60 s, as an app would present; the made frames grow the footprint by ~475 MiB/s.
        auto next = std::chrono::steady_clock::now();
        for (int round = 0; round < (in_use ? 1 : 2); ++round) {
            for (size_t index = 0; index < frames.size(); ++index) {
                if (round == 0) {
                    fill(texels, static_cast<int>(index));
                    sums[index] = sum(texels);
                }
                const auto start = std::chrono::steady_clock::now();
                GL(BindTexture)(GL_TEXTURE_2D, frames[index]);
                if (round == 0) {
                    GL(TexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                   texels.data());
                    GL(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    GL(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                }
                GL(DrawArrays)(GL_TRIANGLE_STRIP, 0, 4);
                GL(ReadPixels)(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
                // The end of the frame: the window gets drawn and swapped, as an app presents. ANGLE presents
                // only a window that was drawn into since its last swap.
                GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
                GL(Clear)(GL_COLOR_BUFFER_BIT);
                EGL(SwapBuffers)(display, surface);
                const auto swapped = std::chrono::steady_clock::now();
                if (round != 0 || index != 0) {
                    swap_ms.push_back(std::chrono::duration<double, std::milli>(swapped - last_swap).count());
                }
                last_swap = swapped;
                GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
                const double milliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                frame_ms.push_back(milliseconds);
                if (sum(readback) != sums[index]) {
                    ++mismatches;
                }
                peak = std::max(peak, footprint());
                next += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(next);
            }
        }
        // --in-use: first empty frames until the background repacks are done (the line's lzfse reaches the parked
        // textures, for at most 60 s), so that the working set comes back at the same memory every run; then the
        // first kInUse frames in every frame; the last frame reads each one back.
        auto parking_line = [] {
            std::array<char, 512> line{};
            void *address = dlsym(library, "ANGLEMetalParkingLine");
            if (address != nullptr) {
                reinterpret_cast<int (*)(char *, size_t)>(address)(line.data(), line.size());
            }
            return std::string(line.data());
        };
        const auto repacks_until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while ((in_use || store) && std::chrono::steady_clock::now() < repacks_until) {
            const std::string line = parking_line();
            // --store: until every frame is parked again (the second round restored them all) and nothing is on
            // its way.
            if (field(line, "lzfse") >= field(line, "n") &&
                (!store || (field(line, "n") >= static_cast<double>(kTextures) && field(line, "pending") == 0.0))) {
                break;
            }
            GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            EGL(SwapBuffers)(display, surface);
            GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        next = std::chrono::steady_clock::now();
        uint64_t in_use_peak = 0;
        for (int frame = 0; in_use && frame < kInUseFrames; ++frame) {
            for (size_t index = 0; index < kInUse; ++index) {
                GL(BindTexture)(GL_TEXTURE_2D, frames[index]);
                GL(DrawArrays)(GL_TRIANGLE_STRIP, 0, 4);
                if (frame == kInUseFrames - 1) {
                    GL(ReadPixels)(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
                    if (sum(readback) != sums[index]) {
                        ++mismatches;
                    }
                }
                if (frame < kRestoringFrames) {
                    in_use_peak = std::max(in_use_peak, footprint());
                }
            }
            GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            EGL(SwapBuffers)(display, surface);
            GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
            in_use_peak = std::max(in_use_peak, footprint());
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);
        }
        peak = std::max(peak, in_use_peak);
        // --store: the footprint once all is parked, then a third round restores every frame.
        std::string store_line;
        uint64_t store_footprint = 0;
        if (store) {
            store_line = parking_line();
            store_footprint = footprint();
            for (size_t index = 0; index < frames.size(); ++index) {
                GL(BindTexture)(GL_TEXTURE_2D, frames[index]);
                GL(DrawArrays)(GL_TRIANGLE_STRIP, 0, 4);
                GL(ReadPixels)(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
                if (sum(readback) != sums[index]) {
                    ++mismatches;
                }
                GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
                GL(Clear)(GL_COLOR_BUFFER_BIT);
                EGL(SwapBuffers)(display, surface);
                GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
                peak = std::max(peak, footprint());
            }
        }
        check(GL(GetError)() == GL_NO_ERROR, "no GL error");

        std::string parking;
        for (const char *name : {"ANGLEMetalMemoryLine", "ANGLEMetalParkingLine"}) {
            void *line_address = dlsym(library, name);
            if (line_address != nullptr) {
                std::array<char, 512> line{};
                reinterpret_cast<int (*)(char *, size_t)>(line_address)(line.data(), line.size());
                std::printf("%s\n", line.data());
                parking = line.data();
            }
        }
        std::printf("limit %llu MiB, peak %llu MiB, frame p50 %.1f ms p99 %.1f ms max %.1f ms, %d mismatches\n",
                    static_cast<unsigned long long>(limit / kMiB), static_cast<unsigned long long>(peak / kMiB),
                    percentile(frame_ms, 0.5), percentile(frame_ms, 0.99),
                    *std::max_element(frame_ms.begin(), frame_ms.end()), mismatches);
        std::vector<size_t> order(frame_ms.size());
        for (size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        std::partial_sort(order.begin(), order.begin() + 3, order.end(),
                          [&frame_ms](size_t a, size_t b) { return frame_ms[a] > frame_ms[b]; });
        for (size_t rank = 0; rank < 3; ++rank) {
            std::printf("slow frame: round %zu texture %zu %.1f ms\n", order[rank] / kTextures + 1,
                        order[rank] % kTextures, frame_ms[order[rank]]);
        }
        check(mismatches == 0, "every frame reads back the texels it was made from");
        check(peak <= limit, "the footprint stays below MINATO_ANGLE_PARK_LIMIT_MB");
        // A stall in every frame shows in the median; a long one in p99. Both bounds leave room for a loaded Mac.
        check(percentile(frame_ms, 0.5) < 25.0, "frame p50 below 25 ms");
        check(percentile(frame_ms, 0.99) < 250.0, "frame p99 below 250 ms");
        if (in_use) {
            std::printf("in use: peak %llu MiB of %llu MiB, %.0f MiB available at the end\n",
                        static_cast<unsigned long long>(in_use_peak / kMiB),
                        static_cast<unsigned long long>(limit / kMiB), field(parking, "avail_last"));
            check(in_use_peak + kRestoringFloor <= limit,
                  "while the working set is restored at least " + std::to_string(kRestoringFloor / kMiB) +
                      " MiB stay available (" + std::to_string((limit - std::min(in_use_peak, limit)) / kMiB) + ")");
            const double last = field(parking, "avail_last");
            const double kept = field(parking, "kept");
            check(kept == 0.0 || last >= static_cast<double>(kEmergencyBelow / kMiB),
                  "no packed copy is kept in the emergency band (kept " + std::to_string(kept) + " MiB at " +
                      std::to_string(last) + " MiB available)");
        }
        if (warning_now) {
            void *warn = dlsym(library, "ANGLEMetalMemoryWarning");
            if (warn == nullptr) {
                fail("no ANGLEMetalMemoryWarning (patch 0022)");
            }
            auto empty_frame = [&] {
                GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
                GL(Clear)(GL_COLOR_BUFFER_BIT);
                EGL(SwapBuffers)(display, surface);
                GL(BindFramebuffer)(GL_FRAMEBUFFER, framebuffer);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            };
            for (int frame = 0; frame < 10; ++frame) {
                empty_frame();  // the frames idle past the emergency band's 33 ms
            }
            const std::string before = parking_line();
            reinterpret_cast<void (*)()>(warn)();
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < until) {
                empty_frame();
            }
            const std::string after = parking_line();
            std::printf("warning: before %s\nwarning: after  %s\n", before.c_str(), after.c_str());
            check(field(before, "parks") == 0.0, "nothing parks with the limit far away before the warning");
            check(field(after, "warn") == 1.0, "the line counts the warning (warn=1)");
            check(field(after, "parks") > 0.0 && field(after, "parks") + field(after, "pending") > 0.0,
                  "the idle frames park within the warning's second (parks " + std::to_string(field(after, "parks")) +
                      ")");
        }
        if (mode.empty() || band) {
            // Patch 0018: the line's least available memory is what the footprint left (give or take a sample
            // between frame ends), and the presents' interval is the one between the frames' swaps as the test
            // measures them: 1/60 s, longer on a loaded Mac, where a frame may take more than its 1/60 s.
            const double least = field(parking, "avail_min");
            const double left = static_cast<double>(limit - std::min(peak, limit)) / kMiB;
            const double present = field(parking, "present_p50");
            check(least >= 0.0 && least <= left + 64.0,
                  "avail_min " + std::to_string(least) + " MiB is what the peak left, " + std::to_string(left));
            check(field(parking, "avail_last") >= least, "the last available memory is not below the least");
            const double swaps = percentile(swap_ms, 0.5);
            check(std::abs(present - swaps) <= std::max(5.0, swaps / 4),
                  "the presents' interval p50 is the swaps' (present_p50 " + std::to_string(present) + " ms, swaps " +
                      std::to_string(swaps) + " ms)");
            check(field(parking, "present_p99") >= present, "the presents' interval p99 is not below its p50");
        }
        if (store) {
            // The frames still on the GPU are all frames' bytes minus the parked ones' (raw).
            const double frames_mib = static_cast<double>(kTextures) * kWidth * kHeight * 4 / kMiB;
            const double on_gpu = frames_mib - field(store_line, "raw");
            const double held = static_cast<double>(store_footprint - std::min(store_footprint, before_frames)) / kMiB;
            const double packed = field(store_line, "packed") + field(store_line, "kept");
            std::printf("store: %s\nstore: %.0f MiB above the start with %.0f MiB of frames on the GPU and %.0f MiB "
                        "packed\n",
                        store_line.c_str(), held, on_gpu, packed);
            check(packed >= kStoreAtLeastMiB,
                  "at least " + std::to_string(kStoreAtLeastMiB) + " MiB packed to measure (" + std::to_string(packed) +
                      ")");
            if (store_full) {
                check(field(store_line, "ffail") > 0.0, "the full park file refused copies (ffail)");
                check(field(store_line, "file") <= static_cast<double>(kStoreFullFile / kMiB),
                      "the park file stays within its limit");
            }
            if (!store_full) {
                // Patch 0024: the file gets the copies that stay (the LZFSE repacks), not each park's LZ4 copy a
                // second before (0021 wrote 1676 MiB for 465 MiB here); the LZ4 copies held meanwhile stay under the
                // 128 MiB cap. On a loaded Mac the LZFSE repacks (QOS_CLASS_UTILITY) fall behind, so still-LZ4 copies
                // are spilled to the file to honour that cap and are rewritten as LZFSE later: the stayed set is then
                // written up to about twice over (measured 1.76x under load, 2.13x for the band run's larger set,
                // 1.02x idle). The bound leaves room for that, as the frame-time bounds do, and still catches 0021's
                // write-everything-twice (3.6x); the real guarantee is the footprint bound.
                check(field(store_line, "fwrite") <= field(store_line, "file") * 2.5,
                      "the park file is written the copies that stay (plus a loaded Mac's LZ4 spill), not each LZ4 "
                      "copy too (fwrite " +
                          std::to_string(field(store_line, "fwrite")) + " MiB for " +
                          std::to_string(field(store_line, "file")) + " MiB)");
                check(field(store_line, "hpeak") >= 0.0 && field(store_line, "hpeak") <= 128.0,
                      "the LZ4 copies held until their repack stay under 128 MiB (hpeak " +
                          std::to_string(field(store_line, "hpeak")) + ")");
            }
            check(store_full || held <= on_gpu + static_cast<double>(kStoreSlack / kMiB),
                  "the packed copies are not in the footprint (" + std::to_string(held) + " MiB held, " +
                      std::to_string(on_gpu) + " MiB of frames on the GPU, " + std::to_string(packed) + " MiB packed)");
        }
        if (band) {
            // A measurement, not a bound: how much the parks leave at the worst, which depends on how fast the
            // background packs keep up (on the Mac 0017 left 620-645 MiB, 0018 722-1023 MiB).
            std::printf("warning band: at least %llu MiB stayed available\n",
                        static_cast<unsigned long long>((limit - std::min(peak, limit)) / kMiB));
        }

        GL(DeleteTextures)(kTextures, frames.data());
        GL(BindFramebuffer)(GL_FRAMEBUFFER, 0);
        GL(DeleteFramebuffers)(1, &framebuffer);
        GL(DeleteTextures)(1, &screen);
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroyContext)(display, context);
        EGL(DestroySurface)(display, surface);
        EGL(Terminate)(display);
    }
    if (failed) {
        return 1;
    }
    std::printf("angle-texture-parking: ok\n");
    return 0;
}
