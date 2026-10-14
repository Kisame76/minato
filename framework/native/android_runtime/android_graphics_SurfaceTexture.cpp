// Ours in place of AOSP's core/jni/android_graphics_SurfaceTexture.cpp (ADR-0058 c)): android.graphics.SurfaceTexture's
// natives on libnativewindow's buffer queue (APKRuntime/buffer_queue.h) instead of libgui's BufferQueue and GLConsumer.
// The method table, its signatures and the fields mSurfaceTexture, mProducer and mFrameAvailableListener are the tag's:
// - mSurfaceTexture is our consumer (a Consumer), mProducer the queue window, which Surface(SurfaceTexture) takes
//   (android_view_Surface.cpp), and mFrameAvailableListener the consumer as well, as the tag keeps its listener there.
// - Each queued frame calls the static postEventFromNative(WeakReference) on the producer's thread, attached to the VM
//   for the call when it is not, which posts to the listener's Handler, the creator's Looper by default. The listener's
//   references live as long as the queue (its destroy), so a producer that still queues after the finalizer calls
//   nothing freed.
// - updateTexImage acquires the queued frame (the newest) and binds an EGL image of its AHardwareBuffer (EGL_ANDROID's
//   image of b), one per slot while its buffer stays) to the texture as GL_TEXTURE_EXTERNAL_OES. Its acquire lets the
//   last frame's slot go to the producer, so glFinish comes first: GLConsumer's release fence, which ANGLE's Metal
//   display cannot give (no EGL_ANDROID_native_fence_sync), a named cost. Without a new frame it binds the current
//   one again. Detached, as Android 15 answers: OK without a queued frame; with one, the frame is dropped, the
//   texture keeps no image, and INVALID_OPERATION. As GLConsumer's: without the attached context INVALID_OPERATION
//   (IllegalStateException); abandoned, NO_INIT (RuntimeException).
// - getTransformMatrix is GLConsumer's computeTransformMatrix for the frame's transform and no crop, which no
//   producer of ours sets: the flips and the rotation, then the vertical flip of a buffer whose first row is the top.
// - detachFromGLContext deletes the texture and leaves the images, which belong to the display; attachToGLContext
//   binds the current frame to the new texture in the current context.
// - release abandons the queue; the producer's calls then answer NO_INIT.
// The EGL images live until the SurfaceTexture's finalizer, which destroys them on their display.
#define LOG_TAG "SurfaceTexture"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <pthread.h>
#include <string.h>

#include <atomic>

#include <APKRuntime/buffer_queue.h>
#include <log/log.h>

#include "core_jni_helpers.h"
#include "minato_gles.h"

