/* ADR-0058 b), its second part: ahb-image-test, EGL images of AHardwareBuffers through the front libEGL
   (EGL_ANDROID_get_native_client_buffer and EGL_ANDROID_image_native_buffer), on ANGLE's Metal display and, with
   MINATO_ANGLE_BACKEND=vulkan, on its Vulkan display (graphics.ahb_image and graphics.ahb_image_vulkan). On a pbuffer
   of 4 x 4 pixels and an ES 3 context:
   - both extensions among the display's, and the refusals the extensions' texts name: a NULL buffer for
     eglGetNativeClientBufferANDROID, a context or no buffer for eglCreateImageKHR;
   - a buffer the CPU filled, sampled through samplerExternalOES in ESSL 1 and ESSL 3 and through a GL_TEXTURE_2D
     sampler, every pixel read back; a write of the CPU after the image is made, seen by the next draw, since the
     image is the buffer and no copy;
   - R8G8B8X8, whose alpha samples as 1, and R5G6B5;
   - the GPU's draw into a renderbuffer of the image, read back by the CPU under a lock after glFinish;
   - eglDestroyImageKHR, a second time EGL_BAD_PARAMETER, and the buffer released after its image.
   The pixels are derived by hand: NEAREST filtering at texel centres reads each texel as written. */
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *egl_error(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        default: return "another error";
    }
}

static int has(const char *list, const char *name) {
    const size_t length = strlen(name);
    for (const char *at = list != NULL ? strstr(list, name) : NULL; at != NULL; at = strstr(at + 1, name)) {
        if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == '\0')) {
            return 1;
        }
    }
    return 0;
}

static AHardwareBuffer *allocated(uint32_t format, uint64_t usage) {
    AHardwareBuffer_Desc description;
    memset(&description, 0, sizeof description);
    description.width = 4;
    description.height = 4;
    description.layers = 1;
    description.format = format;
    description.usage = usage;
    AHardwareBuffer *buffer = NULL;
    return AHardwareBuffer_allocate(&description, &buffer) == 0 ? buffer : NULL;
}

static uint32_t stride_of(AHardwareBuffer *buffer) {
    AHardwareBuffer_Desc description;
    AHardwareBuffer_describe(buffer, &description);
    return description.stride;
}

/* Texel (x, y) of the 4 x 4 pattern, as R, G, B, A bytes. */
static void texel(int x, int y, uint8_t out[4]) {
    out[0] = (uint8_t)(x * 80);
    out[1] = (uint8_t)(y * 80);
    out[2] = 200;
    out[3] = (uint8_t)(255 - x * 16);
}

static int fill_rgba(AHardwareBuffer *buffer, uint32_t format) {
    void *address = NULL;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &address) != 0) {
        return -1;
    }
    const uint32_t stride = stride_of(buffer);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint8_t *pixel = (uint8_t *)address + ((size_t)y * stride + (size_t)x) * 4;
            texel(x, y, pixel);
            if (format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
                pixel[3] = 7;
            }
        }
    }
    return AHardwareBuffer_unlock(buffer, NULL);
}

/* R5G6B5, red in the top 5 bits: each channel at 0 or its maximum, so the conversion to 8 bits is exact: red for odd
   x, green for odd y, blue for x of 2 and 3. */
static int fill_565(AHardwareBuffer *buffer) {
    void *address = NULL;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &address) != 0) {
        return -1;
    }
    const uint32_t stride = stride_of(buffer);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint16_t *pixel = (uint16_t *)address + (size_t)y * stride + (size_t)x;
            *pixel = (uint16_t)((x & 1 ? 31 : 0) << 11 | (y & 1 ? 63 : 0) << 5 | (x >= 2 ? 31 : 0));
        }
    }
    return AHardwareBuffer_unlock(buffer, NULL);
}

static GLuint shader(GLenum type, const char *source) {
    const GLuint made = glCreateShader(type);
    glShaderSource(made, 1, &source, NULL);
    glCompileShader(made);
    GLint compiled = 0;
    glGetShaderiv(made, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512] = "";
        glGetShaderInfoLog(made, sizeof log, NULL, log);
        printf("shader not compiled: %s\n", log);
    }
    return made;
}

static const char *VERTEX_1 =
    "attribute vec2 position;\n"
    "varying vec2 coordinate;\n"
    "void main() { coordinate = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n";
