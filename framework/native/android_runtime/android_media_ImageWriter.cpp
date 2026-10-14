// Ours in place of AOSP's media/jni/android_media_ImageWriter.cpp (ADR-0058 e1), the graphics breadth's G3b):
// ImageWriter's natives as a producer of libnativewindow's buffer queue (APKRuntime/buffer_queue.h) instead of
// libgui's Surface, following the tag's answers:
// - nativeInit connects to the queue window behind the Surface (minato_ANativeWindow_connect, the tag's connect as
//   NATIVE_WINDOW_API_CAMERA): its frames are kept, none dropped, the queue has maxImages slots for the writer next to
//   its consumer's, and a second writer on the same Surface fails with "Failed to connect to native window". The
//   size is the Surface's unless the app gave one, the format and data space the Surface's or the app's; mWriterFormat
//   is the public format of the Surface's. A writer on a window of the host (a SurfaceView's) is not connectable: the
//   tag's error, a named limit.
// - nativeDequeueInputImage dequeues a slot, waiting while every slot is busy, as the tag's; an abandoned queue throws
//   "Surface has been abandoned". A PRIVATE writer whose usage the CPU writes cannot dequeue, as the AVD's gralloc
//   makes no such buffer ("dequeue buffer failed"). The image keeps its frame in mNativeBuffer until it is queued or
//   cancelled.
// - nativeQueueInputImage unlocks and queues with the image's timestamp, data space, crop and transform;
//   cancelImage gives the slot back without a frame and without onImageReleased.
// - nativeAttachAndQueueImage queues an image a reader detached: its buffer goes into a free slot (waiting for one) and
//   leaves the queue again at the consumer's release.
// - onImageReleased: each consumer release of the writer's frames calls the static postEventFromNative.
// - An image's planes are its buffer locked for the CPU, one plane for the RGB formats and none for PRIVATE.
// nativeAttachAndQueueGraphicBuffer (a HardwareBuffer image of no reader) logs Unwritten's line and throws. A fence
// the app sets on an image is closed at its queue or cancel, not waited for: Minato has no sync files. The writer's
// close gives the window its consumer's size, format and data space back, as libgui's disconnect.
#define LOG_TAG "ImageWriter_JNI"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <ui/PublicFormat.h>
#include <unistd.h>

#include <APKRuntime/buffer_queue.h>
#include <log/log.h>

#include "core_jni_helpers.h"
#include "minato_gles.h"

extern "C" ANativeWindow *minato_ANativeWindow_fromSurface(JNIEnv *env, jobject surface);