namespace android {
namespace {

const char *const kClassPathName = "android/graphics/SurfaceTexture";
const char *const kIllegalStateException = "java/lang/IllegalStateException";
const char *const kOutOfResourcesException = "android/view/Surface$OutOfResourcesException";

// libgui's status_t answers (utils/Errors.h).
constexpr jint kOk = 0;
constexpr jint kInvalidOperation = -ENOSYS;
constexpr jint kNoInit = -ENODEV;

struct {
    jfieldID surfaceTexture;
    jfieldID producer;
    jfieldID frameAvailableListener;
    jmethodID postEvent;
} gFields;

// libnativewindow's private functions (APKRT_PRIVATE), looked up once, as the NDK's stub lacks them.
struct Queue {
    decltype(&minato_BufferQueue_create) create = nullptr;
    decltype(&minato_BufferQueue_setDefaultSize) setDefaultSize = nullptr;
    decltype(&minato_BufferQueue_acquire) acquire = nullptr;
    decltype(&minato_BufferQueue_releaseSlot) releaseSlot = nullptr;
    decltype(&minato_BufferQueue_abandon) abandon = nullptr;
    decltype(&minato_BufferQueue_isAbandoned) isAbandoned = nullptr;
    decltype(&minato_BufferQueue_release) release = nullptr;
};

const Queue &queue_functions() {
    static const Queue functions = [] {
        Queue found;
        void *library = dlopen("libnativewindow.so", RTLD_NOW);
        if (library != nullptr) {
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
            FIND(create, minato_BufferQueue_create);
            FIND(setDefaultSize, minato_BufferQueue_setDefaultSize);
            FIND(acquire, minato_BufferQueue_acquire);
            FIND(releaseSlot, minato_BufferQueue_releaseSlot);
            FIND(abandon, minato_BufferQueue_abandon);
            FIND(isAbandoned, minato_BufferQueue_isAbandoned);
            FIND(release, minato_BufferQueue_release);
#undef FIND
        }
        return found;
    }();
    return functions;
}

bool queue_found() {
    const Queue &queue = queue_functions();
    return queue.create != nullptr && queue.setDefaultSize != nullptr && queue.acquire != nullptr &&
           queue.releaseSlot != nullptr && queue.abandon != nullptr && queue.isAbandoned != nullptr &&
           queue.release != nullptr;
}

// An EGL image of a slot's buffer on a display, with a reference of the buffer, so that a new buffer at a freed one's
// address is never taken for it (an ImageWriter's attached buffers change in a slot at each frame).
struct Image {
    AHardwareBuffer *buffer = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
};

// The frame-available listener's references, the queue's context until its destroy.
struct Listener {
    JavaVM *vm = nullptr;
    jclass clazz = nullptr;  // global
    jobject weak = nullptr;  // global, the WeakReference of postEventFromNative
};

// The consumer: the queue's, the texture's and the current frame's state, under its lock. The Java SurfaceTexture
// holds one reference, and each ASurfaceTexture one more, as libgui's sp<SurfaceTexture> counts them; the last one
// lets the queue go.
struct Consumer {
    std::atomic<int> references{1};
    minato_BufferQueue *queue = nullptr;
    ANativeWindow *producer = nullptr;  // the queue window, with a reference of the consumer's own
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    bool attached = false;
    GLuint texture = 0;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    bool current = false;  // a frame acquired
    minato_BufferQueue_frame frame{};
    Image images[MINATO_BUFFER_QUEUE_MAX_SLOTS];  // a slot's index is below it, also for an ImageWriter's slots
};

Consumer *consumer_of(JNIEnv *env, jobject thiz) {
    return reinterpret_cast<Consumer *>(env->GetLongField(thiz, gFields.surfaceTexture));
}

// The calling thread's JNIEnv, attached for the call when it is not (`attached`); null when it cannot attach.
JNIEnv *env_of(JavaVM *vm, bool *attached) {
    JNIEnv *env = nullptr;
    *attached = false;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    *attached = true;
    return env;
}

// postEventFromNative on the producer's thread.
void frame_available(void *context) {
    auto *listener = static_cast<Listener *>(context);
    bool attached;
    JNIEnv *env = env_of(listener->vm, &attached);
    if (env == nullptr) return;
    env->CallStaticVoidMethod(listener->clazz, gFields.postEvent, listener->weak);
    if (env->ExceptionCheck()) {
        ALOGE("An exception was thrown by an onFrameAvailable listener");
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) listener->vm->DetachCurrentThread();
}

// The queue's end, after both the producer window and the consumer let it go.
void listener_destroyed(void *context) {
    auto *listener = static_cast<Listener *>(context);
    bool attached;
    JNIEnv *env = env_of(listener->vm, &attached);
    if (env != nullptr) {
        env->DeleteGlobalRef(listener->clazz);
        env->DeleteGlobalRef(listener->weak);
        if (attached) listener->vm->DetachCurrentThread();
    }
    delete listener;
}

// The image of the current frame's slot on the current display: the slot's, made again for a new buffer or display.
void forget_image(Image &image) {
    if (image.image != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(image.display, image.image);
    if (image.buffer != nullptr) AHardwareBuffer_release(image.buffer);
    image = Image{};
}

EGLImageKHR image_of(Consumer *consumer, EGLDisplay display) {
    if (consumer->frame.slot < 0 || consumer->frame.slot >= MINATO_BUFFER_QUEUE_MAX_SLOTS) return EGL_NO_IMAGE_KHR;
    Image &image = consumer->images[consumer->frame.slot];
    if (image.image != EGL_NO_IMAGE_KHR && (image.buffer != consumer->frame.buffer || image.display != display)) {
        forget_image(image);
    }
    if (image.image == EGL_NO_IMAGE_KHR) {
        static const EGLint kAttributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        image.image = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                        eglGetNativeClientBufferANDROID(consumer->frame.buffer), kAttributes);
        if (image.image == EGL_NO_IMAGE_KHR) {
            ALOGE("bindTextureImage: eglCreateImageKHR failed (%#x)", eglGetError());
            return EGL_NO_IMAGE_KHR;
        }
        image.buffer = consumer->frame.buffer;
        AHardwareBuffer_acquire(image.buffer);
        image.display = display;
    }
    return image.image;
}

// Binds the current frame, if any, to the texture in the current context.
jint bind_current(Consumer *consumer) {
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, consumer->texture);
    if (!consumer->current) return kOk;
    const EGLImageKHR image = image_of(consumer, consumer->display);
    if (image == EGL_NO_IMAGE_KHR) return -EINVAL;
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image));
    return kOk;
}

