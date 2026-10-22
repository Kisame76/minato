// Ours in place of AOSP's media/jni/android_media_ImageReader.cpp (ADR-0058 e1), the graphics breadth's G3a):
// ImageReader's natives on libnativewindow's buffer queue (APKRuntime/buffer_queue.h) instead of libgui's
// BufferItemConsumer, following the tag's answers:
// - nativeInit makes a queue whose consumer holds up to maxImages frames, each until its image's release
//   (minato_BufferQueue_setConsumer), of the reader's size, hardware buffer format (PRIVATE's buffers are RGBA_8888)
//   and usage; mNativeContext is the reader's context. Frames an app's producer queues over one not acquired yet drop,
//   as Android 15's do (image-reader-test). Each frame calls the static postEventFromNative on the producer's thread.
// - nativeImageSetup acquires the oldest frame: ACQUIRE_MAX_IMAGES while maxImages are held, ACQUIRE_NO_BUFS without
//   one; a buffer of another format than the reader's (not PRIVATE) is released and refused with
//   UnsupportedOperationException, as the tag's. The image keeps its frame in mNativeBuffer until its release.
// - An image's planes are its buffer locked for the CPU, one plane for the RGB formats, three for YUV_420_888
//   (lockPlanes' Y, Cb and Cr, ADR-0060 e2a) and none for PRIVATE, as ImageUtils counts them; getHardwareBuffer
//   gives the frame's AHardwareBuffer; there is no fence (-1).
// - A frame whose crop does not start at the origin is refused with UnsupportedOperationException, as the tag's, and
//   stays acquired, its pool item lost, until the reader's close; an image's width and height are its crop's, as the
//   tag's getBufferWidth, and its data space the producer's, else the reader's.
// - nativeDetachImage takes the image's buffer out of the queue (minato_BufferQueue_detach, G3b), for an
//   ImageWriter's queueInputImage, which attaches it; the image keeps its buffer until its close. The static plane
//   natives for detached images log Unwritten's line and throw UnsupportedOperationException.
// - The reader's data space is its queue's default (minato_BufferQueue_setDefaultDataSpace), which a producer reads,
//   as BufferQueue's default buffer data space (JFIF for YUV_420_888); a frame without one gets the reader's.
// Limits while no producer of ours makes them matter: a BLOB reader takes no RGBA_8888 buffer (the tag's JPEG
// workaround) and its plane is the buffer's width in bytes, not the JPEG blob's size.
#define LOG_TAG "ImageReader_JNI"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <ui/PublicFormat.h>

#include <APKRuntime/buffer_queue.h>
#include <log/log.h>

#include "core_jni_helpers.h"
#include "minato_gles.h"

extern "C" jobject minato_ANativeWindow_toSurface(JNIEnv *env, ANativeWindow *window);

