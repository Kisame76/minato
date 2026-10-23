/* libEGL's window surfaces on a queue window (ADR-0058 c), decision 5 as the lead changed it on 2026-10-12): the
   producer side of a SurfaceTexture's buffer queue (APKRuntime/buffer_queue.h), such as Surface(SurfaceTexture) or
   EGL14.eglCreateWindowSurface on a SurfaceTexture.
   - The surface is an ordinary ANGLE pbuffer of the window's size when it is made (its buffer geometry, else the
     queue's default size), so that the app keeps one EGLSurface, as it does on Android.
   - eglSwapBuffers dequeues a slot, blits the pbuffer into the slot's AHardwareBuffer through an EGL image of it
     (EGL_ANDROID_image_native_buffer, b)), flipped, as a window's buffer has its top row first, then glFinish, and
     queues the slot with eglPresentationTimeANDROID's time, else the queue's. One GPU copy per frame is the named
     cost; zero copy is M6's (ADR-0014 parity). The blit is GL_NV_framebuffer_blit's, which both ANGLE backends give
     in ES 2 and ES 3 contexts. What the blit reads and is held back by is set for it and put back afterwards: the draw
     and read framebuffers, the renderbuffer, the scissor test and, in ES 3, the default framebuffer's read buffer and
     rasterizer discard; with another read surface current, the surface is made the read surface for the blit. GL
     errors the app left before the swap are cleared, so that the blit's can be told apart (a named departure).
   - A swap that cannot deliver its frame gives the slot back (cancel) and answers EGL_FALSE: EGL_BAD_NATIVE_WINDOW
     once the queue is abandoned, else EGL_BAD_SURFACE, as for a blit GL refuses (a multisampled or an ES 1 context's
     surface, named limits).
   - A surface's record lives while a swap uses it, so an eglDestroySurface or eglTerminate on another thread frees it
     only after the swap; records are found by display and surface.
   - A window a decoder claims (minato_ANativeWindow_claim) gets no surface, as Android's eglCreateWindowSurface
     answers a window another producer is connected to.
   - Named limit: the pbuffer keeps its size when the window's changes (the slot then gets it scaled); a decoder may
     claim a window that has a surface already, which Android refuses. */
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "APKRuntime/buffer_queue.h"
#include "APKRuntime/guest_interface.h"
#include "libegl.h"


typedef unsigned int GLenum_;
typedef int GLint_;
typedef unsigned int GLuint_;
typedef unsigned char GLboolean_;
#define GL_NO_ERROR 0
#define GL_BACK 0x0405
#define GL_READ_BUFFER 0x0C02
#define GL_SCISSOR_TEST 0x0C11
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_MAJOR_VERSION 0x821B
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601

/* libnativewindow's private producer functions, found once. */
static int (*is_queue)(struct ANativeWindow *);
static int (*dequeue)(struct ANativeWindow *, struct AHardwareBuffer **, int32_t *);
static int (*queue)(struct ANativeWindow *, int32_t, int64_t);
static int (*cancel)(struct ANativeWindow *, int32_t);
static int (*is_claimed)(struct ANativeWindow *);
static int (*connect_producer)(struct ANativeWindow *);
static void (*disconnect_producer)(struct ANativeWindow *);
static int (*window_info)(struct ANativeWindow *, struct minato_window_info *);
static pthread_once_t found = PTHREAD_ONCE_INIT;

static void find(void) {
    void *library = dlopen("libnativewindow.so", RTLD_NOW);
    if (library != NULL) {
        is_queue = (int (*)(struct ANativeWindow *))dlsym(library, "minato_ANativeWindow_isQueue");
        dequeue = (int (*)(struct ANativeWindow *, struct AHardwareBuffer **, int32_t *))dlsym(
            library, "minato_ANativeWindow_dequeue");
        queue = (int (*)(struct ANativeWindow *, int32_t, int64_t))dlsym(library, "minato_ANativeWindow_queue");
        cancel = (int (*)(struct ANativeWindow *, int32_t))dlsym(library, "minato_ANativeWindow_cancel");
        is_claimed = (int (*)(struct ANativeWindow *))dlsym(library, "minato_ANativeWindow_isClaimed");
        connect_producer = (int (*)(struct ANativeWindow *))dlsym(library, "minato_ANativeWindow_connectProducer");
        disconnect_producer =
            (void (*)(struct ANativeWindow *))dlsym(library, "minato_ANativeWindow_disconnectProducer");
        window_info = (int (*)(struct ANativeWindow *, struct minato_window_info *))dlsym(
            library, "minato_ANativeWindow_getInfo");
    }
}

