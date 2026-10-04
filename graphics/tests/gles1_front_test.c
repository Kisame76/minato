/* ADR-0041 a1 (graphics.gles1_front): libGLESv1_CM, the ES 1.x front lib, on the Mac, on ANGLE's macOS slice, with
   EGL's front lib, on a pbuffer. Linked as an app links it, against the NDK's stub, so it needs the guest system's
   libGLESv1_CM.so.
   - An ES 1.1 context: GL_VERSION, a clear, and fixed-function triangles whose arrays come through
     glVertexPointerBounds and glColorPointerBounds, and whose one color comes through glColor4ub (small arguments,
     extended by the stub), read back at pixel centers, in colors exact in 8 bits.
   - GL_VERSION names the Common profile, "OpenGL ES-CM 1.1", as the ES 1.1 specification and Android's drivers do.
   - Android's 7 *Bounds functions, which the NDK exports but no Khronos registry defines: each hands its pointer to
     its plain function and drops the count (opengl/libs/GLES_CM/gl.cpp at the tag). Each runs after its plain
     function with another pointer, then gives the plain function's GL error and its own pointer to glGetPointerv.
     ANGLE lacks OES_matrix_palette (its string leaves it out), so glMatrixIndexPointerOES and glWeightPointerOES
     answer GL_INVALID_OPERATION, and the two *Bounds functions show that they forward by the same error.
   - A name the NDK exports and ANGLE lacks (glAlphaFuncxOES): it does nothing, sets no GL error and is logged once
     on stderr (ADR-0030; Android's gl_unimplemented logs and sets no error either). */
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Android's own, exported by libGLESv1_CM and declared in no NDK header, as frameworks/base's GLES10.cpp declares
   them. */
GL_API void GL_APIENTRY glColorPointerBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                             GLsizei count);
GL_API void GL_APIENTRY glNormalPointerBounds(GLenum type, GLsizei stride, const GLvoid *pointer, GLsizei count);
GL_API void GL_APIENTRY glTexCoordPointerBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                                GLsizei count);
GL_API void GL_APIENTRY glVertexPointerBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                              GLsizei count);
GL_API void GL_APIENTRY glPointSizePointerOESBounds(GLenum type, GLsizei stride, const GLvoid *pointer, GLsizei count);
GL_API void GL_APIENTRY glMatrixIndexPointerOESBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                                      GLsizei count);
GL_API void GL_APIENTRY glWeightPointerOESBounds(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer,
                                                 GLsizei count);

#define SIZE 64

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

static const char *gl_error(GLenum error) {
    switch (error) {
        case GL_NO_ERROR: return "GL_NO_ERROR";
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        default: return "another error";
    }
}