namespace android {

jobject android_hardware_HardwareBuffer_createFromAHardwareBuffer(JNIEnv *env, AHardwareBuffer *hardwareBuffer);
AHardwareBuffer *minato_ImageReader_detachedBuffer(JNIEnv *env, jlong imageNativeBuffer);

namespace {

constexpr int32_t kHalPrivate = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
constexpr uint64_t kCpuUsage = AHARDWAREBUFFER_USAGE_CPU_READ_MASK | AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK;

struct {
    jfieldID writerFormat;  // ImageWriter.mWriterFormat
    jmethodID postEvent;    // ImageWriter.postEventFromNative
    jfieldID nativeBuffer;  // WriterSurfaceImage.mNativeBuffer
    jfieldID fenceFd;       // WriterSurfaceImage.mNativeFenceFd
    jfieldID planes;        // WriterSurfaceImage.mPlanes
    jclass planeClass;      // WriterSurfaceImage.SurfacePlane, global
    jmethodID planeConstructor;
} gFields;

// libnativewindow's private functions (APKRT_PRIVATE), looked up once.
struct Queue {
    decltype(&minato_ANativeWindow_isQueue) isQueue = nullptr;
    decltype(&minato_ANativeWindow_connect) connect = nullptr;
    decltype(&minato_ANativeWindow_disconnect) disconnect = nullptr;
    decltype(&minato_ANativeWindow_dequeue) dequeue = nullptr;
    decltype(&minato_ANativeWindow_queueWith) queueWith = nullptr;
    decltype(&minato_ANativeWindow_attachAndQueue) attachAndQueue = nullptr;
    decltype(&minato_ANativeWindow_cancel) cancel = nullptr;
};

const Queue &queue_functions() {
    static const Queue functions = [] {
        Queue found;
        if (void *library = dlopen("libnativewindow.so", RTLD_NOW)) {
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
            FIND(isQueue, minato_ANativeWindow_isQueue);
            FIND(connect, minato_ANativeWindow_connect);
            FIND(disconnect, minato_ANativeWindow_disconnect);
            FIND(dequeue, minato_ANativeWindow_dequeue);
            FIND(queueWith, minato_ANativeWindow_queueWith);
            FIND(attachAndQueue, minato_ANativeWindow_attachAndQueue);
            FIND(cancel, minato_ANativeWindow_cancel);
#undef FIND
        }
        return found;
    }();
    return functions;
}

bool queue_found() {
    const Queue &queue = queue_functions();
    return queue.isQueue != nullptr && queue.connect != nullptr && queue.disconnect != nullptr &&
           queue.dequeue != nullptr && queue.queueWith != nullptr && queue.attachAndQueue != nullptr &&
           queue.cancel != nullptr;
}

// A writer's context (mNativeContext): its window with a reference of its own, its format, and the references of
// onImageReleased's posts, which the queue calls until the writer disconnects.
struct Writer {
    ANativeWindow *window = nullptr;
    int32_t format = 0;  // the hardware buffer format of its buffers
    uint64_t usage = 0;
    JavaVM *vm = nullptr;
    jclass clazz = nullptr;  // global, ImageWriter
    jobject weak = nullptr;  // global, the WeakReference of postEventFromNative
};

// A dequeued image's frame: its slot, its buffer with a reference of the image's own, and its CPU lock.
struct Frame {
    int32_t slot = -1;
    AHardwareBuffer *buffer = nullptr;
    void *locked = nullptr;
};

Writer *writer_of(jlong context) {
    return reinterpret_cast<Writer *>(context);
}

Frame *frame_of(JNIEnv *env, jobject image) {
    return reinterpret_cast<Frame *>(env->GetLongField(image, gFields.nativeBuffer));
}

void unlock(Frame *frame) {
    if (frame->locked == nullptr) return;
    AHardwareBuffer_unlock(frame->buffer, nullptr);
    frame->locked = nullptr;
}

// The image's frame is gone: its buffer reference, the image's pointer to it, and its fence, which the queue does not
// take (Minato has no sync files to wait for). Under the image's monitor, which its natives hold.
void forget(JNIEnv *env, jobject image, Frame *frame) {
    env->SetLongField(image, gFields.nativeBuffer, 0);
    const jint fence = env->GetIntField(image, gFields.fenceFd);
    if (fence != -1) close(fence);
    env->SetIntField(image, gFields.fenceFd, -1);
    AHardwareBuffer_release(frame->buffer);
    delete frame;
}

AHardwareBuffer_Desc describe(AHardwareBuffer *buffer) {
    AHardwareBuffer_Desc description{};
    AHardwareBuffer_describe(buffer, &description);
    return description;
}

void image_released(void *context) {
    auto *writer = static_cast<Writer *>(context);
    JNIEnv *env = nullptr;
    bool attached = false;
    if (writer->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (writer->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            ALOGW("onBufferReleased event will not posted");
            return;
        }
        attached = true;
    }
    env->CallStaticVoidMethod(writer->clazz, gFields.postEvent, writer->weak);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) writer->vm->DetachCurrentThread();
}

minato_BufferQueue_settings settings(jlong timestamp, jint dataSpace, jint left, jint top, jint right, jint bottom,
                                     jint transform) {
    minato_BufferQueue_settings made{};
    made.timestamp = timestamp;
    made.transform = transform;
    made.data_space = dataSpace;
    made.crop_left = left;
    made.crop_top = top;
    made.crop_right = right;
    made.crop_bottom = bottom;
    return made;
}

void throw_queue_error(JNIEnv *env, int result, const char *otherwise) {
    if (result == -ENODEV) {
        jniThrowException(env, "java/lang/IllegalStateException", "Surface has been abandoned");
    } else {
        jniThrowRuntimeException(env, otherwise);
    }
}

void ImageWriter_classInit(JNIEnv *env, jclass clazz) {
    gFields.writerFormat = GetFieldIDOrDie(env, clazz, "mWriterFormat", "I");
    gFields.postEvent = GetStaticMethodIDOrDie(env, clazz, "postEventFromNative", "(Ljava/lang/Object;)V");
    jclass image = FindClassOrDie(env, "android/media/ImageWriter$WriterSurfaceImage");
    gFields.nativeBuffer = GetFieldIDOrDie(env, image, "mNativeBuffer", "J");
    gFields.fenceFd = GetFieldIDOrDie(env, image, "mNativeFenceFd", "I");
    jclass plane = FindClassOrDie(env, "android/media/ImageWriter$WriterSurfaceImage$SurfacePlane");
    gFields.planeClass = MakeGlobalRefOrDie(env, plane);
    gFields.planeConstructor = GetMethodIDOrDie(
            env, plane, "<init>", "(Landroid/media/ImageWriter$WriterSurfaceImage;IILjava/nio/ByteBuffer;)V");
}

jlong ImageWriter_init(JNIEnv *env, jobject thiz, jobject weakThiz, jobject surface, jint maxImages, jint width,
                       jint height, jboolean useSurfaceImageFormatInfo, jint hardwareBufferFormat, jint dataSpace,
                       jlong usage) {
    if (surface == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "The surface has been released");
        return 0;
    }
    ANativeWindow *window = minato_ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "The surface has been released");
        return 0;
    }
    const Queue &queue = queue_functions();
    auto *writer = new Writer;
    writer->window = window;
    env->GetJavaVM(&writer->vm);
    writer->clazz = static_cast<jclass>(env->NewGlobalRef(env->GetObjectClass(thiz)));
    writer->weak = env->NewGlobalRef(weakThiz);
    auto fail = [&](const char *message) {
        env->DeleteGlobalRef(writer->clazz);
        env->DeleteGlobalRef(writer->weak);
        ANativeWindow_release(window);
        delete writer;
        jniThrowRuntimeException(env, message);
        return 0;
    };
    // The tag's connect as NATIVE_WINDOW_API_CAMERA; a host window has no queue to connect to. Too many slots are the
    // tag's failed set_buffer_count.
    const int connected = queue_found() && queue.isQueue(window)
                                  ? queue.connect(window, maxImages, image_released, writer)
                                  : -ENODEV;
    if (connected == -EINVAL) return fail("Set buffer count failed");
    if (connected != 0) return fail("Failed to connect to native window");
    int32_t format = hardwareBufferFormat;
    if (useSurfaceImageFormatInfo) {
        format = ANativeWindow_getFormat(window);
        dataSpace = ANativeWindow_getBuffersDataSpace(window);
    }
    if (width > 0 && height > 0) {
        ANativeWindow_setBuffersGeometry(window, width, height, format);
    } else {
        ANativeWindow_setBuffersGeometry(window, 0, 0, format);
    }
    ANativeWindow_setBuffersDataSpace(window, dataSpace);
    writer->format = format;
    // As the tag's: an opaque format without a usage of the app's keeps the consumer's, else the CPU writes.
    writer->usage = usage == -1 ? (format == kHalPrivate ? 0 : AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN)
                                : static_cast<uint64_t>(usage);
    env->SetIntField(thiz, gFields.writerFormat,
                     static_cast<jint>(mapHalFormatDataspaceToPublicFormat(
                             format, static_cast<android_dataspace>(dataSpace))));
    return reinterpret_cast<jlong>(writer);
}

