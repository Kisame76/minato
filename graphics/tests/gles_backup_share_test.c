/* graphics.gles_backup_share: a context made on a thread that has backed its context up shares that context's objects,
   and only under the conditions of Ren'Py's race (a departure from Android, for iOS's memory limit).
   SDL 2 backs its context C up on a pause (eglMakeCurrent with nothing). A frame that Ren'Py 8.5.3 draws after that
   fails, its display reset finds no context current, and pygame_sdl2 makes a new context D (SDL_GL_CreateContext with
   no share context) that it never deletes. Ren'Py loads its scene into D; SDL's restore makes C current again, so D
   keeps a whole scene set alive (graphics.gles_resume_race). libEGL makes such a D with C as its share context when:
   - the calling thread released C with eglMakeCurrent and has made no context current since;
   - C is alive and current on no thread;
   - no share context is given, and D asks for C's config and C's attributes;
   - the host window C was last current on is not open: gone, removed, or its gate closed. Ren'Py's race comes after
     SDL's surfaceDestroyed, when Minato has removed the window.
   Then Ren'Py's deletes in D reach C's objects and its load goes into the share group, as it believes, and the deletes
   libEGL keeps for C (deferred.c) are applied when D is made current: the shared kinds (textures, buffers,
   renderbuffers); framebuffers are per context and are dropped.
   Each case backs up a context C with a texture on a new window, sets up one condition and asks D, made then and
   current on a pbuffer, glIsTexture of C's texture: shared or not. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "test_control.h"

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

static create_window create;
static EGLDisplay display;
static EGLConfig config, other_config;
static const EGLint version3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
static const EGLint version2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
static const EGLint small[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};

/* C with one texture and one framebuffer, made current on a new window's surface, then backed up. */
struct backed {
    EGLContext context;
    EGLSurface surface;
    ANativeWindow *window;
    GLuint texture, kept, framebuffer;
};

static struct backed back_up(void) {
    struct backed b = {0};
    const uint64_t id = minato_test(MINATO_TEST_ADD_WINDOW, 0, 0, 0);
    b.window = id != 0 ? create(id, 64, 48) : NULL;
    b.context = eglCreateContext(display, config, EGL_NO_CONTEXT, version3);
    b.surface = b.window != NULL ? eglCreateWindowSurface(display, config, b.window, NULL) : EGL_NO_SURFACE;
    eglMakeCurrent(display, b.surface, b.surface, b.context);
    static const unsigned char pixel[4] = {1, 2, 3, 4};
    GLuint textures[2];
    glGenTextures(2, textures);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    }
    b.texture = textures[0];
    b.kept = textures[1];
    glGenFramebuffers(1, &b.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, b.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);  // SDL's backup
    return b;
}

static void window_gone(void) { minato_test(MINATO_TEST_REMOVE, 0, 0, 0); }

/* Whether D, current on a pbuffer, sees C's texture; D is released again after. */
static int sees(EGLContext d, GLuint texture) {
    if (d == EGL_NO_CONTEXT) {
        return -1;
    }
    EGLConfig mine;
    EGLint id = 0;
    eglQueryContext(display, d, EGL_CONFIG_ID, &id);
    const EGLint by_id[] = {EGL_CONFIG_ID, id, EGL_NONE};
    EGLint count = 0;
    eglChooseConfig(display, by_id, &mine, 1, &count);
    EGLSurface pbuffer = eglCreatePbufferSurface(display, mine, small);
    eglMakeCurrent(display, pbuffer, pbuffer, d);
    const int seen = glIsTexture(texture) == GL_TRUE;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, pbuffer);
    return seen;
}

static void finish(struct backed *b, EGLContext d) {
    if (d != EGL_NO_CONTEXT) {
        eglDestroyContext(display, d);
    }
    if (b->context != EGL_NO_CONTEXT) {
        eglDestroyContext(display, b->context);
    }
    if (b->surface != EGL_NO_SURFACE) {
        eglDestroySurface(display, b->surface);
    }
    if (b->window != NULL) {
        ANativeWindow_release(b->window);
    }
}

static int failures;

static void expect(const char *what, int seen, int shared) {
    const int good = seen == shared;
    failures += !good;
    printf("%s: %s%s\n", what, seen < 0 ? "D NOT MADE" : seen ? "shares C's objects" : "a namespace of its own",
           good ? "" : shared ? " -- WRONG, it should share" : " -- WRONG, it should not share");
}

/* The other thread of case 5: makes C current on a pbuffer and holds it until told. */
static atomic_int holding, let_go;
static EGLContext held;

