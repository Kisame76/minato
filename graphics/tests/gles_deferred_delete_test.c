/* graphics.gles_deferred_delete: the limits of the deletes libEGL keeps for a released context
   (graphics/egl/deferred.c, a departure from Android for iOS's memory limit). A thread makes context C current,
   makes textures, releases C (eglMakeCurrent with no context, as SDL 2's backup does) and deletes them with no
   context current (as Ren'Py 8.5.3 frees its textures in the background). Android drops such deletes; Minato keeps
   them for C on that thread.
   1. C current again on the same thread: the textures are gone (glIsTexture GL_FALSE). On Android they would still
      be there; graphics.gles_resume_free shows the memory this frees.
   2. Another context D made current on the thread in between: C's names are dropped, not applied, so D's texture of
      the same number and C's own textures stay (glIsTexture GL_TRUE in both).
   3. C destroyed in between: its names are dropped with it. A context made afterwards by another thread, with a
      texture of the same number, then made current on this thread, keeps that texture. ANGLE does not hand a
      destroyed context's handle out again (C 0x4, the later one 0x5 on the Mac), so this case shows only that
      nothing goes wrong; the drop frees the names at once and keeps them from a context that would get C's handle.
   Each case on pbuffer surfaces, so no window is needed. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <stdio.h>

enum { count = 3 };

static EGLDisplay display;
static EGLConfig config;

static EGLContext make_context(void) {
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    return eglCreateContext(display, config, EGL_NO_CONTEXT, version);
}

static EGLSurface make_surface(void) {
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    return eglCreatePbufferSurface(display, config, size);
}

/* `count` textures with data, made in the context current now. */
static void make_textures(GLuint *textures) {
    static const unsigned char pixel[4] = {1, 2, 3, 4};
    glGenTextures(count, textures);
    for (int i = 0; i < count; i++) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    }
    glFinish();
}

/* How many of `textures` the context current now still has. */
static int alive(const GLuint *textures) {
    int alive = 0;
    for (int i = 0; i < count; i++) {
        alive += glIsTexture(textures[i]) == GL_TRUE;
    }
    return alive;
}

static void release(void) { eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }

/* Case 3's other thread: a context made after C was destroyed, with textures, released again. */
static EGLContext later;
static EGLSurface later_surface;
static GLuint later_textures[count];
static void *make_later(void *unused) {
    (void)unused;
    later = make_context();
    later_surface = make_surface();
    eglMakeCurrent(display, later_surface, later_surface, later);
    make_textures(later_textures);
    release();
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint configs = 0;
    eglChooseConfig(display, attributes, &config, 1, &configs);
    if (configs < 1) {
        printf("no pbuffer config\n");
        return 1;
    }
    GLuint textures[count];

    /* 1. The same context again on the same thread. */
    EGLContext c = make_context();
    EGLSurface surface = make_surface();
    eglMakeCurrent(display, surface, surface, c);
    make_textures(textures);
    release();
    glDeleteTextures(count, textures);
    eglMakeCurrent(display, surface, surface, c);
    const int same = alive(textures);
    printf("1. deleted with no context current, the same context current again on the same thread: %d of %d "
           "textures left\n", same, count);
    release();
    eglDestroyContext(display, c);

    /* 2. Another context made current on the thread in between. */
    c = make_context();
    EGLContext d = make_context();
    GLuint d_textures[count];
    eglMakeCurrent(display, surface, surface, d);
    make_textures(d_textures);
    eglMakeCurrent(display, surface, surface, c);
    make_textures(textures);
    release();
    glDeleteTextures(count, textures);
    eglMakeCurrent(display, surface, surface, d);
    const int in_d = alive(d_textures);
    eglMakeCurrent(display, surface, surface, c);
    const int in_c = alive(textures);
    printf("2. another context current in between: %d of %d of its own textures left, %d of %d in the first "
           "context (the deletes dropped, as Android drops them)\n", in_d, count, in_c, count);
    release();
    eglDestroyContext(display, c);
    eglDestroyContext(display, d);

    /* 3. The context destroyed in between. */
    c = make_context();
    eglMakeCurrent(display, surface, surface, c);
    make_textures(textures);
    release();
    glDeleteTextures(count, textures);
    eglDestroyContext(display, c);
    pthread_t thread;
    pthread_create(&thread, NULL, make_later, NULL);
    pthread_join(thread, NULL);
    eglMakeCurrent(display, later_surface, later_surface, later);
    const int in_later = alive(later_textures);
    printf("3. the context destroyed in between: a later context keeps %d of %d textures\n", in_later, count);
    release();
    eglDestroyContext(display, later);
    eglDestroySurface(display, later_surface);
    eglDestroySurface(display, surface);
    eglTerminate(display);

    const int ok = same == 0 && in_d == count && in_c == count && in_later == count;
    printf("gles deferred delete: %s\n", ok ? "applied only for the same context on the same thread"
                                            : "BROKEN: a kept delete went to the wrong context, or none was kept");
    return ok ? 0 : 1;
}
