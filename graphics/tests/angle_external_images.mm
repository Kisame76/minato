// ADR-0058 b), patch 0010: on ANGLE's Metal backend, GL_OES_EGL_image_external and its ESSL 3 variant, a
// samplerExternalOES shader in ESSL 1 (texture2D) and in ESSL 3 (texture), and draws that sample an EGL image of an
// MTLTexture (EGL_METAL_TEXTURE_ANGLE) through GL_TEXTURE_EXTERNAL_OES, read back exactly. ANGLE without the patch
// fails at the extensions. Prints each step's answer and "PASS", or "FAIL: ..." and exits 1.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void *library;

static void *sym(const char *name) {
    void *address = dlsym(library, name);
    if (address == nullptr) {
        std::printf("FAIL: no %s\n", name);
        std::exit(1);
    }
    return address;
}

#define EGL(name) reinterpret_cast<decltype(&egl##name)>(sym("EGL_" #name))
#define GL(name) reinterpret_cast<decltype(&gl##name)>(sym("gl" #name))

static GLuint compile(GLenum type, const char *source, const char *what) {
    GLuint shader = GL(CreateShader)(type);
    GL(ShaderSource)(shader, 1, &source, nullptr);
    GL(CompileShader)(shader);
    GLint compiled = GL_FALSE;
    GL(GetShaderiv)(shader, GL_COMPILE_STATUS, &compiled);
    char log[2048] = {};
    GL(GetShaderInfoLog)(shader, sizeof log, nullptr, log);
    std::printf("%s: compiled %d%s%s\n", what, compiled, log[0] ? ", log: " : "", log);
    return compiled == GL_TRUE ? shader : 0;
}

