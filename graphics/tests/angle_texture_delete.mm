// Ren'Py's full display reset at a return, on ANGLE's macOS slice: does ANGLE let go of the textures an app deletes?
// Ren'Py 8.5.3 keeps its textures in its TextureLoader and, at the full reset of each return (gl2draw.on_resize ->
// TextureLoader.init -> quit), deletes every one of them with glDeleteTextures, then loads its scene again. On the
// Simulator ANGLE's color= count grew by about one such set per return. Here, as SDL 2 and Ren'Py do it on Android: a
// window surface on a CAMetalLayer and a context; N textures (RGBA8, uploaded with glTexImage2D and mipmapped, as
// Ren'Py's loader does) drawn into frames that are swapped; then per return SDL's backup (no context current), the
// window surface destroyed, a new one on a new layer, SDL's restore (the same context made current with it), Ren'Py's
// reset (every texture deleted) and its reload (N new textures, drawn and swapped). After each return, once Metal is
// done, ANGLE's gl line must count the start's color textures again: the window's own and the N live ones, not the
// deleted sets on top. Mode upload: each texture uploaded; mode copy: made as Ren'Py's load_gltexture makes it.
// Usage: angle-texture-delete <libGLESv2.dylib> upload|copy [N (600)] [returns (3)]
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-texture-delete: %s\n", what.c_str());
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

int (*line_of)(char *, size_t) = nullptr;

// The gl line's color= count, or -1 without one.
long colors() {
    std::array<char, 1024> line{};
    line_of(line.data(), line.size());
    const char *at = std::strstr(line.data(), " color=");
    return at != nullptr ? std::strtol(at + 7, nullptr, 10) : -1;
}

// The color count once Metal is done with what was let go: the lowest of 2 s of looks, early once it reaches `want`.
long settled(long want) {
    GL(Finish)();
    long lowest = colors();
    for (int look = 0; look < 40 && lowest > want; ++look) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        lowest = std::min(lowest, colors());
    }
    return lowest;
}

CAMetalLayer *layer() {
    CAMetalLayer *made = [CAMetalLayer layer];
    made.frame = CGRectMake(0, 0, 256, 256);
    made.drawableSize = CGSizeMake(256, 256);
    return made;
}

bool via_copy = false;