/* GLES's functions the swap uses, through libGLESv2's lookup. */
struct gl {
    GLenum_ (*get_error)(void);
    void (*get_integerv)(GLenum_, GLint_ *);
    GLboolean_ (*is_enabled)(GLenum_);
    void (*enable)(GLenum_);
    void (*disable)(GLenum_);
    void (*read_buffer)(GLenum_);
    void (*gen_renderbuffers)(int, GLuint_ *);
    void (*bind_renderbuffer)(GLenum_, GLuint_);
    void (*delete_renderbuffers)(int, const GLuint_ *);
    void (*image_target_renderbuffer)(GLenum_, void *);
    void (*gen_framebuffers)(int, GLuint_ *);
    void (*bind_framebuffer)(GLenum_, GLuint_);
    void (*delete_framebuffers)(int, const GLuint_ *);
    void (*framebuffer_renderbuffer)(GLenum_, GLenum_, GLenum_, GLuint_);
    void (*blit)(GLint_, GLint_, GLint_, GLint_, GLint_, GLint_, GLint_, GLint_, unsigned, GLenum_);
    void (*finish)(void);
};

static int gl_of(struct gl *gl) {
#define GL(field, name) gl->field = (__typeof__(gl->field))minato_gles_function(name)
    GL(get_error, "glGetError");
    GL(get_integerv, "glGetIntegerv");
    GL(is_enabled, "glIsEnabled");
    GL(enable, "glEnable");
    GL(disable, "glDisable");
    GL(read_buffer, "glReadBuffer");
    GL(gen_renderbuffers, "glGenRenderbuffers");
    GL(bind_renderbuffer, "glBindRenderbuffer");
    GL(delete_renderbuffers, "glDeleteRenderbuffers");
    GL(image_target_renderbuffer, "glEGLImageTargetRenderbufferStorageOES");
    GL(gen_framebuffers, "glGenFramebuffers");
    GL(bind_framebuffer, "glBindFramebuffer");
    GL(delete_framebuffers, "glDeleteFramebuffers");
    GL(framebuffer_renderbuffer, "glFramebufferRenderbuffer");
    GL(blit, "glBlitFramebufferNV");
    GL(finish, "glFinish");
#undef GL
    void *const *each = (void *const *)gl;
    for (size_t index = 0; index < sizeof *gl / sizeof(void *); index++) {
        if (each[index] == NULL) {
            return -1;
        }
    }
    return 0;
}

/* The queue surfaces libEGL made, with their images of the slots' buffers. The list holds one reference of each
   record, and a swap one more while it runs. */
struct queue_surface {
    struct queue_surface *next;
    int references;  // under the lock
    EGLSurface surface;
    EGLDisplay display;
    EGLConfig config;
    struct ANativeWindow *window;
    EGLint width, height;
    int64_t time;  // eglPresentationTimeANDROID's for the next swap, 0 for none; under the lock
    struct {
        struct AHardwareBuffer *buffer;  // with a reference of the image's, so a new buffer at its address is new
        EGLImageKHR image;
    } images[MINATO_BUFFER_QUEUE_MAX_SLOTS];
};
static struct queue_surface *surfaces;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int minato_window_connect(struct ANativeWindow *window) {
    pthread_once(&found, find);
    return connect_producer != NULL ? connect_producer(window) : 0;
}

void minato_window_disconnect(struct ANativeWindow *window) {
    pthread_once(&found, find);
    if (disconnect_producer != NULL) disconnect_producer(window);
}

int minato_queue_window(struct ANativeWindow *window) {
    pthread_once(&found, find);
    return is_queue != NULL && dequeue != NULL && queue != NULL && cancel != NULL && is_claimed != NULL &&
           window_info != NULL && is_queue(window);
}