// GLConsumer's check of the context: the one it is attached to, current on this thread.
jint check_context(Consumer *consumer) {
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLContext context = eglGetCurrentContext();
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
        ALOGE("checkAndUpdateEglState: invalid current EGLDisplay or EGLContext");
        return kInvalidOperation;
    }
    if (consumer->display == EGL_NO_DISPLAY) {
        consumer->display = display;
        consumer->context = context;
    } else if (consumer->display != display || consumer->context != context) {
        ALOGE("checkAndUpdateEglState: invalid current EGLContext");
        return kInvalidOperation;
    }
    return kOk;
}

void unref(Consumer *consumer) {
    if (consumer->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    for (Image &image : consumer->images) forget_image(image);
    // Abandoned and let go: the listener goes with the queue's last reference, a Surface's window's if it lives.
    queue_functions().release(consumer->queue);
    ANativeWindow_release(consumer->producer);
    delete consumer;
}

jint update(Consumer *consumer) {
    if (queue_functions().isAbandoned(consumer->queue)) {
        ALOGE("updateTexImage: SurfaceTexture is abandoned!");
        return kNoInit;
    }
    if (!consumer->attached) {
        // As Android 15 answers (surface-texture-test): without a queued frame OK; with one, the frame is taken and
        // dropped, the texture keeps no image, and the answer is INVALID_OPERATION.
        minato_BufferQueue_frame dropped{};
        const int taken = queue_functions().acquire(consumer->queue, &dropped);
        if (taken == -EAGAIN) return kOk;
        if (taken != 0) return taken == -ENODEV ? kNoInit : taken;
        queue_functions().releaseSlot(consumer->queue, dropped.slot);
        consumer->current = false;
        ALOGE("updateTexImage: SurfaceTexture is not attached to an OpenGL ES context");
        return kInvalidOperation;
    }
    const jint checked = check_context(consumer);
    if (checked != kOk) return checked;
    // An acquire lets the last frame's slot go to the producer: the GPU's reads of it end first.
    if (consumer->current) glFinish();
    minato_BufferQueue_frame next{};
    const int acquired = queue_functions().acquire(consumer->queue, &next);
    if (acquired == -EAGAIN) return bind_current(consumer);
    if (acquired != 0) return acquired == -ENODEV ? kNoInit : acquired;
    consumer->frame = next;
    consumer->current = true;
    return bind_current(consumer);
}

// GLConsumer's computeTransformMatrix without a crop: column-major 4 x 4 matrices.
void multiply(const float a[16], const float b[16], float out[16]) {
    float result[16];
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[column * 4 + k];
            result[column * 4 + row] = sum;
        }
    }
    memcpy(out, result, sizeof result);
}

