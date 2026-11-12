/* graphics.gles_resume: SDL 2's Android surface order on a return, as an SDL game (e.g. a Ren'Py title) drives it
   through SDL_androidevents.c and SDL_android.c, reproduced deterministically on ANGLE's macOS slice with
   graphics-run's window and test control (test_control.h).

   The roles, as SDL runs them:
   - R, the render thread, holds the GL context current on the window surface and is busy drawing and swapping. It
     backs the context up (eglMakeCurrent with no surface and no context) when told to pause, and restores it on the
     return.
   - the UI thread (main here) plays Android's: it takes the window away, pauses, destroys the surface while R is
     still busy (R has not backed up), goes to the background, then on the return gives a NEW window (a removed one
     never returns), makes the new surface and lets R restore.

   The order the host keeps (apkrt.h, "The order the app keeps"), followed exactly, three returns in a row:
     1. the window is removed first: it is gone, so R's next swap fails at once with EGL_BAD_NATIVE_WINDOW;
     2. the lifecycle events, each answered within 1.5 s: onPause -> nativePause; then SDL's onNativeSurfaceDestroyed
        waits a bounded time for R to back the context up. R is busy and does not, so the wait times out and the UI
        thread calls eglDestroySurface while the surface is STILL current on R;
     3. the session goes to the background (BACKGROUND 1); R's backup -- eglMakeCurrent with nothing -- flushes the
        command buffer and parks in the window's commit gate until the foreground, under ANGLE's one EGL lock;
     4. the return is BACKGROUND 0 and a NEW window; the UI thread makes S2 = eglCreateWindowSurface on it (as SDL's
        onNativeSurfaceChanged does, synchronously, before mIsSurfaceReady), then R restores onto S2.

   On the Mac this is green: the serialization cannot leave the surface empty at the restore, so R restores onto S2,
   glGenTextures writes, the clear reads back and the swap presents. The device's result -- one unchanged name for
   every texture, zero swaps, eglGetError 0x300D -- happens only when eglCreateWindowSurface(S2) itself FAILS on the
   return (a stale or not-yet-registered window id, or a producer already on the new window); SDL resumes anyway and
   the restore silently makes nothing current. So the fixture's assertion is on the return's create: S2 is a real
   surface and eglGetError is EGL_SUCCESS, the context's old textures are still valid, a fresh glGenTextures binds, a
   known clear reads back, and the swap is EGL_TRUE. The create's result and error are logged every return, so a red
   run on the iPhone shows the cause on its own line.

   With MINATO_RESUME_SET=<n> (graphics.gles_resume_set; graphics-run's, which test control hands on) R also plays
   Ren'Py's full reset (gl2draw.on_resize -> TextureLoader.init -> quit) at each return: n RGBA textures with data,
   mipmapped, each sampled by a copy, made at the start; after each restore every one deleted with its own
   glDeleteTextures and n new ones made. Once the GPU is done ANGLE's color textures (test control's
   MINATO_TEST_COLOR_TEXTURES) must have dropped by the n deleted and come back to the start's after the reload: a
   texture the app deleted that stays alive grows every return by a set (the Simulator's Ren'Py anim game: about 600
   more per return). Its lines follow each return's restore line.

   With ",background" after the size (graphics.gles_resume_free) R frees the set as Ren'Py 8.5.3 does when it goes
   to the background: its core.py prints "Freeing memory." after SDL's backup has released the context, and
   free_memory -> kill_textures -> TextureLoader.cleanup deletes every texture with glDeleteTextures on R while no
   context is current. Android's wrapper drops such calls (graphics.gles_no_context), so the reset at the return
   finds the loader empty and deletes nothing, and the set stays alive in the context: the Simulator's Ren'Py anim
   game grew ANGLE's color textures by its 600 on every return this way. The check is the same: once the reset and
   the reload are done, ANGLE's color textures must be the start's again.

   With MINATO_RESUME_DIAG set (the iPhone run; not the Mac check, whose output is byte-compared) the host's memory
   line (MINATO_TEST_GRAPHICS_LINE) prints depth=<n> on stderr before the background, after the backup released,
   after the first return and after the third, and the backup and restore durations are printed too, for the
   surface/layer/depth leak. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_control.h"

enum { width = 64, height = 48, kept_textures = 3, returns = 3, set_most = 1000 };

/* The sentinel a name holds before glGenTextures: if the call writes nothing (no context current) it stays, which
   is how the device showed "one unchanged name for every texture". */
#define UNWRITTEN 0xABCDu

static EGLDisplay display;
static EGLConfig config;
static EGLContext context;
static int diag;  /* MINATO_RESUME_DIAG: the iPhone run's extra, non-deterministic lines on stderr */