namespace android {

jobject android_hardware_HardwareBuffer_createFromAHardwareBuffer(JNIEnv *env, AHardwareBuffer *hardwareBuffer);

namespace {

// ImageReader's ACQUIRE_* answers.
constexpr jint kAcquireSuccess = 0;
constexpr jint kAcquireNoBuffers = 1;
constexpr jint kAcquireMaxImages = 2;
constexpr int32_t kHalPrivate = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;

struct {
    jfieldID nativeContext;    // ImageReader.mNativeContext
    jmethodID postEvent;       // ImageReader.postEventFromNative
    jfieldID nativeBuffer;     // SurfaceImage.mNativeBuffer
    jfieldID timestamp;        // SurfaceImage.mTimestamp
    jfieldID transform;        // SurfaceImage.mTransform
    jfieldID scalingMode;      // SurfaceImage.mScalingMode
    jfieldID dataSpace;        // SurfaceImage.mDataSpace
    jclass planeClass;         // SurfaceImage.SurfacePlane, global
    jmethodID planeConstructor;
} gFields;

// libnativewindow's private functions (APKRT_PRIVATE), looked up once.
struct Queue {
    decltype(&minato_BufferQueue_create) create = nullptr;
    decltype(&minato_BufferQueue_setDefaultSize) setDefaultSize = nullptr;
    decltype(&minato_BufferQueue_setDefaultFormat) setDefaultFormat = nullptr;
    decltype(&minato_BufferQueue_setDefaultDataSpace) setDefaultDataSpace = nullptr;
    decltype(&minato_BufferQueue_setConsumer) setConsumer = nullptr;
    decltype(&minato_BufferQueue_acquire) acquire = nullptr;
    decltype(&minato_BufferQueue_releaseSlot) releaseSlot = nullptr;
    decltype(&minato_BufferQueue_discardFreeBuffers) discardFreeBuffers = nullptr;
    decltype(&minato_BufferQueue_detach) detach = nullptr;
    decltype(&minato_BufferQueue_release) release = nullptr;
};

const Queue &queue_functions() {
    static const Queue functions = [] {
        Queue found;
        if (void *library = dlopen("libnativewindow.so", RTLD_NOW)) {
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
            FIND(create, minato_BufferQueue_create);
            FIND(setDefaultSize, minato_BufferQueue_setDefaultSize);
            FIND(setDefaultFormat, minato_BufferQueue_setDefaultFormat);
            FIND(setDefaultDataSpace, minato_BufferQueue_setDefaultDataSpace);
            FIND(setConsumer, minato_BufferQueue_setConsumer);
            FIND(acquire, minato_BufferQueue_acquire);
            FIND(releaseSlot, minato_BufferQueue_releaseSlot);
            FIND(discardFreeBuffers, minato_BufferQueue_discardFreeBuffers);
            FIND(detach, minato_BufferQueue_detach);
            FIND(release, minato_BufferQueue_release);
#undef FIND
        }
        return found;
    }();
    return functions;
}

bool queue_found() {
    const Queue &queue = queue_functions();
    return queue.create != nullptr && queue.setDefaultSize != nullptr && queue.setDefaultFormat != nullptr &&
           queue.setDefaultDataSpace != nullptr &&
           queue.setConsumer != nullptr && queue.acquire != nullptr && queue.releaseSlot != nullptr &&
           queue.discardFreeBuffers != nullptr && queue.detach != nullptr && queue.release != nullptr;
}

// The frame-available listener's references, the queue's context until its destroy.
struct Listener {
    JavaVM *vm = nullptr;
    jclass clazz = nullptr;  // global, ImageReader
    jobject weak = nullptr;  // global, the WeakReference of postEventFromNative
};

struct Reader {
    minato_BufferQueue *queue = nullptr;
    ANativeWindow *producer = nullptr;
    int32_t format = 0;  // the hardware buffer format the reader asked for
    int32_t dataSpace = 0;
    int32_t maxImages = 0;
    int32_t acquired = 0;
};

// An image's frame: its slot (-1 once detached), its buffer with a reference of the image's own, its crop and its CPU
// lock.
struct Frame {
    minato_BufferQueue *queue = nullptr;
    int32_t slot = -1;
    AHardwareBuffer *buffer = nullptr;
    int32_t cropWidth = 0, cropHeight = 0;  // 0 without a crop
    void *locked = nullptr;
    AHardwareBuffer_Planes planes{};  // a YUV buffer's, while locked
};

JNIEnv *env_of(JavaVM *vm, bool *attached) {
    JNIEnv *env = nullptr;
    *attached = false;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    *attached = true;
    return env;
}

void frame_available(void *context) {
    auto *listener = static_cast<Listener *>(context);
    bool attached;
    JNIEnv *env = env_of(listener->vm, &attached);
    if (env == nullptr) {
        ALOGW("onFrameAvailable event will not posted");
        return;
    }
    env->CallStaticVoidMethod(listener->clazz, gFields.postEvent, listener->weak);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) listener->vm->DetachCurrentThread();
}

void listener_destroyed(void *context) {
    auto *listener = static_cast<Listener *>(context);
    bool attached;
    if (JNIEnv *env = env_of(listener->vm, &attached)) {
        env->DeleteGlobalRef(listener->clazz);
        env->DeleteGlobalRef(listener->weak);
        if (attached) listener->vm->DetachCurrentThread();
    }
    delete listener;
}

Reader *reader_of(JNIEnv *env, jobject thiz) {
    return reinterpret_cast<Reader *>(env->GetLongField(thiz, gFields.nativeContext));
}

Frame *frame_of(JNIEnv *env, jobject image) {
    return reinterpret_cast<Frame *>(env->GetLongField(image, gFields.nativeBuffer));
}

AHardwareBuffer_Desc describe(const Frame *frame) {
    AHardwareBuffer_Desc description{};
    AHardwareBuffer_describe(frame->buffer, &description);
    return description;
}

void ImageReader_classInit(JNIEnv *env, jclass clazz) {
    gFields.nativeContext = GetFieldIDOrDie(env, clazz, "mNativeContext", "J");
    gFields.postEvent = GetStaticMethodIDOrDie(env, clazz, "postEventFromNative", "(Ljava/lang/Object;)V");
    jclass image = FindClassOrDie(env, "android/media/ImageReader$SurfaceImage");
    gFields.nativeBuffer = GetFieldIDOrDie(env, image, "mNativeBuffer", "J");
    gFields.timestamp = GetFieldIDOrDie(env, image, "mTimestamp", "J");
    gFields.transform = GetFieldIDOrDie(env, image, "mTransform", "I");
    gFields.scalingMode = GetFieldIDOrDie(env, image, "mScalingMode", "I");
    gFields.dataSpace = GetFieldIDOrDie(env, image, "mDataSpace", "I");
    jclass plane = FindClassOrDie(env, "android/media/ImageReader$SurfaceImage$SurfacePlane");
    gFields.planeClass = MakeGlobalRefOrDie(env, plane);
    gFields.planeConstructor = GetMethodIDOrDie(env, plane, "<init>",
                                                "(Landroid/media/ImageReader$SurfaceImage;IILjava/nio/ByteBuffer;)V");
}

void ImageReader_init(JNIEnv *env, jobject thiz, jobject weakThiz, jint width, jint height, jint maxImages,
                      jlong usage, jint hardwareBufferFormat, jint dataSpace) {
    if (!queue_found()) {
        jniThrowRuntimeException(env, "Failed to allocate native buffer queue");
        return;
    }
    auto *listener = new Listener;
    env->GetJavaVM(&listener->vm);
    listener->clazz = static_cast<jclass>(env->NewGlobalRef(env->GetObjectClass(thiz)));
    listener->weak = env->NewGlobalRef(weakThiz);
    auto *reader = new Reader;
    reader->format = hardwareBufferFormat;
    reader->dataSpace = dataSpace;
    reader->maxImages = maxImages;
    const Queue &queue = queue_functions();
    if (queue.create(frame_available, listener_destroyed, listener, &reader->producer, &reader->queue) != 0) {
        env->DeleteGlobalRef(listener->clazz);
        env->DeleteGlobalRef(listener->weak);
        delete listener;
        delete reader;
        jniThrowRuntimeException(env, "Failed to allocate native buffer queue");
        return;
    }
    if (queue.setConsumer(reader->queue, maxImages, static_cast<uint64_t>(usage)) != 0) {
        queue.release(reader->queue);
        ANativeWindow_release(reader->producer);
        delete reader;
        jniThrowExceptionFmt(env, "java/lang/IllegalStateException",
                             "Failed to set buffer consumer max acquired buffer count %d", maxImages);
        return;
    }
    queue.setDefaultSize(reader->queue, width, height);
    // IMPLEMENTATION_DEFINED for PRIVATE, which the queue makes as RGBA_8888.
    queue.setDefaultFormat(reader->queue, hardwareBufferFormat);
    queue.setDefaultDataSpace(reader->queue, dataSpace);
    env->SetLongField(thiz, gFields.nativeContext, reinterpret_cast<jlong>(reader));
}

void ImageReader_close(JNIEnv *env, jobject thiz) {
    Reader *reader = reader_of(env, thiz);
    if (reader == nullptr) return;
    env->SetLongField(thiz, gFields.nativeContext, 0);
    // The queue is abandoned; its images' frames release their slots into it until they go.
    queue_functions().release(reader->queue);
    ANativeWindow_release(reader->producer);
    delete reader;
}

// The image's frame back to the queue, unlocked; its own reference of the queue ends with it.
void release_frame(JNIEnv *env, jobject image) {
    Frame *frame = frame_of(env, image);
    if (frame == nullptr) return;
    env->SetLongField(image, gFields.nativeBuffer, 0);
    if (frame->locked != nullptr) AHardwareBuffer_unlock(frame->buffer, nullptr);
    if (frame->slot >= 0) queue_functions().releaseSlot(frame->queue, frame->slot);
    AHardwareBuffer_release(frame->buffer);
    delete frame;
}

void ImageReader_imageRelease(JNIEnv *env, jobject thiz, jobject image) {
    Reader *reader = reader_of(env, thiz);
    // Under the image's monitor, which its plane and buffer natives hold, so none reads a frame being deleted.
    if (env->MonitorEnter(image) != JNI_OK) return;
    // A detached frame's pool item, too, comes back only now, as the tag's.
    const bool held = frame_of(env, image) != nullptr;
    if (held) release_frame(env, image);
    env->MonitorExit(image);
    if (held && reader != nullptr && reader->acquired > 0) reader->acquired--;
}

jint ImageReader_imageSetup(JNIEnv *env, jobject thiz, jobject image) {
    Reader *reader = reader_of(env, thiz);
    if (reader == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException",
                          "ImageReader is not initialized or was already closed");
        return -1;
    }
    // As the tag's pool of maxImages buffer items.
    if (reader->acquired >= reader->maxImages) return kAcquireMaxImages;
    minato_BufferQueue_frame acquired{};
    const int result = queue_functions().acquire(reader->queue, &acquired);
    if (result == -EAGAIN || result == -ENODEV) return kAcquireNoBuffers;
    if (result == -EBUSY) return kAcquireMaxImages;
    if (result != 0) {
        jniThrowExceptionFmt(env, "java/lang/IllegalStateException", "Unknown error (%d) when acquiring buffer",
                             result);
        return -1;
    }
    // As the tag's: the item stays acquired and out of the pool, so the reader holds one image fewer from now on.
    if (reader->format != kHalPrivate && (acquired.crop_left != 0 || acquired.crop_top != 0)) {
        reader->acquired++;
        jniThrowExceptionFmt(env, "java/lang/UnsupportedOperationException",
                             "crop left top corner [%d, %d] need to be at origin", acquired.crop_left,
                             acquired.crop_top);
        return -1;
    }
    auto *frame = new Frame;
    frame->queue = reader->queue;
    frame->slot = acquired.slot;
    frame->buffer = acquired.buffer;
    if (acquired.crop_right > acquired.crop_left && acquired.crop_bottom > acquired.crop_top) {
        frame->cropWidth = acquired.crop_right - acquired.crop_left;
        frame->cropHeight = acquired.crop_bottom - acquired.crop_top;
    }
    AHardwareBuffer_acquire(frame->buffer);
    const AHardwareBuffer_Desc description = describe(frame);
    if (reader->format != kHalPrivate && static_cast<int32_t>(description.format) != reader->format) {
        queue_functions().releaseSlot(frame->queue, frame->slot);
        AHardwareBuffer_release(frame->buffer);
        delete frame;
        jniThrowExceptionFmt(env, "java/lang/UnsupportedOperationException",
                             "The producer output buffer format 0x%x doesn't match the ImageReader's configured buffer "
                             "format 0x%x.",
                             description.format, reader->format);
        return -1;
    }
    env->SetLongField(image, gFields.nativeBuffer, reinterpret_cast<jlong>(frame));
    env->SetLongField(image, gFields.timestamp, acquired.timestamp);
    env->SetIntField(image, gFields.transform, acquired.transform);
    env->SetIntField(image, gFields.scalingMode, 0);  // NATIVE_WINDOW_SCALING_MODE_FREEZE, the queue's only one
    env->SetIntField(image, gFields.dataSpace, acquired.data_space != 0 ? acquired.data_space : reader->dataSpace);
    reader->acquired++;
    return kAcquireSuccess;
}