void transform_matrix(int32_t transform, float out[16]) {
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const float kFlipH[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1};
    static const float kFlipV[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
    static const float kRot90[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1};
    float matrix[16];
    memcpy(matrix, kIdentity, sizeof matrix);
    if (transform & ANATIVEWINDOW_TRANSFORM_MIRROR_HORIZONTAL) multiply(matrix, kFlipH, matrix);
    if (transform & ANATIVEWINDOW_TRANSFORM_MIRROR_VERTICAL) multiply(matrix, kFlipV, matrix);
    if (transform & ANATIVEWINDOW_TRANSFORM_ROTATE_90) multiply(matrix, kRot90, matrix);
    multiply(kFlipV, matrix, out);
}

jint update_locked(Consumer *consumer) {
    pthread_mutex_lock(&consumer->lock);
    const jint err = update(consumer);
    pthread_mutex_unlock(&consumer->lock);
    return err;
}

jint detach(Consumer *consumer) {
    pthread_mutex_lock(&consumer->lock);
    jint err = kOk;
    if (queue_functions().isAbandoned(consumer->queue)) {
        ALOGE("detachFromContext: abandoned SurfaceTexture");
        err = kNoInit;
    } else if (!consumer->attached) {
        ALOGE("detachFromContext: SurfaceTexture is not attached to a context");
        err = kInvalidOperation;
    } else if ((err = check_context(consumer)) == kOk) {
        glDeleteTextures(1, &consumer->texture);
        consumer->texture = 0;
        consumer->attached = false;
        consumer->display = EGL_NO_DISPLAY;
        consumer->context = EGL_NO_CONTEXT;
    }
    pthread_mutex_unlock(&consumer->lock);
    return err;
}

jint attach(Consumer *consumer, GLuint texture) {
    pthread_mutex_lock(&consumer->lock);
    jint err = kOk;
    if (queue_functions().isAbandoned(consumer->queue)) {
        ALOGE("attachToContext: abandoned SurfaceTexture");
        err = kNoInit;
    } else if (consumer->attached) {
        ALOGE("attachToContext: SurfaceTexture is already attached to a context");
        err = kInvalidOperation;
    } else if ((err = check_context(consumer)) == kOk) {
        consumer->texture = texture;
        consumer->attached = true;
        err = bind_current(consumer);
    }
    pthread_mutex_unlock(&consumer->lock);
    return err;
}

void matrix_of(Consumer *consumer, float matrix[16]) {
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(matrix, kIdentity, sizeof kIdentity);
    pthread_mutex_lock(&consumer->lock);
    if (consumer->current) transform_matrix(consumer->frame.transform, matrix);
    pthread_mutex_unlock(&consumer->lock);
}

int64_t timestamp_of(Consumer *consumer) {
    pthread_mutex_lock(&consumer->lock);
    const int64_t timestamp = consumer->current ? consumer->frame.timestamp : 0;
    pthread_mutex_unlock(&consumer->lock);
    return timestamp;
}

void SurfaceTexture_init(JNIEnv *env, jobject thiz, jboolean isDetached, jint texName, jboolean, jobject weakThiz) {
    if (!queue_found()) {
        jniThrowException(env, kOutOfResourcesException, "Unable to create native SurfaceTexture");
        return;
    }
    auto *listener = new Listener;
    env->GetJavaVM(&listener->vm);
    jclass clazz = env->GetObjectClass(thiz);
    listener->clazz = static_cast<jclass>(env->NewGlobalRef(clazz));
    listener->weak = env->NewGlobalRef(weakThiz);
    auto *consumer = new Consumer;
    consumer->attached = !isDetached;
    consumer->texture = static_cast<GLuint>(texName);
    ANativeWindow *producer = nullptr;
    if (queue_functions().create(frame_available, listener_destroyed, listener, &producer, &consumer->queue) != 0) {
        env->DeleteGlobalRef(listener->clazz);
        env->DeleteGlobalRef(listener->weak);
        delete listener;
        delete consumer;
        jniThrowException(env, kOutOfResourcesException, "Unable to create native SurfaceTexture");
        return;
    }
    consumer->producer = producer;
    ANativeWindow_acquire(producer);  // the consumer's own; the one of create is the Java field's
    env->SetLongField(thiz, gFields.surfaceTexture, reinterpret_cast<jlong>(consumer));
    env->SetLongField(thiz, gFields.producer, reinterpret_cast<jlong>(producer));
    env->SetLongField(thiz, gFields.frameAvailableListener, reinterpret_cast<jlong>(consumer));
}

void SurfaceTexture_finalize(JNIEnv *env, jobject thiz) {
    Consumer *consumer = consumer_of(env, thiz);
    auto *producer = reinterpret_cast<ANativeWindow *>(env->GetLongField(thiz, gFields.producer));
    env->SetLongField(thiz, gFields.surfaceTexture, 0);
    env->SetLongField(thiz, gFields.producer, 0);
    env->SetLongField(thiz, gFields.frameAvailableListener, 0);
    if (consumer != nullptr) unref(consumer);
    if (producer != nullptr) ANativeWindow_release(producer);
}

void SurfaceTexture_setDefaultBufferSize(JNIEnv *env, jobject thiz, jint width, jint height) {
    Consumer *consumer = consumer_of(env, thiz);
    if (consumer != nullptr) queue_functions().setDefaultSize(consumer->queue, width, height);
}

void SurfaceTexture_updateTexImage(JNIEnv *env, jobject thiz) {
    Consumer *consumer = consumer_of(env, thiz);
    if (consumer == nullptr) return;
    const jint err = update_locked(consumer);
    if (err == kInvalidOperation) {
        jniThrowException(env, kIllegalStateException,
                          "Unable to update texture contents (see logcat for details)");
    } else if (err < 0) {
        jniThrowRuntimeException(env, "Error during updateTexImage (see logcat for details)");
    }
}

// GLConsumer's releaseTexImage: the current frame's slot goes back; the texture keeps what it shows.
void SurfaceTexture_releaseTexImage(JNIEnv *env, jobject thiz) {
    Consumer *consumer = consumer_of(env, thiz);
    if (consumer == nullptr) return;
    pthread_mutex_lock(&consumer->lock);
    jint err = kOk;
    if (queue_functions().isAbandoned(consumer->queue)) {
        err = kNoInit;
    } else if (!consumer->attached) {
        err = kInvalidOperation;
    } else if (consumer->current) {
        glFinish();
        queue_functions().releaseSlot(consumer->queue, consumer->frame.slot);
        consumer->current = false;
    }
    pthread_mutex_unlock(&consumer->lock);
    if (err == kInvalidOperation) {
        jniThrowException(env, kIllegalStateException,
                          "Unable to release texture contents (see logcat for details)");
    } else if (err < 0) {
        jniThrowRuntimeException(env, "Error during updateTexImage (see logcat for details)");
    }
}

jint SurfaceTexture_detachFromGLContext(JNIEnv *env, jobject thiz) {
    Consumer *consumer = consumer_of(env, thiz);
    return consumer != nullptr ? detach(consumer) : kNoInit;
}

jint SurfaceTexture_attachToGLContext(JNIEnv *env, jobject thiz, jint tex) {
    Consumer *consumer = consumer_of(env, thiz);
    return consumer != nullptr ? attach(consumer, static_cast<GLuint>(tex)) : kNoInit;
}

void SurfaceTexture_getTransformMatrix(JNIEnv *env, jobject thiz, jfloatArray jmtx) {
    Consumer *consumer = consumer_of(env, thiz);
    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (consumer != nullptr) matrix_of(consumer, matrix);
    env->SetFloatArrayRegion(jmtx, 0, 16, matrix);
}

jlong SurfaceTexture_getTimestamp(JNIEnv *env, jobject thiz) {
    Consumer *consumer = consumer_of(env, thiz);
    return consumer != nullptr ? timestamp_of(consumer) : 0;
}

jint SurfaceTexture_getDataSpace(JNIEnv *, jobject) {
    return 0;  // HAL_DATASPACE_UNKNOWN, as the queue keeps none
}

void SurfaceTexture_release(JNIEnv *env, jobject thiz) {
    Consumer *consumer = consumer_of(env, thiz);
    if (consumer != nullptr) queue_functions().abandon(consumer->queue);
}

jboolean SurfaceTexture_isReleased(JNIEnv *env, jobject thiz) {
    Consumer *consumer = consumer_of(env, thiz);
    return consumer == nullptr || queue_functions().isAbandoned(consumer->queue) ? JNI_TRUE : JNI_FALSE;
}

const JNINativeMethod gSurfaceTextureMethods[] = {
        {"nativeInit", "(ZIZLjava/lang/ref/WeakReference;)V", (void *)SurfaceTexture_init},
        {"nativeFinalize", "()V", (void *)SurfaceTexture_finalize},
        {"nativeSetDefaultBufferSize", "(II)V", (void *)SurfaceTexture_setDefaultBufferSize},
        {"nativeUpdateTexImage", "()V", (void *)SurfaceTexture_updateTexImage},
        {"nativeReleaseTexImage", "()V", (void *)SurfaceTexture_releaseTexImage},
        {"nativeDetachFromGLContext", "()I", (void *)SurfaceTexture_detachFromGLContext},
        {"nativeAttachToGLContext", "(I)I", (void *)SurfaceTexture_attachToGLContext},
        {"nativeGetTransformMatrix", "([F)V", (void *)SurfaceTexture_getTransformMatrix},
        {"nativeGetTimestamp", "()J", (void *)SurfaceTexture_getTimestamp},
        {"nativeGetDataSpace", "()I", (void *)SurfaceTexture_getDataSpace},
        {"nativeRelease", "()V", (void *)SurfaceTexture_release},
        {"nativeIsReleased", "()Z", (void *)SurfaceTexture_isReleased},
};

}  // namespace