// Ren'Py's loader: N RGBA8 textures of 128 x 128 with data, mipmapped. With `via_copy` (mode copy), as Ren'Py's
// load_gltexture does it: the data into a texture of its own, the premultiplied texture made from the framebuffer
// with glCopyTexImage2D, and the first texture deleted at once, one glDeleteTextures each.
std::vector<GLuint> load_set(int count) {
    static std::vector<unsigned char> pixels(128 * 128 * 4, 0x7f);
    std::vector<GLuint> textures(static_cast<size_t>(count));
    GL(GenTextures)(count, textures.data());
    for (GLuint texture : textures) {
        GLuint upload = texture;
        if (via_copy) {
            GL(GenTextures)(1, &upload);
        }
        GL(BindTexture)(GL_TEXTURE_2D, upload);
        GL(TexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (via_copy) {
            GL(BindTexture)(GL_TEXTURE_2D, texture);
            GL(CopyTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 128, 128, 0);
            GL(DeleteTextures)(1, &upload);
        }
        GL(GenerateMipmap)(GL_TEXTURE_2D);
    }
    return textures;
}

// A frame that samples every texture of the set: a framebuffer per texture would make more color textures, so each
// texture is the source of a copy into the window instead (glCopyTexSubImage2D reads the window into it).
void frames(EGLDisplay display, EGLSurface window, const std::vector<GLuint> &textures) {
    for (int frame = 0; frame < 3; ++frame) {
        GL(ClearColor)(0, static_cast<float>(frame) / 3.0f, 0, 1);
        GL(Clear)(GL_COLOR_BUFFER_BIT);
        for (GLuint texture : textures) {
            GL(BindTexture)(GL_TEXTURE_2D, texture);
            GL(CopyTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 4, 4);
        }
        EGL(SwapBuffers)(display, window);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5 || (std::strcmp(argv[2], "upload") != 0 && std::strcmp(argv[2], "copy") != 0)) {
        fail("usage: angle-texture-delete <libGLESv2.dylib> upload|copy [N] [returns]");
    }
    via_copy = std::strcmp(argv[2], "copy") == 0;
    const int count = argc > 3 ? std::atoi(argv[3]) : 600;
    const int returns = argc > 4 ? std::atoi(argv[4]) : 3;
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not built, see tools/build-angle.sh\n", argv[1]);
        return 0;
    }
    @autoreleasepool {
        library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            fail(std::string("dlopen: ") + dlerror());
        }
        line_of = load<int (*)(char *, size_t)>("ANGLEMetalMemoryLine");
        const EGLAttrib display_attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                                                EGL_NONE};
        EGLDisplay display = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, display_attributes);
        if (display == EGL_NO_DISPLAY || EGL(Initialize)(display, nullptr, nullptr) != EGL_TRUE) {
            fail("no Metal display");
        }
        const EGLint spec[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                               EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                               EGL_RED_SIZE,        8,
                               EGL_GREEN_SIZE,      8,
                               EGL_BLUE_SIZE,       8,
                               EGL_ALPHA_SIZE,      8,
                               EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        if (EGL(ChooseConfig)(display, spec, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            fail("no ES 2 window config");
        }
        // SDL 2 asks for ES 2.0 for Ren'Py.
        const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        CAMetalLayer *first = layer();
        EGLSurface window = EGL(CreateWindowSurface)(display, config, (__bridge EGLNativeWindowType)first, nullptr);
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, version);
        if (window == EGL_NO_SURFACE || EGL(MakeCurrent)(display, window, window, context) != EGL_TRUE) {
            fail("no current context on a window surface");
        }
        frames(display, window, {});
        const long empty = settled(0);
        std::vector<GLuint> textures = load_set(count);
        frames(display, window, textures);
        const long start = settled(empty + count);
        std::printf("start: color=%ld with the window alone, %ld with %d textures\n", empty, start, count);
        if (empty < 0 || start < 0) {
            fail("no color= in ANGLE's gl line");
        }
        bool grew = false;
        for (int trip = 1; trip <= returns; ++trip) {
            // The pause: SDL's backup, then the window surface destroyed (onNativeSurfaceDestroyed).
            EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            EGL(DestroySurface)(display, window);
            // The return: a new surface on a new layer, SDL's restore with the same context.
            CAMetalLayer *next = layer();
            window = EGL(CreateWindowSurface)(display, config, (__bridge EGLNativeWindowType)next, nullptr);
            if (window == EGL_NO_SURFACE || EGL(MakeCurrent)(display, window, window, context) != EGL_TRUE) {
                fail("no current context after the return");
            }
            // Ren'Py's full reset: every texture of its loader deleted, one call each; then its scene loaded again.
            for (GLuint texture : textures) {
                GL(DeleteTextures)(1, &texture);
            }
            frames(display, window, {});
            const long deleted = settled(empty);
            textures = load_set(count);
            frames(display, window, textures);
            const long reloaded = settled(empty + count);
            std::printf("return %d: color=%ld after the reset deleted the %d textures, %ld after the reload\n", trip,
                        deleted, count, reloaded);
            if (deleted > empty + 2 || reloaded > start + 2) {
                grew = true;
            }
        }
        if (GL(GetError)() != GL_NO_ERROR) {
            fail("a GL error");
        }
        GL(DeleteTextures)(static_cast<GLsizei>(textures.size()), textures.data());
        EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL(DestroySurface)(display, window);
        EGL(DestroyContext)(display, context);
        EGL(Terminate)(display);
        if (grew) {
            fail("deleted textures stayed in ANGLE's color count: a return's set stays alive after glDeleteTextures");
        }
        std::printf("ok: after %d returns ANGLE counts the start's color textures, the deleted sets are gone\n",
                    returns);
    }
    return 0;
}