jobject ImageReader_getSurface(JNIEnv *env, jobject thiz) {
    Reader *reader = reader_of(env, thiz);
    if (reader == nullptr) {
        jniThrowRuntimeException(env, "ImageReaderContext is not initialized");
        return nullptr;
    }
    return minato_ANativeWindow_toSurface(env, reader->producer);
}

jint ImageReader_detachImage(JNIEnv *env, jobject thiz, jobject image, jboolean throwISEOnly) {
    Reader *reader = reader_of(env, thiz);
    if (reader == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Image was released");
        return -1;
    }
    if (env->MonitorEnter(image) != JNI_OK) return -1;
    Frame *frame = frame_of(env, image);
    const int result = frame != nullptr && frame->slot >= 0 ? queue_functions().detach(frame->queue, frame->slot)
                                                           : -EINVAL;
    if (result == 0) {
        if (frame->locked != nullptr) AHardwareBuffer_unlock(frame->buffer, nullptr);
        frame->locked = nullptr;
        frame->slot = -1;
        frame->queue = nullptr;
    }
    env->MonitorExit(image);
    if (frame == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Image is not initialized");
        return -1;
    }
    if (result != 0) {
        jniThrowException(env, throwISEOnly ? "java/lang/IllegalStateException" : "java/lang/RuntimeException",
                          "nativeDetachImage: unable to detach buffer");
        return -1;
    }
    return 0;
}