int register_android_graphics_SurfaceTexture(JNIEnv *env) {
    jclass clazz = FindClassOrDie(env, kClassPathName);
    gFields.surfaceTexture = GetFieldIDOrDie(env, clazz, "mSurfaceTexture", "J");
    gFields.producer = GetFieldIDOrDie(env, clazz, "mProducer", "J");
    gFields.frameAvailableListener = GetFieldIDOrDie(env, clazz, "mFrameAvailableListener", "J");
    gFields.postEvent = GetStaticMethodIDOrDie(env, clazz, "postEventFromNative", "(Ljava/lang/ref/WeakReference;)V");
    return RegisterMethodsOrDie(env, kClassPathName, gSurfaceTextureMethods, NELEM(gSurfaceTextureMethods));
}

// For Surface(SurfaceTexture) (android_view_Surface.cpp): the SurfaceTexture's producer window, or NULL.
ANativeWindow *android_SurfaceTexture_getProducer(JNIEnv *env, jobject surfaceTexture) {
    return reinterpret_cast<ANativeWindow *>(env->GetLongField(surfaceTexture, gFields.producer));
}

// For libandroid's ASurfaceTexture (runtime/android/surface_texture.cpp, ADR-0058 c)): AOSP's ASurfaceTexture is a
// reference to the native SurfaceTexture and its producer, which outlives the Java object; ours is the consumer with
// a reference of its own. The answers are the consumer's, as libandroid's ASurfaceTexture_* give them.
extern "C" {

void *minato_ASurfaceTexture_fromSurfaceTexture(JNIEnv *env, jobject surfaceTexture) {
    Consumer *consumer = surfaceTexture != nullptr ? consumer_of(env, surfaceTexture) : nullptr;
    if (consumer != nullptr) consumer->references.fetch_add(1, std::memory_order_relaxed);
    return consumer;
}

void minato_ASurfaceTexture_release(void *texture) {
    if (texture != nullptr) unref(static_cast<Consumer *>(texture));
}

ANativeWindow *minato_ASurfaceTexture_acquireANativeWindow(void *texture) {
    ANativeWindow *window = static_cast<Consumer *>(texture)->producer;
    ANativeWindow_acquire(window);
    return window;
}

int minato_ASurfaceTexture_attachToGLContext(void *texture, uint32_t name) {
    return attach(static_cast<Consumer *>(texture), name);
}

int minato_ASurfaceTexture_detachFromGLContext(void *texture) {
    return detach(static_cast<Consumer *>(texture));
}

int minato_ASurfaceTexture_updateTexImage(void *texture) {
    return update_locked(static_cast<Consumer *>(texture));
}

void minato_ASurfaceTexture_getTransformMatrix(void *texture, float matrix[16]) {
    matrix_of(static_cast<Consumer *>(texture), matrix);
}

int64_t minato_ASurfaceTexture_getTimestamp(void *texture) {
    return timestamp_of(static_cast<Consumer *>(texture));
}

}  // extern "C"