void ImageWriter_close(JNIEnv *env, jobject, jlong context) {
    Writer *writer = writer_of(context);
    if (writer == nullptr) return;
    // After the disconnect no onImageReleased runs or starts, so its references can go. As libgui's disconnect, the
    // window's size, format and data space are the consumer's again.
    queue_functions().disconnect(writer->window);
    ANativeWindow_setBuffersGeometry(writer->window, 0, 0, 0);
    ANativeWindow_setBuffersDataSpace(writer->window, 0);
    ANativeWindow_release(writer->window);
    env->DeleteGlobalRef(writer->clazz);
    env->DeleteGlobalRef(writer->weak);
    delete writer;
}

void ImageWriter_dequeueImage(JNIEnv *env, jobject, jlong context, jobject image) {
    Writer *writer = writer_of(context);
    if (writer == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "ImageWriterContext is not initialized");
        return;
    }
    // The AVD's gralloc makes no IMPLEMENTATION_DEFINED buffer the CPU writes.
    if (writer->format == kHalPrivate && (writer->usage & kCpuUsage) != 0) {
        jniThrowRuntimeException(env, "dequeue buffer failed");
        return;
    }
    AHardwareBuffer *buffer = nullptr;
    int32_t slot = -1;
    const int result = queue_functions().dequeue(writer->window, &buffer, &slot);
    if (result != 0) {
        throw_queue_error(env, result, "dequeue buffer failed");
        return;
    }
    auto *frame = new Frame;
    frame->slot = slot;
    frame->buffer = buffer;
    AHardwareBuffer_acquire(buffer);
    env->SetLongField(image, gFields.nativeBuffer, reinterpret_cast<jlong>(frame));
}