void ImageReader_discardFreeBuffers(JNIEnv *env, jobject thiz) {
    if (Reader *reader = reader_of(env, thiz)) queue_functions().discardFreeBuffers(reader->queue);
}

jobjectArray ImageReader_createImagePlanes(JNIEnv *env, jclass, jint, jobject, jint, jint, jint, jint, jint, jint) {
    minato_unwritten(env, "android.media.ImageReader.nativeCreateImagePlanes");
    jniThrowException(env, "java/lang/UnsupportedOperationException", "Planes of a detached image are not supported");
    return nullptr;
}

void ImageReader_unlockGraphicBuffer(JNIEnv *env, jclass, jobject) {
    minato_unwritten(env, "android.media.ImageReader.nativeUnlockGraphicBuffer");
}

// Y8Cb8Cr8_420's three planes (ADR-0060 e2a), locked for the CPU by lockPlanes at the first call, which the frame
// keeps until its unlock: Y, then Cb and Cr, each with its strides and a buffer from its first sample to its last.
bool yuv_planes(JNIEnv *env, jobject thiz, Frame *frame, const AHardwareBuffer_Desc &description, uint64_t lockUsage,
                jobjectArray planes) {
    if (frame->locked == nullptr) {
        AHardwareBuffer_Planes locked{};
        if (AHardwareBuffer_lockPlanes(frame->buffer, lockUsage, -1, nullptr, &locked) != 0) return false;
        if (locked.planeCount != 3) {
            AHardwareBuffer_unlock(frame->buffer, nullptr);
            return false;
        }
        frame->planes = locked;
        frame->locked = locked.planes[0].data;
    }
    for (uint32_t index = 0; index < 3; index++) {
        const AHardwareBuffer_Plane &plane = frame->planes.planes[index];
        const jlong width = index == 0 ? description.width : (description.width + 1) / 2;
        const jlong height = index == 0 ? description.height : (description.height + 1) / 2;
        const jlong size = static_cast<jlong>(plane.rowStride) * (height - 1) +
                           static_cast<jlong>(plane.pixelStride) * (width - 1) + 1;
        jobject bytes = env->NewDirectByteBuffer(plane.data, size);
        if (bytes == nullptr) return false;
        jobject object = env->NewObject(gFields.planeClass, gFields.planeConstructor, thiz,
                                        static_cast<jint>(plane.rowStride), static_cast<jint>(plane.pixelStride),
                                        bytes);
        if (object == nullptr) return false;
        env->SetObjectArrayElement(planes, static_cast<jsize>(index), object);
    }
    return true;
}

