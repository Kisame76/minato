/* Deletes made with no context current, kept for the context the thread released (a departure from Android, for
   iOS's memory limit). SDL 2 backs its context up on a pause (eglMakeCurrent with no context) before the app sees
   the background event, and Ren'Py 8.5.3 then frees its textures there ("Freeing memory.": TextureLoader.cleanup,
   one glDeleteTextures each, on the same thread). Android's wrapper drops such calls (graphics.gles_no_context), so
   the textures stay alive in the context, and Ren'Py, which believes them gone, loads its scene again on the return:
   a set more on every return (the anim game: about 600 textures). On Android that costs GPU memory; on an iPhone a
   few returns can end the app (jetsam).

   So libGLES's glDeleteTextures, glDeleteBuffers, glDeleteFramebuffers and glDeleteRenderbuffers
   (graphics/gles/deferred_delete.c) hand their names here when no context is current on their thread because the
   thread released context C with eglMakeCurrent and has made none current since. The names wait under C, on that
   thread, and are deleted when the same thread makes C current again, at once after the eglMakeCurrent and before
   the app's next call. Until then they stay reserved in C's share group, so glGen* cannot hand them out again, and
   the app, which deleted them, does not bind them. The limits:
   - only those four deletes; any other call without a context does nothing, as on Android;
   - only C again on the same thread: another context made current there drops the names, and so do
     eglReleaseThread, eglDestroyContext(C) (ANGLE lets go of C's objects then anyway) and eglTerminate. One
     exception: a context libEGL made to share C's objects in the backup window (display.c, Ren'Py's race) deletes
     the textures, buffers and renderbuffers when it is made current; the framebuffers, which are C's own, drop;
   - at most `most_names` names a thread; past that a delete does nothing, as on Android, with one warning line.
   Names dropped by one of these rules are dropped as Android drops them; an I/libEGL line says how many and why.
   The one difference an app can see: glIsTexture on such a name answers GL_FALSE after the return, Android GL_TRUE
   until the context goes. */
#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "libegl.h"

enum { kinds = 4, framebuffers = 2, most_names = 1 << 18 };

/* The deletes in the order of minato_egl_defer_delete's kind. */
static const char *const delete_names[kinds] = {"glDeleteTextures", "glDeleteBuffers", "glDeleteFramebuffers",
                                                "glDeleteRenderbuffers"};

