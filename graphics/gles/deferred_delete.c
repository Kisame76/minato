/* The deletes that libEGL keeps when no context is current because the thread released its context
   (graphics/egl/deferred.c, a departure from Android for iOS's memory limit): glDeleteTextures, glDeleteBuffers,
   glDeleteFramebuffers and glDeleteRenderbuffers first offer their names to libEGL's minato_egl_defer_delete, which
   keeps them for that context and deletes them when the thread makes it current again; otherwise they go to ANGLE
   as before. With a context current libEGL answers at once from a thread-local flag. */
#include <dlfcn.h>
#include <pthread.h>

#include "libgles.h"

MINATO_HOST(glDeleteTextures);
MINATO_HOST(glDeleteBuffers);
MINATO_HOST(glDeleteFramebuffers);
MINATO_HOST(glDeleteRenderbuffers);

static int (*defer)(int kind, int count, const unsigned *names);
static pthread_once_t found = PTHREAD_ONCE_INIT;

static void find(void) {
    void *egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (egl == NULL) {
        egl = dlopen("libEGL.so", RTLD_NOW);
    }
    defer = egl != NULL ? (int (*)(int, int, const unsigned *))dlsym(egl, "minato_egl_defer_delete") : NULL;
}

static int kept(int kind, GLsizei count, const GLuint *names) {
    pthread_once(&found, find);
    return defer != NULL && defer(kind, count, names);
}

void glDeleteTextures(GLsizei n, const GLuint *textures) {
    if (!kept(0, n, textures)) {
        minato_host_glDeleteTextures(n, textures);
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    if (!kept(1, n, buffers)) {
        minato_host_glDeleteBuffers(n, buffers);
    }
}

void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {
    if (!kept(2, n, framebuffers)) {
        minato_host_glDeleteFramebuffers(n, framebuffers);
    }
}

void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) {
    if (!kept(3, n, renderbuffers)) {
        minato_host_glDeleteRenderbuffers(n, renderbuffers);
    }
}