// The planes of the image's buffer, locked for the CPU at the first call: one for an RGB format, three for
// YUV_420_888, none for PRIVATE.
jobjectArray Image_createSurfacePlanes(JNIEnv *env, jobject thiz, jint numPlanes, jint, jlong readerUsage) {
    jobjectArray planes = env->NewObjectArray(numPlanes, gFields.planeClass, nullptr);
    if (planes == nullptr || numPlanes == 0) return planes;
    Frame *frame = frame_of(env, thiz);
    if (frame == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Image was released");
        return nullptr;
    }
    const AHardwareBuffer_Desc description = describe(frame);
    if (description.format == AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 && numPlanes == 3) {
        const uint64_t writes = static_cast<uint64_t>(readerUsage) & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK;
        if (!yuv_planes(env, thiz, frame, description, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | writes, planes)) {
            if (!env->ExceptionCheck()) {
                jniThrowException(env, "java/lang/IllegalStateException", "Failed to lock buffer");
            }
            return nullptr;
        }
        return planes;
    }
    int32_t pixelStride;
    switch (description.format) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
            pixelStride = 4;
            break;
        case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
            pixelStride = 3;
            break;
        case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
            pixelStride = 2;
            break;
        case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
            pixelStride = 8;
            break;
        case AHARDWAREBUFFER_FORMAT_BLOB:
        case AHARDWAREBUFFER_FORMAT_R8_UNORM:
            pixelStride = 1;
            break;
        default:
            jniThrowExceptionFmt(env, "java/lang/UnsupportedOperationException",
                                 "Format 0x%x is not supported", description.format);
            return nullptr;
    }
    if (numPlanes != 1) {
        jniThrowExceptionFmt(env, "java/lang/UnsupportedOperationException",
                             "%d planes of format 0x%x are not supported", numPlanes, description.format);
        return nullptr;
    }
    // As the tag's: for reading, and for writing too when the reader's usage writes.
    const uint64_t writes = static_cast<uint64_t>(readerUsage) & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK;
    const uint64_t lockUsage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | writes;
    if (frame->locked == nullptr && AHardwareBuffer_lock(frame->buffer, lockUsage, -1, nullptr, &frame->locked) != 0) {
        frame->locked = nullptr;
        jniThrowException(env, "java/lang/IllegalStateException", "Failed to lock buffer");
        return nullptr;
    }
    const jint rowStride = static_cast<jint>(description.stride) * pixelStride;
    const jlong size = description.format == AHARDWAREBUFFER_FORMAT_BLOB
                               ? static_cast<jlong>(description.width)
                               : static_cast<jlong>(rowStride) * description.height;
    jobject bytes = env->NewDirectByteBuffer(frame->locked, size);
    if (bytes == nullptr) return nullptr;
    jobject plane = env->NewObject(gFields.planeClass, gFields.planeConstructor, thiz, rowStride, pixelStride, bytes);
    if (plane == nullptr) return nullptr;
    env->SetObjectArrayElement(planes, 0, plane);
    return planes;
}