/* The pixel whose center is (x + 0.5, y + 0.5), as "r g b a". */
static void pixel(const char *what, int x, int y) {
    uint8_t rgba[4] = {0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    printf("  %s (%d, %d): %u %u %u %u\n", what, x, y, rgba[0], rgba[1], rgba[2], rgba[3]);
}

/* The lower left half of the viewport, below the diagonal from (-1, 1) to (1, -1). */
static const GLfloat triangle[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f};
static const GLubyte magenta[] = {255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255};

static void draws(void) {
    printf("draws:\n");
    glViewport(0, 0, SIZE, SIZE);
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    pixel("clear", 48, 48);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointerBounds(2, GL_FLOAT, 0, triangle, 3);
    glColorPointerBounds(4, GL_UNSIGNED_BYTE, 0, magenta, 3);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    pixel("arrays through the *Bounds functions, inside", 16, 16);
    pixel("arrays through the *Bounds functions, outside", 48, 48);
    glDisableClientState(GL_COLOR_ARRAY);
    glColor4ub(0, 255, 0, 255);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    pixel("glColor4ub, inside", 16, 16);
    glDisableClientState(GL_VERTEX_ARRAY);
    printf("  GL error: %s\n", gl_error(glGetError()));
}

/* Each plain function with one pointer, then its *Bounds function with another, then glGetPointerv of the
   array's pointer. */
static void bounds(void) {
    static const GLfloat plain[8][16], data[8][16];
    printf("*Bounds functions after their plain functions:\n");
    const struct {
        const char *name;
        GLenum query;
    } arrays[] = {
        {"glVertexPointerBounds", GL_VERTEX_ARRAY_POINTER},
        {"glNormalPointerBounds", GL_NORMAL_ARRAY_POINTER},
        {"glColorPointerBounds", GL_COLOR_ARRAY_POINTER},
        {"glTexCoordPointerBounds", GL_TEXTURE_COORD_ARRAY_POINTER},
        {"glPointSizePointerOESBounds", GL_POINT_SIZE_ARRAY_POINTER_OES},
        {"glMatrixIndexPointerOESBounds", GL_MATRIX_INDEX_ARRAY_POINTER_OES},
        {"glWeightPointerOESBounds", GL_WEIGHT_ARRAY_POINTER_OES},
    };
    for (unsigned index = 0; index < sizeof arrays / sizeof arrays[0]; index++) {
        const void *first = plain[index];
        switch (index) {
            case 0: glVertexPointer(2, GL_FLOAT, 0, first); break;
            case 1: glNormalPointer(GL_FLOAT, 0, first); break;
            case 2: glColorPointer(4, GL_FLOAT, 0, first); break;
            case 3: glTexCoordPointer(2, GL_FLOAT, 0, first); break;
            case 4: glPointSizePointerOES(GL_FLOAT, 0, first); break;
            case 5: glMatrixIndexPointerOES(1, GL_UNSIGNED_BYTE, 0, first); break;
            default: glWeightPointerOES(1, GL_FLOAT, 0, first); break;
        }
        const GLenum plain_error = glGetError();
        const void *mine = data[index];
        switch (index) {
            case 0: glVertexPointerBounds(2, GL_FLOAT, 0, mine, 4); break;
            case 1: glNormalPointerBounds(GL_FLOAT, 0, mine, 4); break;
            case 2: glColorPointerBounds(4, GL_FLOAT, 0, mine, 4); break;
            case 3: glTexCoordPointerBounds(2, GL_FLOAT, 0, mine, 4); break;
            case 4: glPointSizePointerOESBounds(GL_FLOAT, 0, mine, 4); break;
            case 5: glMatrixIndexPointerOESBounds(1, GL_UNSIGNED_BYTE, 0, mine, 4); break;
            default: glWeightPointerOESBounds(1, GL_FLOAT, 0, mine, 4); break;
        }
        const GLenum set = glGetError();
        void *got = NULL;
        glGetPointerv(arrays[index].query, &got);
        const GLenum queried = glGetError();
        printf("  %s: %s, as its plain function: %s; its pointer back: %s (%s)\n", arrays[index].name,
               gl_error(set), yes(set == plain_error), yes(got == mine), gl_error(queried));
    }
}

static void missing(void) {
    printf("a name ANGLE lacks:\n");
    glAlphaFuncxOES(GL_ALWAYS, 0);
    glAlphaFuncxOES(GL_ALWAYS, 0);
    printf("  glAlphaFuncxOES twice: %s\n", gl_error(glGetError()));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    printf("eglInitialize: %s\n", eglInitialize(display, &major, &minor) ? "EGL_TRUE" : "EGL_FALSE");
    const EGLint wanted[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = NULL;
    EGLint found = 0;
    const EGLBoolean chosen = eglChooseConfig(display, wanted, &config, 1, &found);
    printf("a config for ES 1.x on a pbuffer: %s\n", yes(chosen && found == 1));
    const EGLint size[] = {EGL_WIDTH, SIZE, EGL_HEIGHT, SIZE, EGL_NONE};
    const EGLSurface pbuffer = eglCreatePbufferSurface(display, config, size);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    printf("an ES 1.x context, current on the pbuffer: %s\n",
           yes(context != EGL_NO_CONTEXT && eglMakeCurrent(display, pbuffer, pbuffer, context)));
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    printf("GL_VERSION begins with OpenGL ES-CM 1.1: %s\n",
           yes(gl_version != NULL && strncmp(gl_version, "OpenGL ES-CM 1.1", 16) == 0));
    draws();
    bounds();
    missing();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, pbuffer);
    printf("eglTerminate: %s\n", eglTerminate(display) ? "EGL_TRUE" : "EGL_FALSE");
    return 0;
}