ANativeWindow *minato_surface_texture_window(JNIEnv *env, jobject surfaceTexture) {
    ANativeWindow *window = android_SurfaceTexture_getProducer(env, surfaceTexture);
    if (window != nullptr) ANativeWindow_acquire(window);
    return window;
}

// The software layer's frame: the queue's newest after the consumer's last one, kept as the current frame, whose slot
// the next acquire lets go, so the caller copies it before the next latch. No GL reads it here, so no glFinish.
AHardwareBuffer *minato_surface_texture_latch(JNIEnv *env, jobject surfaceTexture, bool *fresh) {
    *fresh = false;
    Consumer *consumer = surfaceTexture != nullptr ? consumer_of(env, surfaceTexture) : nullptr;
    if (consumer == nullptr) return nullptr;
    pthread_mutex_lock(&consumer->lock);
    minato_BufferQueue_frame next{};
    if (queue_functions().acquire(consumer->queue, &next) == 0) {
        consumer->frame = next;
        consumer->current = true;
        *fresh = true;
    }
    AHardwareBuffer *buffer = consumer->current ? consumer->frame.buffer : nullptr;
    if (buffer != nullptr) AHardwareBuffer_acquire(buffer);
    pthread_mutex_unlock(&consumer->lock);
    return buffer;
}

}  // namespace android
