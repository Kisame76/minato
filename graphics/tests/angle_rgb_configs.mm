// ADR-0043 c4: RGB888 configs without alpha on the Metal backend (patch 0006), which GLSurfaceView's default chooser
// takes (ComponentSizeChooser with 8, 8, 8, 0 and a depth of 16 or more). Android 15's AVD lists such a config first,
// with a buffer size of 24, and its surfaces have no alpha: GL_ALPHA_BITS is 0, alpha reads 255 after a clear or a
// draw with less, and GL_DST_ALPHA blends with 1. Every RGBA8 config has such a twin, of the same depth, stencil and
// samples, and an RGBA8 surface keeps its alpha as before. The CAMetalLayers lie in no window; their changes go
// through explicit transactions, since this thread has no run loop.
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

void *library = nullptr;
EGLDisplay display = EGL_NO_DISPLAY;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-rgb-configs: %s\n", what.c_str());
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

EGLint attribute(EGLConfig config, EGLint name) {
    EGLint value = -1;
    if (EGL(GetConfigAttrib)(display, config, name, &value) != EGL_TRUE) {
        fail("eglGetConfigAttrib failed");
    }
    return value;
}

std::string sizes(EGLConfig config) {
    return "rgba " + std::to_string(attribute(config, EGL_RED_SIZE)) + " " +
           std::to_string(attribute(config, EGL_GREEN_SIZE)) + " " + std::to_string(attribute(config, EGL_BLUE_SIZE)) +
           " " + std::to_string(attribute(config, EGL_ALPHA_SIZE)) + ", buffer " +
           std::to_string(attribute(config, EGL_BUFFER_SIZE)) + ", depth " +
           std::to_string(attribute(config, EGL_DEPTH_SIZE)) + ", stencil " +
           std::to_string(attribute(config, EGL_STENCIL_SIZE)) + ", samples " +
           std::to_string(attribute(config, EGL_SAMPLES));
}

// Each config's depth, stencil and samples, of the configs with alpha `alpha` and RGB 888.
std::multiset<std::tuple<EGLint, EGLint, EGLint>> kinds(EGLint alpha) {
    EGLint count = 0;
    EGL(GetConfigs)(display, nullptr, 0, &count);
    std::vector<EGLConfig> configs(static_cast<size_t>(count));
    EGL(GetConfigs)(display, configs.data(), count, &count);
    std::multiset<std::tuple<EGLint, EGLint, EGLint>> found;
    for (EGLConfig config : configs) {
        if (attribute(config, EGL_RED_SIZE) == 8 && attribute(config, EGL_GREEN_SIZE) == 8 &&
            attribute(config, EGL_BLUE_SIZE) == 8 && attribute(config, EGL_ALPHA_SIZE) == alpha) {
            found.insert({attribute(config, EGL_DEPTH_SIZE), attribute(config, EGL_STENCIL_SIZE),
                          attribute(config, EGL_SAMPLES)});
        }
    }
    return found;
}