void ImageWriter_queueImage(JNIEnv *env, jobject, jlong context, jobject image, jlong timestamp, jint dataSpace,
                            jint left, jint top, jint right, jint bottom, jint transform, jint) {
    Writer *writer = writer_of(context);
    if (writer == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "ImageWriterContext is not initialized");
        return;
    }
    if (env->MonitorEnter(image) != JNI_OK) return;
    Frame *frame = frame_of(env, image);
    int result = -EINVAL;
    if (frame != nullptr) {
        unlock(frame);
        const minato_BufferQueue_settings queued = settings(timestamp, dataSpace, left, top, right, bottom, transform);
        result = queue_functions().queueWith(writer->window, frame->slot, &queued);
        if (result == 0) forget(env, image, frame);
    }
    env->MonitorExit(image);
    if (frame == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Image is not initialized");
    } else if (result != 0) {
        throw_queue_error(env, result, "Queue input buffer failed");
    }
}

void ImageWriter_cancelImage(JNIEnv *env, jobject, jlong context, jobject image) {
    Writer *writer = writer_of(context);
    if (writer == nullptr) {
        ALOGW("ImageWriter is not initialized");
        return;
    }
    if (env->MonitorEnter(image) != JNI_OK) return;
    if (Frame *frame = frame_of(env, image)) {  // else cancelled already
        unlock(frame);
        queue_functions().cancel(writer->window, frame->slot);
        forget(env, image, frame);
    }
    env->MonitorExit(image);
}

jint ImageWriter_attachAndQueueImage(JNIEnv *env, jobject, jlong context, jlong imageNativeBuffer,
                                     jint hardwareBufferFormat, jlong timestamp, jint dataSpace, jint left, jint top,
                                     jint right, jint bottom, jint transform, jint) {
    Writer *writer = writer_of(context);
    if (writer == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "ImageWriterContext is not initialized");
        return -1;
    }
    if ((writer->format == kHalPrivate) != (hardwareBufferFormat == kHalPrivate)) {
        jniThrowException(env, "java/lang/IllegalStateException",
                          "Trying to attach an opaque image into a non-opaque ImageWriter, or vice versa");
        return -1;
    }
    AHardwareBuffer *buffer = minato_ImageReader_detachedBuffer(env, imageNativeBuffer);
    if (buffer == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Image is not initialized or already closed");
        return -1;
    }
    const minato_BufferQueue_settings queued = settings(timestamp, dataSpace, left, top, right, bottom, transform);
    const int result = queue_functions().attachAndQueue(writer->window, buffer, &queued);
    if (result != 0) {
        throw_queue_error(env, result, "nativeAttachImage failed!!!");
        return -1;
    }
    return 0;
}

jint ImageWriter_attachAndQueueGraphicBuffer(JNIEnv *env, jobject, jlong, jobject, jint, jlong, jint, jint, jint,
                                             jint, jint, jint, jint) {
    minato_unwritten(env, "android.media.ImageWriter.nativeAttachAndQueueGraphicBuffer");
    jniThrowException(env, "java/lang/UnsupportedOperationException",
                      "Queueing an image of no ImageReader is not supported");
    return -1;
}

// The planes of the image's buffer, locked for the CPU at the first call: one for an RGB format, none for PRIVATE.
jobjectArray Image_createSurfacePlanes(JNIEnv *env, jobject thiz, jint numPlanes, jint writerFormat) {
    const int32_t halFormat = mapPublicFormatToHalFormat(static_cast<PublicFormat>(writerFormat));
    if (halFormat == kHalPrivate && numPlanes > 0) {
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException",
                             "Format 0x%x is opaque, thus not writable, the number of planes (%d) must be 0",
                             halFormat, numPlanes);
        return nullptr;
    }
    jobjectArray planes = env->NewObjectArray(numPlanes, gFields.planeClass, nullptr);
    if (planes == nullptr) {
        jniThrowRuntimeException(env, "Failed to create SurfacePlane arrays, probably out of memory");
        return nullptr;
    }
    if (numPlanes == 0) return planes;
    Frame *frame = frame_of(env, thiz);
    if (frame == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Image is not initialized");
        return nullptr;
    }
    const AHardwareBuffer_Desc description = describe(frame->buffer);
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
            jniThrowExceptionFmt(env, "java/lang/UnsupportedOperationException", "Pixel format: 0x%x is unsupported",
                                 description.format);
            return nullptr;
    }
    if (numPlanes != 1) {
        jniThrowExceptionFmt(env, "java/lang/UnsupportedOperationException", "Pixel format: 0x%x is unsupported",
                             description.format);
        return nullptr;
    }
    // As the tag's, for writing; reading too, which an IOSurface lock costs nothing more for.
    const uint64_t lockUsage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    if (frame->locked == nullptr && AHardwareBuffer_lock(frame->buffer, lockUsage, -1, nullptr, &frame->locked) != 0) {
        frame->locked = nullptr;
        jniThrowExceptionFmt(env, "java/lang/RuntimeException", "lock buffer failed for format 0x%x",
                             description.format);
        return nullptr;
    }
    const jint rowStride = static_cast<jint>(description.stride) * pixelStride;
    const jlong size = description.format == AHARDWAREBUFFER_FORMAT_BLOB
                               ? static_cast<jlong>(description.width)
                               : static_cast<jlong>(rowStride) * (description.height - 1) +
                                         static_cast<jlong>(description.width) * pixelStride;
    jobject byteBuffer = env->NewDirectByteBuffer(frame->locked, size);
    if (byteBuffer == nullptr) {
        if (!env->ExceptionCheck()) {
            jniThrowException(env, "java/lang/IllegalStateException", "Failed to allocate ByteBuffer");
        }
        return nullptr;
    }
    jobject plane = env->NewObject(gFields.planeClass, gFields.planeConstructor, thiz, rowStride, pixelStride,
                                   byteBuffer);
    if (plane == nullptr) return nullptr;
    env->SetObjectArrayElement(planes, 0, plane);
    return planes;
}