EGLSurface minato_queue_surface(EGLDisplay display, EGLConfig config, struct ANativeWindow *window) {
    if (is_claimed(window)) {
        minato_bad_native_window(display, config);  /* a decoder's: Android's connect fails, "already connected" */
        return EGL_NO_SURFACE;
    }
    struct minato_window_info info;
    if (window_info(window, &info) != 0) {
        minato_bad_native_window(display, config);
        return EGL_NO_SURFACE;
    }
    const int geometry = info.buffer_width > 0 && info.buffer_height > 0;
    const EGLint width = geometry ? info.buffer_width : info.width;
    const EGLint height = geometry ? info.buffer_height : info.height;
    struct queue_surface *record = calloc(1, sizeof *record);
    if (record == NULL) {
        minato_bad_native_window(display, config);
        return EGL_NO_SURFACE;
    }
    const EGLint size[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    if (surface == EGL_NO_SURFACE) {
        free(record);
        return EGL_NO_SURFACE;
    }
    ANativeWindow_acquire(window);
    *record = (struct queue_surface){NULL, 1, surface, display, config, window, width, height, 0, {{0}}};
    pthread_mutex_lock(&lock);
    record->next = surfaces;
    surfaces = record;
    pthread_mutex_unlock(&lock);
    return surface;
}

static void free_surface(struct queue_surface *record) {
    for (int index = 0; index < MINATO_BUFFER_QUEUE_MAX_SLOTS; index++) {
        if (record->images[index].image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(record->display, record->images[index].image);
        }
        if (record->images[index].buffer != NULL) {
            AHardwareBuffer_release(record->images[index].buffer);
        }
    }
    ANativeWindow_release(record->window);
    free(record);
}

/* Drops a reference of a record; the last frees it. */
static void unref(struct queue_surface *record) {
    pthread_mutex_lock(&lock);
    const int last = --record->references == 0;
    pthread_mutex_unlock(&lock);
    if (last) {
        free_surface(record);
    }
}

/* The record of `surface` on `display` with a reference for the caller, or NULL for another surface. */
static struct queue_surface *referenced(EGLDisplay display, EGLSurface surface) {
    pthread_mutex_lock(&lock);
    struct queue_surface *record = surfaces;
    while (record != NULL && (record->display != display || record->surface != surface)) {
        record = record->next;
    }
    if (record != NULL) {
        record->references++;
    }
    pthread_mutex_unlock(&lock);
    return record;
}

/* Takes the first record of `display` (and `surface`, unless it is EGL_NO_SURFACE) out of the list, NULL for none. */
static struct queue_surface *taken(EGLDisplay display, EGLSurface surface) {
    pthread_mutex_lock(&lock);
    struct queue_surface *record = NULL;
    for (struct queue_surface **entry = &surfaces; *entry != NULL; entry = &(*entry)->next) {
        if ((*entry)->display == display && (surface == EGL_NO_SURFACE || (*entry)->surface == surface)) {
            record = *entry;
            *entry = record->next;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return record;
}

int minato_queue_presentation_time(EGLDisplay display, EGLSurface surface, int64_t time) {
    struct queue_surface *record = referenced(display, surface);
    if (record == NULL) {
        return 0;
    }
    pthread_mutex_lock(&lock);
    record->time = time;
    pthread_mutex_unlock(&lock);
    unref(record);
    return 1;
}

/* The image of a slot's buffer, kept per slot while the buffer stays the slot's. Only the swapping thread, the
   surface's current one, touches the images. */
static EGLImageKHR image_of(struct queue_surface *record, int32_t slot, struct AHardwareBuffer *buffer) {
    if (slot < 0 || slot >= MINATO_BUFFER_QUEUE_MAX_SLOTS) {
        return EGL_NO_IMAGE_KHR;
    }
    if (record->images[slot].buffer != buffer) {
        if (record->images[slot].image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(record->display, record->images[slot].image);
            record->images[slot].image = EGL_NO_IMAGE_KHR;
        }
        if (record->images[slot].buffer != NULL) {
            AHardwareBuffer_release(record->images[slot].buffer);
            record->images[slot].buffer = NULL;
        }
    }
    if (record->images[slot].image == EGL_NO_IMAGE_KHR) {
        static const EGLint preserved[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        record->images[slot].image = eglCreateImageKHR(record->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                                       eglGetNativeClientBufferANDROID(buffer), preserved);
        if (record->images[slot].image != EGL_NO_IMAGE_KHR) {
            record->images[slot].buffer = buffer;
            AHardwareBuffer_acquire(buffer);
        }
    }
    return record->images[slot].image;
}

/* The pbuffer into the buffer of `image` (`width` x `height`), flipped, then glFinish: 0, or the GL error that
   refused it. */
static GLenum_ blit(const struct gl *gl, const struct queue_surface *record, EGLImageKHR image, EGLint width,
                    EGLint height) {
    while (gl->get_error() != GL_NO_ERROR) {
    }
    GLint_ major = 0;
    gl->get_integerv(GL_MAJOR_VERSION, &major);
    const int es3 = gl->get_error() == GL_NO_ERROR && major >= 3;
    GLint_ draw = 0, read = 0, renderbuffer = 0, read_buffer = GL_BACK;
    gl->get_integerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    gl->get_integerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    gl->get_integerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    const GLboolean_ scissor = gl->is_enabled(GL_SCISSOR_TEST);
    const GLboolean_ discard = es3 ? gl->is_enabled(GL_RASTERIZER_DISCARD) : 0;
    GLuint_ target = 0, framebuffer = 0;
    gl->gen_renderbuffers(1, &target);
    gl->bind_renderbuffer(GL_RENDERBUFFER, target);
    gl->image_target_renderbuffer(GL_RENDERBUFFER, image);
    gl->gen_framebuffers(1, &framebuffer);
    gl->bind_framebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    gl->framebuffer_renderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, target);
    gl->bind_framebuffer(GL_READ_FRAMEBUFFER, 0);
    if (es3) {
        gl->get_integerv(GL_READ_BUFFER, &read_buffer);
        gl->read_buffer(GL_BACK);
    }
    if (scissor) {
        gl->disable(GL_SCISSOR_TEST);
    }
    if (discard) {
        gl->disable(GL_RASTERIZER_DISCARD);
    }
    const int scaled = width != record->width || height != record->height;
    gl->blit(0, 0, record->width, record->height, 0, height, width, 0, GL_COLOR_BUFFER_BIT,
             scaled ? GL_LINEAR : GL_NEAREST);
    const GLenum_ refused = gl->get_error();
    gl->finish();
    if (discard) {
        gl->enable(GL_RASTERIZER_DISCARD);
    }
    if (scissor) {
        gl->enable(GL_SCISSOR_TEST);
    }
    if (es3) {
        gl->read_buffer((GLenum_)read_buffer);
    }
    gl->bind_framebuffer(GL_DRAW_FRAMEBUFFER, (GLuint_)draw);
    gl->bind_framebuffer(GL_READ_FRAMEBUFFER, (GLuint_)read);
    gl->bind_renderbuffer(GL_RENDERBUFFER, (GLuint_)renderbuffer);
    gl->delete_framebuffers(1, &framebuffer);
    gl->delete_renderbuffers(1, &target);
    while (gl->get_error() != GL_NO_ERROR) {
    }
    return refused;
}

/* EGL_BAD_SURFACE for the swap, as ANGLE answers a query of no surface. */
static void bad_surface(EGLDisplay display) {
    EGLint ignored;
    eglQuerySurface(display, EGL_NO_SURFACE, EGL_WIDTH, &ignored);
}

static EGLBoolean swap(struct queue_surface *record, const struct gl *gl) {
    struct AHardwareBuffer *buffer = NULL;
    int32_t slot = -1;
    if (dequeue(record->window, &buffer, &slot) != 0) {
        minato_bad_native_window(record->display, record->config);
        return EGL_FALSE;
    }
    AHardwareBuffer_Desc description;
    AHardwareBuffer_describe(buffer, &description);
    const EGLImageKHR image = image_of(record, slot, buffer);
    const EGLContext context = eglGetCurrentContext();
    const EGLSurface read = eglGetCurrentSurface(EGL_READ);
    if (read != record->surface) {
        eglMakeCurrent(record->display, record->surface, record->surface, context);
    }
    const GLenum_ refused = image != EGL_NO_IMAGE_KHR
                                ? blit(gl, record, image, (EGLint)description.width, (EGLint)description.height)
                                : 1;
    if (read != record->surface) {
        eglMakeCurrent(record->display, record->surface, read, context);
    }
    if (refused != GL_NO_ERROR) {
        cancel(record->window, slot);
        bad_surface(record->display);
        return EGL_FALSE;
    }
    pthread_mutex_lock(&lock);
    const int64_t time = record->time;
    record->time = 0;
    pthread_mutex_unlock(&lock);
    if (queue(record->window, slot, time) != 0) {
        minato_bad_native_window(record->display, record->config);
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

int minato_queue_swap(EGLDisplay display, EGLSurface surface, EGLBoolean *result) {
    struct queue_surface *record = referenced(display, surface);
    if (record == NULL) {
        return 0;
    }
    struct gl gl;
    if (eglGetCurrentSurface(EGL_DRAW) != surface || gl_of(&gl) != 0) {
        unref(record);
        return 0;  // not current: the pbuffer's swap, which ANGLE answers
    }
    *result = swap(record, &gl);
    unref(record);
    return 1;
}

void minato_queue_destroyed(EGLDisplay display, EGLSurface surface) {
    struct queue_surface *record = taken(display, surface);
    if (record != NULL) {
        unref(record);
    }
}

void minato_queue_terminated(EGLDisplay display) {
    for (struct queue_surface *record; (record = taken(display, EGL_NO_SURFACE)) != NULL;) {
        unref(record);
    }
}