// GLSurfaceView's ComponentSizeChooser: the configs eglChooseConfig gives for the sizes and ES 3, then the first with
// exactly these red, green, blue and alpha sizes and at least the depth and stencil. Its place goes to `place`.
EGLConfig choose(EGLint red, EGLint green, EGLint blue, EGLint alpha, EGLint depth, EGLint stencil, int *place) {
    const EGLint spec[] = {EGL_RED_SIZE,   red,   EGL_GREEN_SIZE, green, EGL_BLUE_SIZE,       blue,
                           EGL_ALPHA_SIZE, alpha, EGL_DEPTH_SIZE, depth, EGL_STENCIL_SIZE,    stencil,
                           EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint count = 0;
    if (EGL(ChooseConfig)(display, spec, nullptr, 0, &count) != EGL_TRUE) {
        fail("eglChooseConfig failed");
    }
    std::vector<EGLConfig> configs(static_cast<size_t>(count));
    EGL(ChooseConfig)(display, spec, configs.data(), count, &count);
    for (int index = 0; index < count; index++) {
        EGLConfig config = configs[static_cast<size_t>(index)];
        if (attribute(config, EGL_DEPTH_SIZE) >= depth && attribute(config, EGL_STENCIL_SIZE) >= stencil &&
            attribute(config, EGL_RED_SIZE) == red && attribute(config, EGL_GREEN_SIZE) == green &&
            attribute(config, EGL_BLUE_SIZE) == blue && attribute(config, EGL_ALPHA_SIZE) == alpha) {
            *place = index;
            return config;
        }
    }
    fail("no config for " + std::to_string(red) + ", " + std::to_string(green) + ", " + std::to_string(blue) + ", " +
         std::to_string(alpha) + " with a depth of " + std::to_string(depth) + " or more among " +
         std::to_string(count) + ": GLSurfaceView's \"No config chosen\"");
}

CAMetalLayer *small_layer() {
    CAMetalLayer *layer = [CAMetalLayer layer];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contentsScale = 1.0;
    layer.bounds = CGRectMake(0, 0, 8, 8);
    [CATransaction commit];
    return layer;
}

EGLContext context_on(EGLConfig config, EGLSurface surface, const char *what) {
    if (surface == EGL_NO_SURFACE) {
        char error[16];
        std::snprintf(error, sizeof(error), "0x%04X", static_cast<unsigned>(EGL(GetError)()));
        fail(std::string("no ") + what + " surface, error " + error);
    }
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, attributes);
    if (context == EGL_NO_CONTEXT || EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
        fail(std::string("no ES 3.0 context on the ") + what + " surface");
    }
    GL(Viewport)(0, 0, 8, 8);
    return context;
}

void release(EGLSurface surface, EGLContext context) {
    EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL(DestroyContext)(display, context);
    EGL(DestroySurface)(display, surface);
}

void expect_alpha_bits(const char *what, GLint expected) {
    GLint bits = -1;
    GL(GetIntegerv)(GL_ALPHA_BITS, &bits);
    std::printf("%s: GL_ALPHA_BITS %d\n", what, bits);
    if (bits != expected) {
        fail(std::string(what) + ": expected GL_ALPHA_BITS " + std::to_string(expected));
    }
}