static void *holder(void *unused) {
    (void)unused;
    EGLSurface pbuffer = eglCreatePbufferSurface(display, config, small);
    eglMakeCurrent(display, pbuffer, pbuffer, held);
    atomic_store(&holding, 1);
    while (!atomic_load(&let_go)) {
        usleep(1000);
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, pbuffer);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig configs[64];
    EGLint count = 0;
    eglChooseConfig(display, attributes, configs, 64, &count);
    if (create == NULL || count < 2) {
        printf("no window, or fewer than two window and pbuffer configs (%d)\n", count);
        return 1;
    }
    config = configs[0];
    other_config = configs[1];

    /* 1. The race: C's window gone, then D made. */
    struct backed b = back_up();
    window_gone();
    EGLContext d = eglCreateContext(display, config, EGL_NO_CONTEXT, version3);
    expect("1. C backed up, its window gone, D made with C's config and attributes", sees(d, b.texture), 1);
    finish(&b, d);

    /* 2. C's window still there with its gate closed. */
    b = back_up();
    minato_test(MINATO_TEST_GATE, 0, 0, 0);
    d = eglCreateContext(display, config, EGL_NO_CONTEXT, version3);
    minato_test(MINATO_TEST_GATE, 1, 0, 0);
    expect("2. C backed up, its window kept with its gate closed", sees(d, b.texture), 1);
    window_gone();
    finish(&b, d);

    /* 3. C's window there and open: an app's second context, not a race. */
    b = back_up();
    d = eglCreateContext(display, config, EGL_NO_CONTEXT, version3);
    expect("3. C backed up, its window open", sees(d, b.texture), 0);
    window_gone();
    finish(&b, d);

    /* 4. A share context given: that one, not C (made before C's backup, so it has a namespace of its own). */
    EGLContext given = eglCreateContext(display, config, EGL_NO_CONTEXT, version3);
    b = back_up();
    window_gone();
    d = eglCreateContext(display, config, given, version3);
    expect("4. a share context given", sees(d, b.texture), 0);
    eglDestroyContext(display, given);
    finish(&b, d);

    /* 5. C current on another thread. */
    b = back_up();
    window_gone();
    held = b.context;
    atomic_store(&holding, 0);
    atomic_store(&let_go, 0);
    pthread_t thread;
    pthread_create(&thread, NULL, holder, NULL);
    while (!atomic_load(&holding)) {
        usleep(1000);
    }
    d = eglCreateContext(display, config, EGL_NO_CONTEXT, version3);
    atomic_store(&let_go, 1);
    pthread_join(thread, NULL);
    expect("5. C current on another thread", sees(d, b.texture), 0);
    finish(&b, d);

    /* 6. Another config. */
    b = back_up();
    window_gone();
    d = eglCreateContext(display, other_config, EGL_NO_CONTEXT, version3);
    expect("6. D with another config", sees(d, b.texture), 0);
    finish(&b, d);

    /* 7. Another client version. */
    b = back_up();
    window_gone();
    d = eglCreateContext(display, config, EGL_NO_CONTEXT, version2);
    expect("7. D with another client version", sees(d, b.texture), 0);
    finish(&b, d);

    /* 8. C destroyed. */
    b = back_up();
    window_gone();
    eglDestroyContext(display, b.context);
    b.context = EGL_NO_CONTEXT;
    d = eglCreateContext(display, config, EGL_NO_CONTEXT, version3);
    expect("8. C destroyed", sees(d, b.texture), 0);
    finish(&b, d);

    /* 9. The deletes kept for C are applied when D is made current: the texture's in the share group, the
       framebuffer's dropped (a framebuffer is C's own). C current again then lacks the texture only. */
    b = back_up();
    glDeleteTextures(1, &b.texture);  // no context current: kept for C
    glDeleteFramebuffers(1, &b.framebuffer);
    window_gone();
    d = eglCreateContext(display, config, EGL_NO_CONTEXT, version3);
    const int kept_seen = sees(d, b.kept);  // D made current: the kept deletes run
    EGLSurface pbuffer = eglCreatePbufferSurface(display, config, small);
    eglMakeCurrent(display, pbuffer, pbuffer, b.context);
    const int texture_gone = glIsTexture(b.texture) == GL_FALSE, other_kept = glIsTexture(b.kept) == GL_TRUE;
    const int framebuffer_kept = glIsFramebuffer(b.framebuffer) == GL_TRUE;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, pbuffer);
    const int applied = kept_seen == 1 && texture_gone && other_kept && framebuffer_kept;
    failures += !applied;
    printf("9. deletes kept for C, then D made current: texture deleted %s, the other texture kept %s, C's framebuffer "
           "kept %s%s\n", texture_gone ? "yes" : "NO", other_kept ? "yes" : "NO", framebuffer_kept ? "yes" : "NO",
           applied ? "" : " -- WRONG");
    finish(&b, d);

    printf("gles backup share: %s\n", failures == 0 ? "a context made in a backup window shares C's objects, only "
                                                      "under the race's conditions"
                                                    : "BROKEN");
    eglTerminate(display);
    return failures == 0 ? 0 : 1;
}