static const char *EXTERNAL_1 =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES image;\n"
    "varying vec2 coordinate;\n"
    "void main() { gl_FragColor = texture2D(image, coordinate); }\n";
static const char *PLAIN_1 =
    "precision mediump float;\n"
    "uniform sampler2D image;\n"
    "varying vec2 coordinate;\n"
    "void main() { gl_FragColor = texture2D(image, coordinate); }\n";
static const char *SOLID_1 =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "void main() { gl_FragColor = color; }\n";
static const char *VERTEX_3 =
    "#version 300 es\n"
    "in vec2 position;\n"
    "out vec2 coordinate;\n"
    "void main() { coordinate = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n";
static const char *EXTERNAL_3 =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES image;\n"
    "in vec2 coordinate;\n"
    "out vec4 color;\n"
    "void main() { color = texture(image, coordinate); }\n";

static GLuint program(const char *vertex, const char *fragment) {
    const GLuint made = glCreateProgram();
    glAttachShader(made, shader(GL_VERTEX_SHADER, vertex));
    glAttachShader(made, shader(GL_FRAGMENT_SHADER, fragment));
    glBindAttribLocation(made, 0, "position");
    glLinkProgram(made);
    GLint linked = 0;
    glGetProgramiv(made, GL_LINK_STATUS, &linked);
    if (!linked) {
        printf("program not linked\n");
    }
    return made;
}

/* Draws the image through `target` with `used` into the bound 4 x 4 framebuffer and prints its pixels, bottom row
   first as glReadPixels gives them, which is the buffer's first row: texture coordinate 0 is the buffer's row 0. */