jint Image_getWidth(JNIEnv *env, jobject thiz) {
    Frame *frame = frame_of(env, thiz);
    if (frame == nullptr) return -1;
    return frame->cropWidth > 0 ? frame->cropWidth : static_cast<jint>(describe(frame).width);
}

jint Image_getHeight(JNIEnv *env, jobject thiz) {
    Frame *frame = frame_of(env, thiz);
    if (frame == nullptr) return -1;
    return frame->cropHeight > 0 ? frame->cropHeight : static_cast<jint>(describe(frame).height);
}

jint Image_getFormat(JNIEnv *env, jobject thiz, jint readerFormat) {
    if (readerFormat == static_cast<jint>(PublicFormat::PRIVATE)) return readerFormat;
    Frame *frame = frame_of(env, thiz);
    if (frame == nullptr) return 0;
    const jint dataSpace = env->GetIntField(thiz, gFields.dataSpace);
    return static_cast<jint>(mapHalFormatDataspaceToPublicFormat(static_cast<int>(describe(frame).format),
                                                                 static_cast<android_dataspace>(dataSpace)));
}

jint Image_getFenceFd(JNIEnv *, jobject) {
    return -1;
}

jobject Image_getHardwareBuffer(JNIEnv *env, jobject thiz) {
    Frame *frame = frame_of(env, thiz);
    return frame != nullptr ? android_hardware_HardwareBuffer_createFromAHardwareBuffer(env, frame->buffer) : nullptr;
}