/* A thread's names waiting for `context`; EGL_NO_CONTEXT when none wait. */
struct queue {
    struct queue *next;
    EGLContext context;
    unsigned *names[kinds];
    size_t count[kinds], room[kinds], total;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct queue *queues;  // every thread's, for eglDestroyContext and eglTerminate
static pthread_key_t key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static __thread struct queue *mine;
static __thread int released;  // the fast path: this thread released a context and has made none current since
static atomic_int warned, told;

static void drop(struct queue *queue) {
    for (int kind = 0; kind < kinds; kind++) {
        free(queue->names[kind]);
        queue->names[kind] = NULL;
        queue->count[kind] = queue->room[kind] = 0;
    }
    queue->total = 0;
    queue->context = EGL_NO_CONTEXT;
}

static void forget(void *value) {
    struct queue *queue = value;
    pthread_mutex_lock(&lock);
    for (struct queue **at = &queues; *at != NULL; at = &(*at)->next) {
        if (*at == queue) {
            *at = queue->next;
            break;
        }
    }
    drop(queue);
    pthread_mutex_unlock(&lock);
    free(queue);
    // The thread is ending; another key's destructor (display.c's, eglReleaseThread) may still run after this one.
    mine = NULL;
    released = 0;
}

static void make_key(void) { pthread_key_create(&key, forget); }

static void say_dropped(size_t names, const char *why) {
    if (names > 0) {
        __android_log_print(ANDROID_LOG_INFO, "libEGL", "deletes made with no context current: %zu names dropped (%s), "
                            "as Android drops them", names, why);
    }
}

static struct queue *my_queue(void) {
    if (mine == NULL) {
        pthread_once(&key_once, make_key);
        struct queue *queue = calloc(1, sizeof *queue);
        if (queue == NULL) {
            return NULL;
        }
        pthread_mutex_lock(&lock);
        queue->next = queues;
        queues = queue;
        pthread_mutex_unlock(&lock);
        pthread_setspecific(key, queue);
        mine = queue;
    }
    return mine;
}

int minato_egl_defer_delete(int kind, int count, const unsigned *names) {
    if (!released || mine == NULL || kind < 0 || kind >= kinds || count <= 0 || names == NULL) {
        return 0;
    }
    int kept = 0;
    pthread_mutex_lock(&lock);
    struct queue *queue = mine;
    if (queue->context != EGL_NO_CONTEXT) {
        if (queue->total + (size_t)count > most_names) {
            if (atomic_exchange(&warned, 1) == 0) {
                __android_log_print(ANDROID_LOG_WARN, "libEGL", "deletes made with no context current: more than %d "
                                    "names wait for the context, the rest are dropped as Android drops them",
                                    most_names);
            }
        } else {
            size_t room = queue->room[kind];
            while (queue->count[kind] + (size_t)count > room) {
                room = room != 0 ? room * 2 : 256;
            }
            unsigned *grown = room != queue->room[kind] ? realloc(queue->names[kind], room * sizeof *grown)
                                                        : queue->names[kind];
            if (grown != NULL) {
                memcpy(grown + queue->count[kind], names, (size_t)count * sizeof *grown);
                queue->names[kind] = grown;
                queue->room[kind] = room;
                queue->count[kind] += (size_t)count;
                queue->total += (size_t)count;
                kept = 1;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return kept;
}

void minato_deferred_current(EGLContext before, EGLContext context) {
    if (context == EGL_NO_CONTEXT) {
        if (before == EGL_NO_CONTEXT) {
            return;  // nothing released now: names already waiting keep waiting
        }
        struct queue *queue = my_queue();
        if (queue == NULL) {
            return;
        }
        size_t dropped = 0;
        pthread_mutex_lock(&lock);
        if (queue->context != before) {
            dropped = queue->total;
            drop(queue);
            queue->context = before;
        }
        pthread_mutex_unlock(&lock);
        say_dropped(dropped, "another context released");
        released = 1;
        return;
    }
    if (!released) {
        return;
    }
    released = 0;
    pthread_mutex_lock(&lock);
    const EGLContext kept_for = mine->context;
    pthread_mutex_unlock(&lock);
    // A context libEGL made to share kept_for's objects (display.c's backup share) takes the shared kinds.
    const int shares = kept_for != EGL_NO_CONTEXT && kept_for != context && minato_contexts_share(kept_for, context);
    unsigned *names[kinds] = {NULL};
    size_t counts[kinds] = {0}, total = 0, dropped = 0, own = 0;
    pthread_mutex_lock(&lock);
    if (mine->context == context || (shares && mine->context == kept_for)) {
        for (int kind = 0; kind < kinds; kind++) {
            if (mine->context != context && kind == framebuffers) {
                own += mine->count[kind];  // a framebuffer is its context's own, not the share group's
                continue;
            }
            names[kind] = mine->names[kind];
            counts[kind] = mine->count[kind];
            mine->names[kind] = NULL;
            total += counts[kind];
        }
    } else {
        dropped = mine->total;
    }
    drop(mine);
    pthread_mutex_unlock(&lock);
    say_dropped(dropped, "another context made current");
    say_dropped(own, "framebuffers, made current in another context of the share group");
    for (int kind = 0; kind < kinds; kind++) {
        void (*delete)(int, const unsigned *) = (void (*)(int, const unsigned *))minato_gles_function(
            delete_names[kind]);
        if (delete != NULL && counts[kind] > 0) {
            delete((int)counts[kind], names[kind]);
        }
        free(names[kind]);
    }
    if (total > 0 && kept_for != context) {
        __android_log_print(ANDROID_LOG_INFO, "libEGL", "deletes made with no context current: %zu names deleted "
                            "when the thread made context %p current, which shares context %p's objects", total,
                            context, kept_for);
    } else if (total > 0 && (atomic_exchange(&told, 1) == 0 || minato_trace_on())) {
        __android_log_print(ANDROID_LOG_INFO, "libEGL", "deletes made with no context current: %zu names deleted "
                            "when the thread made context %p current again", total, context);
    }
}

void minato_deferred_release_thread(void) {
    released = 0;
    if (mine != NULL) {
        pthread_mutex_lock(&lock);
        const size_t dropped = mine->total;
        drop(mine);
        pthread_mutex_unlock(&lock);
        say_dropped(dropped, "eglReleaseThread");
    }
}

EGLContext minato_deferred_released(void) {
    if (!released || mine == NULL) {
        return EGL_NO_CONTEXT;
    }
    pthread_mutex_lock(&lock);
    const EGLContext context = mine->context;
    pthread_mutex_unlock(&lock);
    return context;
}

void minato_deferred_destroyed(EGLContext context) {
    size_t dropped = 0;
    pthread_mutex_lock(&lock);
    for (struct queue *queue = queues; queue != NULL; queue = queue->next) {
        if (context == EGL_NO_CONTEXT || queue->context == context) {
            dropped += queue->total;
            drop(queue);
        }
    }
    pthread_mutex_unlock(&lock);
    say_dropped(dropped, context == EGL_NO_CONTEXT ? "eglTerminate" : "eglDestroyContext");
}