void expect_pixel(const char *what, int red, int green, int blue, int alpha) {
    unsigned char pixel[4] = {};
    GL(ReadPixels)(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    std::printf("%s: reads %d %d %d %d\n", what, pixel[0], pixel[1], pixel[2], pixel[3]);
    if (pixel[0] != red || pixel[1] != green || pixel[2] != blue || pixel[3] != alpha) {
        fail(std::string(what) + ": expected " + std::to_string(red) + " " + std::to_string(green) + " " +
             std::to_string(blue) + " " + std::to_string(alpha));
    }
}

// A channel's value of 0 to 255 as GL's 0 to 1.
float unit(int value) {
    return static_cast<float>(value) / 255.0f;
}

void clear(int red, int green, int blue, int alpha) {
    GL(ClearColor)(unit(red), unit(green), unit(blue), unit(alpha));
    GL(Clear)(GL_COLOR_BUFFER_BIT);
}

GLuint shader(GLenum type, const char *source) {
    GLuint id = GL(CreateShader)(type);
    GL(ShaderSource)(id, 1, &source, nullptr);
    GL(CompileShader)(id);
    GLint compiled = GL_FALSE;
    GL(GetShaderiv)(id, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        fail("a shader does not compile");
    }
    return id;
}

// A triangle over the whole viewport in one color.
GLuint color_program() {
    static const char *const vertex =
        "#version 300 es\n"
        "void main() {\n"
        "    vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));\n"
        "    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);\n"
        "}\n";
    static const char *const fragment =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform vec4 color;\n"
        "out vec4 result;\n"
        "void main() {\n"
        "    result = color;\n"
        "}\n";
    GLuint program = GL(CreateProgram)();
    GL(AttachShader)(program, shader(GL_VERTEX_SHADER, vertex));
    GL(AttachShader)(program, shader(GL_FRAGMENT_SHADER, fragment));
    GL(LinkProgram)(program);
    GLint linked = GL_FALSE;
    GL(GetProgramiv)(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        fail("the program does not link");
    }
    return program;
}

void draw(GLuint program, int red, int green, int blue, int alpha) {
    GL(UseProgram)(program);
    GL(Uniform4f)(GL(GetUniformLocation)(program, "color"), unit(red), unit(green), unit(blue), unit(alpha));
    GL(DrawArrays)(GL_TRIANGLES, 0, 3);
}

// What an RGB config's surface answers: no alpha bits, and alpha 255 after a clear with 0, a draw with 128, and a
// blend with the destination's alpha, which is 1.
void without_alpha(const char *what) {
    expect_alpha_bits(what, 0);
    clear(64, 128, 192, 0);
    expect_pixel((std::string(what) + ", cleared with alpha 0").c_str(), 64, 128, 192, 255);
    GLuint program = color_program();
    draw(program, 128, 64, 192, 128);
    expect_pixel((std::string(what) + ", drawn with alpha 128").c_str(), 128, 64, 192, 255);
    clear(0, 0, 0, 0);
    GL(Enable)(GL_BLEND);
    GL(BlendFunc)(GL_DST_ALPHA, GL_ZERO);
    draw(program, 128, 64, 192, 255);
    GL(Disable)(GL_BLEND);
    expect_pixel((std::string(what) + ", blended with GL_DST_ALPHA").c_str(), 128, 64, 192, 255);
    GL(DeleteProgram)(program);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-rgb-configs <libGLESv2.dylib>");
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
        display = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, display_attributes);
        if (display == EGL_NO_DISPLAY || EGL(Initialize)(display, nullptr, nullptr) != EGL_TRUE) {
            fail("no Metal display");
        }

        // GLSurfaceView's default: 8, 8, 8, 0 with a depth of 16 or more, which the AVD finds first.
        int place = -1;
        EGLConfig rgb = choose(8, 8, 8, 0, 16, 0, &place);
        std::printf("GLSurfaceView's default chooser: config %d of the list, %s\n", place, sizes(rgb).c_str());
        if (place != 0 || attribute(rgb, EGL_BUFFER_SIZE) != 24) {
            fail("expected the first config of the list, with a buffer size of 24, as on Android 15's AVD");
        }
        const auto twins = kinds(0);
        const auto rgba = kinds(8);
        std::printf("RGB888 configs without alpha: %zu, RGBA8888 configs: %zu\n", twins.size(), rgba.size());
        if (twins != rgba) {
            fail("expected an RGB888 twin of every RGBA8888 config, of the same depth, stencil and samples");
        }

        CAMetalLayer *layer = small_layer();
        EGLSurface window = EGL(CreateWindowSurface)(
            display, rgb, reinterpret_cast<EGLNativeWindowType>((__bridge void *)layer), nullptr);
        EGLContext context = context_on(rgb, window, "RGB888 window");
        without_alpha("RGB888 window");
        if (EGL(SwapBuffers)(display, window) != EGL_TRUE) {
            fail("eglSwapBuffers on the RGB888 window failed");
        }
        std::printf("RGB888 window: the layer's pixel format %s\n",
                    layer.pixelFormat == MTLPixelFormatBGRA8Unorm ? "BGRA8Unorm" : "another");
        release(window, context);

        const EGLint pbuffer_attributes[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        EGLSurface pbuffer = EGL(CreatePbufferSurface)(display, rgb, pbuffer_attributes);
        context = context_on(rgb, pbuffer, "RGB888 pbuffer");
        without_alpha("RGB888 pbuffer");
        release(pbuffer, context);

        // An RGBA8888 window keeps its alpha.
        EGLConfig with_alpha = choose(8, 8, 8, 8, 0, 0, &place);
        CAMetalLayer *other = small_layer();
        window = EGL(CreateWindowSurface)(display, with_alpha,
                                          reinterpret_cast<EGLNativeWindowType>((__bridge void *)other), nullptr);
        context = context_on(with_alpha, window, "RGBA8888 window");
        expect_alpha_bits("RGBA8888 window", 8);
        clear(64, 128, 192, 0);
        expect_pixel("RGBA8888 window, cleared with alpha 0", 64, 128, 192, 0);
        release(window, context);
        EGL(Terminate)(display);
    }
    return 0;
}