/* data->egl_surface in SDL: the surface R restores onto; the UI thread sets it on each return. */
static _Atomic(EGLSurface) egl_surface;

/* The hand-off: to_render is written only by the UI thread, from_render only by R, so neither races the other.
   from_render only grows (a return's step, FR_STEPS a return), and the UI thread waits until it has reached a step:
   R goes on to the next return's FR_RAN right after FR_RESTORED, and a wait for an exact value could miss it and
   hang with R waiting for the next pause. */
enum { TR_PAUSE = 1, TR_BACKUP, TR_RESUME, TR_DONE };
enum { FR_RAN = 1, FR_GONE, FR_BACKED_UP, FR_RESTORED, FR_STEPS };
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int to_render, from_render;
static atomic_int entering_backup;

static GLuint kept[kept_textures];
static int set_size;  /* MINATO_RESUME_SET: Ren'Py's set, 0 without */
static int free_in_background;  /* ",background": the set deleted after the backup, with no context current */
static GLuint set[set_most];
static uint64_t set_start;  /* ANGLE's color textures with the set made, at the start */
static int set_ok[returns];
static int create_ok[returns], gl_ok[returns];
static EGLBoolean swapped[returns];
static double backup_ms, restore_ms;

static void tell(int value) {
    pthread_mutex_lock(&m);
    to_render = value;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&m);
}
static void report(int r, int value) {
    pthread_mutex_lock(&m);
    from_render = r * FR_STEPS + value;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&m);
}
static void await_to(int value) {
    pthread_mutex_lock(&m);
    while (to_render != value) {
        pthread_cond_wait(&cv, &m);
    }
    pthread_mutex_unlock(&m);
}
static void await_from(int r, int value) {
    pthread_mutex_lock(&m);
    while (from_render < r * FR_STEPS + value) {
        pthread_cond_wait(&cv, &m);
    }
    pthread_mutex_unlock(&m);
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}
static const char *ok(EGLBoolean value) { return value == EGL_TRUE ? "EGL_TRUE" : "EGL_FALSE"; }

