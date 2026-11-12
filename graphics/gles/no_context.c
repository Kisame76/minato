/* What libGLESv2 answers on a thread without a current context, where ANGLE's answer is not Android's
   (graphics/tests/gles_no_context_test.c, against Android 15's libGLESv2 on the emulator). Android's wrapper sends a
   call without a context to its no-context hooks, which do nothing and answer 0; ANGLE answers its error values
   there: -1 for a location, GL_INVALID_INDEX for an index, GL_WAIT_FAILED for a client wait. Each function below
   asks ANGLE and, only when ANGLE gives that error value, whether a context is current (libEGL's
   eglGetCurrentContext): without one the answer is Android's 0, with one ANGLE's value stands, as the GL
   specification asks for an invalid program or name. The normal path costs nothing more. glClientWaitSync's part is
   in gles.c, where it is written by hand already. A thread that finds no context so logs Android's line once, as
   Android's no-context hook does (E/libEGL "call to OpenGL ES API with no current context (logged once per
   thread)"); a call without an error value to look at (a void call, a 0 answer) logs nothing, so as to cost nothing. */
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>

#include "libgles.h"

MINATO_HOST(glGetUniformLocation);
MINATO_HOST(glGetAttribLocation);
MINATO_HOST(glGetUniformBlockIndex);
MINATO_HOST(glGetFragDataLocation);
MINATO_HOST(glGetProgramResourceIndex);
MINATO_HOST(glGetProgramResourceLocation);

static void *(*get_current_context)(void);
static pthread_once_t found = PTHREAD_ONCE_INIT;

static void find(void) {
    void *egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (egl == NULL) {
        egl = dlopen("libEGL.so", RTLD_NOW);
    }
    get_current_context = egl != NULL ? (void *(*)(void))dlsym(egl, "eglGetCurrentContext") : NULL;
}

static __thread int logged;

int minato_gles_no_context(void) {
    pthread_once(&found, find);
    const int none = get_current_context != NULL && get_current_context() == NULL;
    if (none && !logged) {
        logged = 1;
        __android_log_write(ANDROID_LOG_ERROR, "libEGL",
                            "call to OpenGL ES API with no current context (logged once per thread)");
    }
    return none;
}

GLint glGetUniformLocation(GLuint program, const GLchar *name) {
    const GLint location = minato_host_glGetUniformLocation(program, name);
    return location == -1 && minato_gles_no_context() ? 0 : location;
}

GLint glGetAttribLocation(GLuint program, const GLchar *name) {
    const GLint location = minato_host_glGetAttribLocation(program, name);
    return location == -1 && minato_gles_no_context() ? 0 : location;
}

GLuint glGetUniformBlockIndex(GLuint program, const GLchar *name) {
    const GLuint index = minato_host_glGetUniformBlockIndex(program, name);
    return index == GL_INVALID_INDEX && minato_gles_no_context() ? 0 : index;
}

GLint glGetFragDataLocation(GLuint program, const GLchar *name) {
    const GLint location = minato_host_glGetFragDataLocation(program, name);
    return location == -1 && minato_gles_no_context() ? 0 : location;
}

GLuint glGetProgramResourceIndex(GLuint program, GLenum interface, const GLchar *name) {
    const GLuint index = minato_host_glGetProgramResourceIndex(program, interface, name);
    return index == GL_INVALID_INDEX && minato_gles_no_context() ? 0 : index;
}

GLint glGetProgramResourceLocation(GLuint program, GLenum interface, const GLchar *name) {
    const GLint location = minato_host_glGetProgramResourceLocation(program, interface, name);
    return location == -1 && minato_gles_no_context() ? 0 : location;
}
