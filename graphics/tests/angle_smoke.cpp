// Test 1 of ADR-0029: the macOS slice of ANGLE's libGLESv2, opened with dlopen as the host side will
// open it, draws with OpenGL ES 3.0 on the Metal backend into a 64 x 64 pbuffer and reads it back.
// No guest code and no window. libGLESv2 exports EGL as EGL_* and GLES under its own names.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void *library = nullptr;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "angle-smoke: %s\n", what.c_str());
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

GLuint compile(GLenum type, const char *source) {
    GLuint shader = GL(CreateShader)(type);
    GL(ShaderSource)(shader, 1, &source, nullptr);
    GL(CompileShader)(shader);
    GLint compiled = GL_FALSE;
    GL(GetShaderiv)(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[1024] = {};
        GL(GetShaderInfoLog)(shader, sizeof(log), nullptr, log);
        fail(std::string("shader does not compile: ") + log);
    }
    return shader;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: angle-smoke <libGLESv2.dylib>");
    }
    // Other work in the tree must not fail while ANGLE is being built (CTest: SKIP_REGULAR_EXPRESSION).
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not built, see tools/build-angle.sh\n", argv[1]);
        return 0;
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
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                        EGL_RED_SIZE,     8,               EGL_GREEN_SIZE,      8,
                                        EGL_BLUE_SIZE,    8,               EGL_ALPHA_SIZE,      8,
                                        EGL_NONE};
    EGLConfig config = nullptr;
    EGLint configs = 0;
    if (EGL(ChooseConfig)(display, config_attributes, &config, 1, &configs) != EGL_TRUE || configs < 1) {
        fail("no RGBA8888 pbuffer config for ES 3");
    }
    const EGLint surface_attributes[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = EGL(CreatePbufferSurface)(display, config, surface_attributes);
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
        fail("no ES 3.0 context on a 64 x 64 pbuffer");
    }

    GLint major = 0;
    GLint minor = 0;
    GL(GetIntegerv)(GL_MAJOR_VERSION, &major);
    GL(GetIntegerv)(GL_MINOR_VERSION, &minor);
    if (major != 3 || minor != 0) {
        fail("GL_MAJOR_VERSION " + std::to_string(major) + ", GL_MINOR_VERSION " + std::to_string(minor) +
             "; expected 3.0");
    }

    GLuint program = GL(CreateProgram)();
    GL(AttachShader)(program, compile(GL_VERTEX_SHADER,
                                      "#version 300 es\n"
                                      "layout(location = 0) in vec2 position;\n"
                                      "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n"));
    GL(AttachShader)(program, compile(GL_FRAGMENT_SHADER,
                                      "#version 300 es\n"
                                      "precision mediump float;\n"
                                      "out vec4 color;\n"
                                      "void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
    GL(LinkProgram)(program);
    GLint linked = GL_FALSE;
    GL(GetProgramiv)(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        fail("program does not link");
    }

    // The left half red on blue. Its right edge lies on the boundary between columns 31 and 32, so
    // every conformant GPU gives the same pixels (ES 3.0.6, 3.6.1); 0 and 1 convert exactly (2.1.6.2).
    const GLfloat corners[] = {-1.0f, -1.0f, 0.0f, -1.0f, -1.0f, 1.0f, 0.0f, 1.0f};
    GL(Viewport)(0, 0, 64, 64);
    GL(ClearColor)(0.0f, 0.0f, 1.0f, 1.0f);
    GL(Clear)(GL_COLOR_BUFFER_BIT);
    GL(UseProgram)(program);
    GL(VertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
    GL(EnableVertexAttribArray)(0);
    GL(DrawArrays)(GL_TRIANGLE_STRIP, 0, 4);

    std::vector<unsigned char> pixels(64 * 64 * 4);
    GL(ReadPixels)(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (GL(GetError)() != GL_NO_ERROR) {
        fail("a GL call failed");
    }
    for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            const unsigned char *pixel = &pixels[(y * 64 + x) * 4];
            const bool red = x < 32;
            if (pixel[0] != (red ? 255 : 0) || pixel[1] != 0 || pixel[2] != (red ? 0 : 255) || pixel[3] != 255) {
                fail("pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + std::to_string(pixel[0]) +
                     " " + std::to_string(pixel[1]) + " " + std::to_string(pixel[2]) + " " +
                     std::to_string(pixel[3]));
            }
        }
    }

    const auto *renderer = reinterpret_cast<const char *>(GL(GetString)(GL_RENDERER));
    std::printf("angle-smoke: ES %d.%d on %s: 64 x 64 drawn and read back\n", major, minor,
                renderer != nullptr ? renderer : "?");
    EGL(MakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL(DestroyContext)(display, context);
    EGL(DestroySurface)(display, surface);
    EGL(Terminate)(display);
    return 0;
}