/* Ren'Py's loader: the set's RGBA textures of 64 x 64 with data, mipmapped, each sampled once by a copy. */
static void load_set(void) {
    static unsigned char pixels[64 * 64 * 4];
    glGenTextures(set_size, set);
    for (int i = 0; i < set_size; i++) {
        glBindTexture(GL_TEXTURE_2D, set[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glGenerateMipmap(GL_TEXTURE_2D);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 4, 4);
    }
}

/* ANGLE's color textures once the GPU is done with what went: the lowest of up to 2 s of looks, early at `most`. */
static uint64_t color_textures(uint64_t most) {
    glFinish();
    uint64_t lowest = minato_test(MINATO_TEST_COLOR_TEXTURES, 0, 0, 0);
    for (int look = 0; look < 40 && lowest > most; look++) {
        usleep(50000);
        const uint64_t now = minato_test(MINATO_TEST_COLOR_TEXTURES, 0, 0, 0);
        lowest = now < lowest ? now : lowest;
    }
    return lowest;
}
static const char *yn(int value) { return value ? "yes" : "no"; }

/* R: owns the context across all returns, draws, backs up on each pause and restores on each return. */
static void *render_thread(void *argument) {
    EGLSurface surface = (EGLSurface)argument;
    eglMakeCurrent(display, surface, surface, context);
    glGenTextures(kept_textures, kept);
    for (int i = 0; i < kept_textures; i++) {
        glBindTexture(GL_TEXTURE_2D, kept[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    if (set_size > 0) {
        load_set();
        eglSwapBuffers(display, surface);
        set_start = color_textures(0);
    }

    for (int r = 0; r < returns; r++) {
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display, surface);
        report(r, FR_RAN);

        /* The window went while R is busy. R's next swap meets the gone window. */
        await_to(TR_PAUSE);
        const EGLBoolean swap = eglSwapBuffers(display, surface);
        const EGLint error = eglGetError();
        printf("return %d R: swap after the window went: %s, error %s\n", r + 1, ok(swap),
               error == EGL_BAD_NATIVE_WINDOW ? "EGL_BAD_NATIVE_WINDOW" : "another");
        report(r, FR_GONE);

        /* The UI thread has destroyed the surface and gone to the background; only now does R back up. */
        await_to(TR_BACKUP);
        atomic_store(&entering_backup, 1);
        const double at = now_ms();
        const EGLBoolean backed = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (r == 0) {
            backup_ms = now_ms() - at;
        }
        printf("return %d R: backup eglMakeCurrent(none): %s\n", r + 1, ok(backed));
        if (set_size > 0 && free_in_background) {
            /* Ren'Py's "Freeing memory." in the background: every texture deleted, one call each, no context. */
            for (int i = 0; i < set_size; i++) {
                glDeleteTextures(1, &set[i]);
            }
        }
        report(r, FR_BACKED_UP);

        /* The restore, after the UI thread made S2 (the real order: the create is done before the resume). */
        await_to(TR_RESUME);
        if (set_size > 0 && free_in_background) {
            /* Said only now, after the UI thread's line for the return, so that the output's order is fixed. */
            printf("return %d R: Ren'Py's free in the background: the set deleted with no context current\n", r + 1);
        }
        surface = atomic_load(&egl_surface);
        const double restore_at = now_ms();
        const EGLBoolean restored = eglMakeCurrent(display, surface, surface,
                                                   surface != EGL_NO_SURFACE ? context : EGL_NO_CONTEXT);
        if (r == 0) {
            restore_ms = now_ms() - restore_at;
        }
        GLuint fresh = UNWRITTEN;
        GLubyte pixel[4] = {0, 0, 0, 0};
        if (surface != EGL_NO_SURFACE) {
            glGenTextures(1, &fresh);
            glBindTexture(GL_TEXTURE_2D, fresh);
            glClearColor(0.0f, 1.0f, 0.0f, 1.0f);  /* pure green: exact in 8 bits, so the read-back is deterministic */
            glClear(GL_COLOR_BUFFER_BIT);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            swapped[r] = eglSwapBuffers(display, surface);
        }
        gl_ok[r] = fresh != UNWRITTEN && glIsTexture(fresh) == GL_TRUE && pixel[0] == 0 && pixel[1] == 255 &&
                   pixel[2] == 0 && pixel[3] == 255;
        printf("return %d R: restore eglMakeCurrent(%s): %s, error 0x%x; fresh name written: %s; clear read back "
               "%u %u %u %u; swap %s\n", r + 1, surface != EGL_NO_SURFACE ? "S2" : "none", ok(restored),
               eglGetError(), yn(fresh != UNWRITTEN), pixel[0], pixel[1], pixel[2], pixel[3], ok(swapped[r]));
        if (set_size > 0 && surface != EGL_NO_SURFACE) {
            /* Ren'Py's full reset: the set deleted, one call each (none left after a free in the background, whose
               cleanup emptied the loader); then its scene loaded again. */
            for (int i = 0; i < set_size && !free_in_background; i++) {
                glDeleteTextures(1, &set[i]);
            }
            const uint64_t want = set_start - (uint64_t)set_size;
            const uint64_t deleted = color_textures(want);
            load_set();
            eglSwapBuffers(display, surface);
            const uint64_t reloaded = color_textures(set_start);
            set_ok[r] = deleted <= want && reloaded <= set_start;
            char gone[64] = "yes", again[64] = "yes";
            if (deleted > want) {
                snprintf(gone, sizeof gone, "NO (%llu, the start's %llu)", (unsigned long long)deleted,
                         (unsigned long long)set_start);
            }
            if (reloaded > set_start) {
                snprintf(again, sizeof again, "NO (%llu, the start's %llu)", (unsigned long long)reloaded,
                         (unsigned long long)set_start);
            }
            if (free_in_background) {
                printf("return %d R: the set deleted with no context current, then the same context current again on "
                       "the same thread: gone from ANGLE's color textures: %s; loaded again, as many as at the start: "
                       "%s\n", r + 1, gone, again);
            } else {
                printf("return %d R: Ren'Py's reset: the set deleted, gone from ANGLE's color textures: %s; loaded "
                       "again, as many as at the start: %s\n", r + 1, gone, again);
            }
        }
        report(r, FR_RESTORED);
    }
    await_to(TR_DONE);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return NULL;
}

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

static void graphics_line(void) {
    if (diag) {
        minato_test(MINATO_TEST_GRAPHICS_LINE, 0, 0, 0);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    diag = getenv("MINATO_RESUME_DIAG") != NULL;
    /* The guest has not the Mac's environment: the host's MINATO_RESUME_SET through test control, a guest's own
       (the iPhone's run) first. */
    const char *set_text = getenv("MINATO_RESUME_SET");
    const uint64_t set_value = set_text != NULL ? (uint64_t)atoi(set_text) | (strstr(set_text, ",background") != NULL
                                                                              ? 1ull << 32 : 0)
                                                : minato_test(MINATO_TEST_RESUME_SET, 0, 0, 0);
    set_size = (int)(set_value & 0xffffffffu);
    free_in_background = (set_value >> 32) & 1;
    set_size = set_size < 0 ? 0 : set_size > set_most ? set_most : set_size;
    create_window create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_DEPTH_SIZE, 16, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint count = 0;
    eglChooseConfig(display, attributes, &config, 1, &count);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);

    ANativeWindow *window = create(minato_test(MINATO_TEST_WINDOW, 0, 0, 0), width, height);
    EGLSurface surface = eglCreateWindowSurface(display, config, window, NULL);
    pthread_t render;
    pthread_create(&render, NULL, render_thread, surface);

    for (int r = 0; r < returns; r++) {
        await_from(r, FR_RAN);
        if (r == 0) {
            graphics_line();  /* depth before the background */
        }
        /* 1. The window goes. 2. onPause, then SDL's bounded wait for the backup, which does not come. */
        printf("return %d UI: window removed (apkrt_window_remove -> %lld); onPause -> nativePause\n", r + 1,
               (long long)minato_test(MINATO_TEST_REMOVE, 0, 0, 0));
        tell(TR_PAUSE);
        await_from(r, FR_GONE);
        printf("return %d UI: surfaceDestroyed's wait for the backup timed out (R still busy); eglDestroySurface "
               "while current on R: %s\n", r + 1, ok(eglDestroySurface(display, surface)));

        /* 3. Into the background; R's backup then parks in the gate until the foreground. The UI thread does not
           wait for the backup (it cannot finish before the foreground), only for R to have entered it. */
        minato_test(MINATO_TEST_BACKGROUND, 1, 0, 0);
        atomic_store(&entering_backup, 0);
        tell(TR_BACKUP);
        while (atomic_load(&entering_backup) == 0) {
            usleep(1000);
        }

        /* 4. The return: foreground, then a NEW window and S2 made on the UI thread before the resume. */
        minato_test(MINATO_TEST_BACKGROUND, 0, 0, 0);
        ANativeWindow_release(window);
        const uint64_t id = minato_test(MINATO_TEST_ADD_WINDOW, ((uint64_t)width << 32) | (uint64_t)height, 0, 0);
        window = id != 0 ? create(id, width, height) : NULL;
        const double make_at = now_ms();
        surface = window != NULL ? eglCreateWindowSurface(display, config, window, NULL) : EGL_NO_SURFACE;
        const EGLint make_error = eglGetError();
        const double make_ms = now_ms() - make_at;
        create_ok[r] = surface != EGL_NO_SURFACE && make_error == EGL_SUCCESS;
        atomic_store(&egl_surface, surface);
        printf("return %d UI: new window %s; eglCreateWindowSurface(S2): %s, error 0x%x\n", r + 1,
               id != 0 ? "added" : "ABSENT (MINATO_TEST_ADD_WINDOW)",
               surface != EGL_NO_SURFACE ? "a surface" : "EGL_NO_SURFACE", make_error);
        await_from(r, FR_BACKED_UP);
        graphics_line();  /* depth after the backup released (diag only) */
        tell(TR_RESUME);
        await_from(r, FR_RESTORED);
        if (r == 0 || r == returns - 1) {
            graphics_line();  /* depth after the first return and after the third (diag only) */
        }
        if (diag) {
            fprintf(stderr, "resume diag: return %d, S2 made in %.0f ms\n", r + 1, make_ms);
        }
    }
    tell(TR_DONE);
    pthread_join(render, NULL);

    /* The old context's textures must have outlived all three returns. */
    eglMakeCurrent(display, surface, surface, context);
    int kept_ok = 1;
    for (int i = 0; i < kept_textures; i++) {
        kept_ok &= glIsTexture(kept[i]) == GL_TRUE;
    }
    int created = 1, drew = 1, presented = 1;
    for (int r = 0; r < returns; r++) {
        created &= create_ok[r];
        drew &= gl_ok[r];
        presented &= swapped[r] == EGL_TRUE;
    }
    if (diag) {
        fprintf(stderr, "resume diag: backup %.0f ms, restore %.0f ms\n", backup_ms, restore_ms);
    }
    printf("after three returns: S2 created every return: %s; GL worked every return: %s; presented every return: "
           "%s; kept textures still valid: %s\n", yn(created), yn(drew), yn(presented), yn(kept_ok));
    printf("gles resume: %s\n", (created && drew && presented && kept_ok) ? "the context survived every return" :
           "BROKEN: eglCreateWindowSurface failed on a return, so the restore made nothing current");
    int sets = 1;
    for (int r = 0; r < returns; r++) {
        sets &= set_ok[r];
    }
    if (set_size > 0) {
        printf("gles resume set: %s\n", sets ? "every return's deleted set left ANGLE" :
               "BROKEN: textures the app deleted stayed alive in ANGLE");
    }

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) {
        eglDestroySurface(display, surface);
    }
    eglDestroyContext(display, context);
    if (window != NULL) {
        ANativeWindow_release(window);
    }
    eglTerminate(display);
    return (created && drew && presented && kept_ok && (set_size == 0 || sets)) ? 0 : 1;
}