static GLuint link(GLuint vertex, GLuint fragment, const char *what) {
    if (vertex == 0 || fragment == 0) return 0;
    GLuint program = GL(CreateProgram)();
    GL(AttachShader)(program, vertex);
    GL(AttachShader)(program, fragment);
    GL(BindAttribLocation)(program, 0, "position");
    GL(LinkProgram)(program);
    GLint linked = GL_FALSE;
    GL(GetProgramiv)(program, GL_LINK_STATUS, &linked);
    char log[2048] = {};
    GL(GetProgramInfoLog)(program, sizeof log, nullptr, log);
    std::printf("%s: linked %d%s%s\n", what, linked, log[0] ? ", log: " : "", log);
    return linked == GL_TRUE ? program : 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    @autoreleasepool {
        if (argc != 2) {
            std::printf("FAIL: usage: angle-external-images <libGLESv2.dylib>\n");
            return 1;
        }
        // Other work in the tree must not fail while ANGLE is being built (CTest: SKIP_REGULAR_EXPRESSION).
        if (access(argv[1], F_OK) != 0) {
            std::printf("skipped: %s is not built, see tools/build-angle.sh\n", argv[1]);
            return 0;
        }
        library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            std::printf("FAIL: dlopen %s\n", dlerror());
            return 1;
        }
        const EGLAttrib display_attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                                                EGL_NONE};
        EGLDisplay display = EGL(GetPlatformDisplay)(EGL_PLATFORM_ANGLE_ANGLE, nullptr, display_attributes);
        EGL(Initialize)(display, nullptr, nullptr);
        const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                            EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        EGL(ChooseConfig)(display, config_attributes, &config, 1, &configs);
        const EGLint surface_attributes[] = {EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_NONE};
        EGLSurface surface = EGL(CreatePbufferSurface)(display, config, surface_attributes);
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        EGLContext context = EGL(CreateContext)(display, config, EGL_NO_CONTEXT, context_attributes);
        if (EGL(MakeCurrent)(display, surface, surface, context) != EGL_TRUE) {
            std::printf("FAIL: no context\n");
            return 1;
        }
        const char *extensions = reinterpret_cast<const char *>(GL(GetString)(GL_EXTENSIONS));
        const bool has_external = std::strstr(extensions, "GL_OES_EGL_image_external ") != nullptr;
        const bool has_essl3 = std::strstr(extensions, "GL_OES_EGL_image_external_essl3") != nullptr;
        std::printf("GL_OES_EGL_image_external %d, _essl3 %d\n", has_external, has_essl3);
        if (!has_external || !has_essl3) {
            std::printf("FAIL: the external-image extensions are missing (patch 0010)\n");
            return 1;
        }

        GLuint v1 = compile(GL_VERTEX_SHADER,
                            "attribute vec2 position; varying vec2 uv;\n"
                            "void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "ESSL 1 vertex");
        GLuint f1 = compile(GL_FRAGMENT_SHADER,
                            "#extension GL_OES_EGL_image_external : require\n"
                            "precision mediump float; uniform samplerExternalOES s; varying vec2 uv;\n"
                            "void main() { gl_FragColor = texture2D(s, uv); }\n",
                            "ESSL 1 samplerExternalOES");
        GLuint p1 = link(v1, f1, "ESSL 1 program");
        GLuint v3 = compile(GL_VERTEX_SHADER,
                            "#version 300 es\nin vec2 position; out vec2 uv;\n"
                            "void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n",
                            "ESSL 3 vertex");
        GLuint f3 = compile(GL_FRAGMENT_SHADER,
                            "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\n"
                            "precision mediump float; uniform samplerExternalOES s; in vec2 uv; out vec4 color;\n"
                            "void main() { color = texture(s, uv); }\n",
                            "ESSL 3 samplerExternalOES");
        GLuint p3 = link(v3, f3, "ESSL 3 program");

        // An MTLTexture of ANGLE's device with four colors, as an EGL image, on an external texture.
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        MTLTextureDescriptor *descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
        const uint8_t pixels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:pixels bytesPerRow:8];
        const EGLint image_attributes[] = {EGL_NONE};
        auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(sym("EGL_CreateImageKHR"));
        EGLImageKHR image = create_image(display, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
                                         (__bridge EGLClientBuffer)texture, image_attributes);
        std::printf("EGL image of the MTLTexture: %s, error 0x%x\n", image != EGL_NO_IMAGE_KHR ? "made" : "none",
                    EGL(GetError)());
        GLuint external = 0;
        GL(GenTextures)(1, &external);
        GL(BindTexture)(GL_TEXTURE_EXTERNAL_OES, external);
        auto target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(sym("glEGLImageTargetTexture2DOES"));
        target(GL_TEXTURE_EXTERNAL_OES, image);
        std::printf("glEGLImageTargetTexture2DOES on GL_TEXTURE_EXTERNAL_OES: error 0x%x\n", GL(GetError)());
        GL(TexParameteri)(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        GL(TexParameteri)(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const GLfloat corners[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        GLuint programs[2] = {p1, p3};
        int failures = 0;
        for (int index = 0; index < 2; index++) {
            if (programs[index] == 0) {
                failures++;
                continue;
            }
            GL(UseProgram)(programs[index]);
            GL(Uniform1i)(GL(GetUniformLocation)(programs[index], "s"), 0);
            GL(VertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
            GL(EnableVertexAttribArray)(0);
            GL(ClearColor)(0.5f, 0.5f, 0.5f, 1.0f);
            GL(Clear)(GL_COLOR_BUFFER_BIT);
            GL(DrawArrays)(GL_TRIANGLE_STRIP, 0, 4);
            uint8_t read[16] = {};
            GL(ReadPixels)(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, read);
            std::printf("ESSL %d draw: error 0x%x, pixels the texture's %d (", index == 0 ? 1 : 3, GL(GetError)(),
                        std::memcmp(read, pixels, 16) == 0);
            for (int byte = 0; byte < 16; byte++) std::printf("%s%u", byte ? " " : "", read[byte]);
            std::printf(")\n");
            failures += std::memcmp(read, pixels, 16) != 0;
        }
        if (image == EGL_NO_IMAGE_KHR || failures != 0) {
            std::printf("FAIL: %d of the 2 draws did not sample the image\n", failures);
            return 1;
        }
        std::printf("PASS\n");
    }
    return 0;
}