jint Image_getWidth(JNIEnv *env, jobject thiz) {
    Frame *frame = frame_of(env, thiz);
    return frame != nullptr ? static_cast<jint>(describe(frame->buffer).width) : -1;
}

jint Image_getHeight(JNIEnv *env, jobject thiz) {
    Frame *frame = frame_of(env, thiz);
    return frame != nullptr ? static_cast<jint>(describe(frame->buffer).height) : -1;
}

jint Image_getFormat(JNIEnv *env, jobject thiz, jint dataSpace) {
    Frame *frame = frame_of(env, thiz);
    if (frame == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Image is not initialized");
        return 0;
    }
    return static_cast<jint>(mapHalFormatDataspaceToPublicFormat(
            static_cast<int>(describe(frame->buffer).format), static_cast<android_dataspace>(dataSpace)));
}

jobject Image_getHardwareBuffer(JNIEnv *env, jobject thiz) {
    Frame *frame = frame_of(env, thiz);
    if (frame == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Image is not initialized");
        return nullptr;
    }
    return android_hardware_HardwareBuffer_createFromAHardwareBuffer(env, frame->buffer);
}

void Image_setFenceFd(JNIEnv *env, jobject thiz, jint fenceFd) {
    const jint old = env->GetIntField(thiz, gFields.fenceFd);
    if (old != -1) close(old);
    env->SetIntField(thiz, gFields.fenceFd, fenceFd);
}

const JNINativeMethod gWriterMethods[] = {
        {"nativeClassInit", "()V", (void *)ImageWriter_classInit},
        {"nativeInit", "(Ljava/lang/Object;Landroid/view/Surface;IIIZIIJ)J", (void *)ImageWriter_init},
        {"nativeClose", "(J)V", (void *)ImageWriter_close},
        {"nativeDequeueInputImage", "(JLandroid/media/Image;)V", (void *)ImageWriter_dequeueImage},
        {"nativeQueueInputImage", "(JLandroid/media/Image;JIIIIIII)V", (void *)ImageWriter_queueImage},
        {"nativeAttachAndQueueImage", "(JJIJIIIIIII)I", (void *)ImageWriter_attachAndQueueImage},
        {"nativeAttachAndQueueGraphicBuffer", "(JLandroid/graphics/GraphicBuffer;IJIIIIIII)I",
         (void *)ImageWriter_attachAndQueueGraphicBuffer},
        {"cancelImage", "(JLandroid/media/Image;)V", (void *)ImageWriter_cancelImage},
};

const JNINativeMethod gImageMethods[] = {
        {"nativeCreatePlanes", "(II)[Landroid/media/ImageWriter$WriterSurfaceImage$SurfacePlane;",
         (void *)Image_createSurfacePlanes},
        {"nativeGetWidth", "()I", (void *)Image_getWidth},
        {"nativeGetHeight", "()I", (void *)Image_getHeight},
        {"nativeGetFormat", "(I)I", (void *)Image_getFormat},
        {"nativeGetHardwareBuffer", "()Landroid/hardware/HardwareBuffer;", (void *)Image_getHardwareBuffer},
        {"nativeSetFenceFd", "(I)V", (void *)Image_setFenceFd},
};

}  // namespace

int register_android_media_ImageWriter(JNIEnv *env) {
    RegisterMethodsOrDie(env, "android/media/ImageWriter", gWriterMethods, NELEM(gWriterMethods));
    return RegisterMethodsOrDie(env, "android/media/ImageWriter$WriterSurfaceImage", gImageMethods,
                                NELEM(gImageMethods));
}

}  // namespace android