static void draw(const char *name, GLuint used, GLenum target, EGLImageKHR image) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(target, texture);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(target, (GLeglImageOES)image);
    const GLenum bound = glGetError();
    static const GLfloat QUAD[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glUseProgram(used);
    glUniform1i(glGetUniformLocation(used, "image"), 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, QUAD);
    glEnableVertexAttribArray(0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    uint8_t pixels[4 * 4 * 4];
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    printf("%s: bound 0x%x, draw 0x%x\n", name, bound, glGetError());
    for (int y = 0; y < 4; y++) {
        printf("  row %d:", y);
        for (int x = 0; x < 4; x++) {
            const uint8_t *pixel = pixels + (y * 4 + x) * 4;
            printf(" %02x%02x%02x%02x", pixel[0], pixel[1], pixel[2], pixel[3]);
        }
        printf("\n");
    }
    glDeleteTextures(1, &texture);
}

static EGLImageKHR image_of(EGLDisplay display, AHardwareBuffer *buffer) {
    static const EGLint PRESERVED[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    return eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                             eglGetNativeClientBufferANDROID(buffer), PRESERVED);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, NULL, NULL)) {
        printf("eglInitialize: %s\n", egl_error(eglGetError()));
        return 1;
    }
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    printf("EGL_ANDROID_get_native_client_buffer: %d\n", has(extensions, "EGL_ANDROID_get_native_client_buffer"));
    printf("EGL_ANDROID_image_native_buffer: %d\n", has(extensions, "EGL_ANDROID_image_native_buffer"));

    static const EGLint CONFIG[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    eglChooseConfig(display, CONFIG, &config, 1, &count);
    static const EGLint SIZE[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    const EGLSurface surface = eglCreatePbufferSurface(display, config, SIZE);
    static const EGLint VERSION[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, VERSION);
    if (count != 1 || surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) {
        printf("no context: %s\n", egl_error(eglGetError()));
        return 1;
    }
    const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    printf("GL_OES_EGL_image_external: %d\n", has(gl_extensions, "GL_OES_EGL_image_external"));

    // The refusals.
    EGLClientBuffer none = eglGetNativeClientBufferANDROID(NULL);
    printf("eglGetNativeClientBufferANDROID(NULL): %s, %s\n", none == NULL ? "NULL" : "a buffer",
           egl_error(eglGetError()));
    const uint64_t sampled = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                             AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer *buffer = allocated(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, sampled);
    if (buffer == NULL || fill_rgba(buffer, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) != 0) {
        printf("no buffer\n");
        return 1;
    }
    EGLImageKHR refused = eglCreateImageKHR(display, context, EGL_NATIVE_BUFFER_ANDROID,
                                            eglGetNativeClientBufferANDROID(buffer), NULL);
    printf("eglCreateImageKHR with a context: %s, %s\n", refused == EGL_NO_IMAGE_KHR ? "EGL_NO_IMAGE_KHR" : "an image",
           egl_error(eglGetError()));
    refused = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, NULL, NULL);
    printf("eglCreateImageKHR without a buffer: %s, %s\n",
           refused == EGL_NO_IMAGE_KHR ? "EGL_NO_IMAGE_KHR" : "an image", egl_error(eglGetError()));

    // R8G8B8A8, sampled three ways.
    EGLImageKHR image = image_of(display, buffer);
    printf("eglCreateImageKHR: %s, %s\n", image == EGL_NO_IMAGE_KHR ? "EGL_NO_IMAGE_KHR" : "an image",
           egl_error(eglGetError()));
    GLuint target = 0, framebuffer = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glViewport(0, 0, 4, 4);
    const GLuint external_1 = program(VERTEX_1, EXTERNAL_1);
    const GLuint external_3 = program(VERTEX_3, EXTERNAL_3);
    const GLuint plain = program(VERTEX_1, PLAIN_1);
    draw("RGBA8 through samplerExternalOES, ESSL 1", external_1, GL_TEXTURE_EXTERNAL_OES, image);
    draw("RGBA8 through samplerExternalOES, ESSL 3", external_3, GL_TEXTURE_EXTERNAL_OES, image);
    draw("RGBA8 through sampler2D", plain, GL_TEXTURE_2D, image);

    // A write of the CPU after the image is made: texel (0, 0) becomes 0x11223344.
    void *address = NULL;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &address) == 0) {
        memcpy(address, "\x11\x22\x33\x44", 4);
        AHardwareBuffer_unlock(buffer, NULL);
    }
    draw("RGBA8 after the CPU's write", external_3, GL_TEXTURE_EXTERNAL_OES, image);
    printf("eglDestroyImageKHR: %d\n", eglDestroyImageKHR(display, image));
    printf("eglDestroyImageKHR again: %d, %s\n", eglDestroyImageKHR(display, image), egl_error(eglGetError()));
    AHardwareBuffer_release(buffer);

    // R8G8B8X8 and R5G6B5.
    AHardwareBuffer *opaque = allocated(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, sampled);
    if (opaque != NULL && fill_rgba(opaque, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) == 0) {
        image = image_of(display, opaque);
        draw("RGBX8 through samplerExternalOES", external_3, GL_TEXTURE_EXTERNAL_OES, image);
        eglDestroyImageKHR(display, image);
    }
    AHardwareBuffer_release(opaque);
    AHardwareBuffer *packed = allocated(AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, sampled);
    if (packed != NULL && fill_565(packed) == 0) {
        image = image_of(display, packed);
        draw("RGB565 through samplerExternalOES", external_3, GL_TEXTURE_EXTERNAL_OES, image);
        eglDestroyImageKHR(display, image);
    }
    AHardwareBuffer_release(packed);

    // The GPU's draw into a renderbuffer of the image, read by the CPU.
    AHardwareBuffer *drawn = allocated(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                       AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN);
    image = drawn != NULL ? image_of(display, drawn) : EGL_NO_IMAGE_KHR;
    GLuint renderbuffer = 0;
    glGenRenderbuffers(1, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)image);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
    printf("renderbuffer: complete %d\n", glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    const GLuint solid = program(VERTEX_1, SOLID_1);
    static const GLfloat QUAD[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glUseProgram(solid);
    glUniform4f(glGetUniformLocation(solid, "color"), 0x10 / 255.0f, 0x20 / 255.0f, 0x30 / 255.0f, 0x40 / 255.0f);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, QUAD);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    if (drawn != NULL && AHardwareBuffer_lock(drawn, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &address) == 0) {
        const uint8_t *first = address;
        const uint8_t *last = (const uint8_t *)address + ((size_t)3 * stride_of(drawn) + 3) * 4;
        printf("drawn by the GPU: %02x%02x%02x%02x, last %02x%02x%02x%02x\n", first[0], first[1], first[2],
               first[3], last[0], last[1], last[2], last[3]);
        AHardwareBuffer_unlock(drawn, NULL);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteRenderbuffers(1, &renderbuffer);
    eglDestroyImageKHR(display, image);
    AHardwareBuffer_release(drawn);

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(display);
    printf("done\n");
    return 0;
}