const JNINativeMethod gImageReaderMethods[] = {
        {"nativeClassInit", "()V", (void *)ImageReader_classInit},
        {"nativeInit", "(Ljava/lang/Object;IIIJII)V", (void *)ImageReader_init},
        {"nativeClose", "()V", (void *)ImageReader_close},
        {"nativeReleaseImage", "(Landroid/media/Image;)V", (void *)ImageReader_imageRelease},
        {"nativeImageSetup", "(Landroid/media/Image;)I", (void *)ImageReader_imageSetup},
        {"nativeGetSurface", "()Landroid/view/Surface;", (void *)ImageReader_getSurface},
        {"nativeDetachImage", "(Landroid/media/Image;Z)I", (void *)ImageReader_detachImage},
        {"nativeCreateImagePlanes",
         "(ILandroid/graphics/GraphicBuffer;IIIIII)[Landroid/media/ImageReader$ImagePlane;",
         (void *)ImageReader_createImagePlanes},
        {"nativeUnlockGraphicBuffer", "(Landroid/graphics/GraphicBuffer;)V", (void *)ImageReader_unlockGraphicBuffer},
        {"nativeDiscardFreeBuffers", "()V", (void *)ImageReader_discardFreeBuffers},
};

const JNINativeMethod gImageMethods[] = {
        {"nativeCreatePlanes", "(IIJ)[Landroid/media/ImageReader$SurfaceImage$SurfacePlane;",
         (void *)Image_createSurfacePlanes},
        {"nativeGetWidth", "()I", (void *)Image_getWidth},
        {"nativeGetHeight", "()I", (void *)Image_getHeight},
        {"nativeGetFormat", "(I)I", (void *)Image_getFormat},
        {"nativeGetFenceFd", "()I", (void *)Image_getFenceFd},
        {"nativeGetHardwareBuffer", "()Landroid/hardware/HardwareBuffer;", (void *)Image_getHardwareBuffer},
};

}  // namespace

// The buffer of a reader's image that its reader detached, for ImageWriter's attach, else null; the image keeps its
// reference.
AHardwareBuffer *minato_ImageReader_detachedBuffer(JNIEnv *, jlong imageNativeBuffer) {
    const Frame *frame = reinterpret_cast<const Frame *>(imageNativeBuffer);
    return frame != nullptr && frame->slot < 0 ? frame->buffer : nullptr;
}

int register_android_media_ImageReader(JNIEnv *env) {
    RegisterMethodsOrDie(env, "android/media/ImageReader", gImageReaderMethods, NELEM(gImageReaderMethods));
    return RegisterMethodsOrDie(env, "android/media/ImageReader$SurfaceImage", gImageMethods, NELEM(gImageMethods));
}

}  // namespace android
