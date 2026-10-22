// libmediandk's AImageReader and AImage (ADR-0058 e1), the graphics breadth's G3c), in place of AOSP's
// media/ndk/NdkImageReader.cpp and NdkImage.cpp, on libnativewindow's buffer queue (APKRuntime/buffer_queue.h) as an
// ImageReader's consumer (G3a), following android-15.0.0_r1's answers [verified: image-ndk-test on apk-runner-35]:
// - a reader is a queue of maxImages acquired frames, its window the queue's producer, which the reader keeps (the
//   caller does not release it). Frames a producer queues over one not acquired yet drop, as Android 15's.
// - onImageAvailable runs on the reader's own thread (the tag's ALooper "ImageReader-..."), one call per frame the
//   queue reports while a listener is set, never on the producer's; a frame without a listener is not reported once
//   one is set; AImageReader_delete stops the thread and waits for a call that runs.
// - acquire: maxImages held answer AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED, an empty queue NO_BUFFER_AVAILABLE; a crop off
//   the origin or a buffer of another format AMEDIA_ERROR_UNKNOWN; acquireLatestImage keeps only the newest; the Async
//   variants' fence is -1, as Minato has no fences.
// - an image keeps its reader alive (the tag's AImage holds its reader), so AImage_delete after AImageReader_delete,
//   which closes every image still held and abandons the queue, is fine; a closed image answers
//   AMEDIA_ERROR_INVALID_OBJECT. Its planes lock the buffer for the CPU at the first call, which needs the reader's
//   CPU_READ usage (AMEDIA_IMGREADER_CANNOT_LOCK_IMAGE without); its hardware buffer is its own, without a reference
//   for the caller; its size is the producer's crop's when it set one, else the buffer's, and its crop rectangle that
//   size from the origin, as the tag's. A failed acquire leaves NULL; a NULL fence of the Async variants is fine.
// Departures: maxImages below 1 or past the queue's 62 answers AMEDIA_ERROR_INVALID_PARAMETER, where Android's
// BufferItemConsumer aborts the process (the tag ignores validateParameters' answer); a buffer-removed listener is
// kept but never called, as the queue frees no buffer a producer would hear of. YUV formats: a YUV reader's
// producers get YUV buffers (ADR-0060 e2a), whose AImage gives three planes, Y, Cb and Cr, by lockPlanes
// (ADR-0060 e2c), each from its first sample to its last, as the framework's Java planes.
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "APKRuntime/buffer_queue.h"

// The library of the queue's functions; buffer-queue-host-test builds the queue into its own program (nullptr).
#ifndef MINATO_QUEUE_LIBRARY
#define MINATO_QUEUE_LIBRARY "libnativewindow.so"
#endif

namespace {

constexpr int32_t kHalPrivate = 0x22;      // HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED
constexpr int32_t kHalYCbCr420 = 0x23;     // HAL_PIXEL_FORMAT_YCbCr_420_888
constexpr int32_t kHalBlob = 0x21;         // HAL_PIXEL_FORMAT_BLOB, JPEG's
constexpr int32_t kMaxAcquired = MINATO_BUFFER_QUEUE_MAX_SLOTS - 2;

// libnativewindow's private functions (APKRT_PRIVATE), looked up once.
struct Queue {
    decltype(&minato_BufferQueue_create) create = nullptr;
    decltype(&minato_BufferQueue_setDefaultSize) setDefaultSize = nullptr;
    decltype(&minato_BufferQueue_setDefaultFormat) setDefaultFormat = nullptr;
    decltype(&minato_BufferQueue_setConsumer) setConsumer = nullptr;
    decltype(&minato_BufferQueue_acquire) acquire = nullptr;
    decltype(&minato_BufferQueue_releaseSlot) releaseSlot = nullptr;
    decltype(&minato_BufferQueue_release) release = nullptr;
};

// Without the C++ runtime (no guards for a function's statics): found once by pthread_once.
Queue found_functions;
pthread_once_t functions_once = PTHREAD_ONCE_INIT;

void find_functions() {
    Queue &found = found_functions;
    if (void *library = dlopen(MINATO_QUEUE_LIBRARY, RTLD_NOW)) {
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
        FIND(create, minato_BufferQueue_create);
        FIND(setDefaultSize, minato_BufferQueue_setDefaultSize);
        FIND(setDefaultFormat, minato_BufferQueue_setDefaultFormat);
        FIND(setConsumer, minato_BufferQueue_setConsumer);
        FIND(acquire, minato_BufferQueue_acquire);
        FIND(releaseSlot, minato_BufferQueue_releaseSlot);
        FIND(release, minato_BufferQueue_release);
#undef FIND
    }
}

const Queue &queue_functions() {
    pthread_once(&functions_once, find_functions);
    return found_functions;
}

bool queue_found() {
    const Queue &queue = queue_functions();
    return queue.create != nullptr && queue.setDefaultSize != nullptr && queue.setDefaultFormat != nullptr &&
           queue.setConsumer != nullptr && queue.acquire != nullptr && queue.releaseSlot != nullptr &&
           queue.release != nullptr;
}

// The hardware buffer format of a public AIMAGE_FORMAT_*, and whether the tag's isSupportedFormatAndUsage takes it.
bool hal_format(int32_t format, int32_t *hal) {
    switch (format) {
        case AIMAGE_FORMAT_RGBA_8888:
        case AIMAGE_FORMAT_RGBX_8888:
        case AIMAGE_FORMAT_RGB_888:
        case AIMAGE_FORMAT_RGB_565:
        case AIMAGE_FORMAT_RGBA_FP16:
        case AIMAGE_FORMAT_RAW16:
        case AIMAGE_FORMAT_RAW_PRIVATE:
        case AIMAGE_FORMAT_RAW10:
        case AIMAGE_FORMAT_RAW12:
        case AIMAGE_FORMAT_Y8:
            *hal = format;
            return true;
        case AIMAGE_FORMAT_YUV_420_888:
            *hal = kHalYCbCr420;
            return true;
        case AIMAGE_FORMAT_PRIVATE:
            *hal = kHalPrivate;
            return true;
        case AIMAGE_FORMAT_JPEG:
        case AIMAGE_FORMAT_DEPTH_POINT_CLOUD:
        case AIMAGE_FORMAT_HEIC:
        case AIMAGE_FORMAT_DEPTH_JPEG:
            *hal = kHalBlob;
            return true;
        case AIMAGE_FORMAT_DEPTH16:
            *hal = 0x20363159;  // HAL_PIXEL_FORMAT_Y16
            return true;
        default:
            return false;
    }
}

// The bytes of a pixel of an AHardwareBuffer format with one plane, else 0.
int32_t pixel_bytes(uint32_t format) {
    switch (format) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
            return 4;
        case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
            return 3;
        case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
        case 0x20:  // RAW16
            return 2;
        case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
            return 8;
        case AHARDWAREBUFFER_FORMAT_R8_UNORM:
            return 1;
        default:
            return 0;
    }
}

// What onImageAvailable runs with: the queue's context, which lives until the queue's end, so that a producer that
// read `available` before the reader's delete finds it; the dispatcher thread waits on it.
struct Hub {
    int32_t references;  // the queue's, and the dispatcher thread's while it runs; under the lock
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int32_t pending;  // frames reported while a listener was set, not yet called back
    bool stopped;
    // The listener, set and read under the lock; as the tag's, a frame that comes without one is not reported later.
    AImageReader_ImageListener listener;
    bool listening;
    AImageReader *reader;  // the callback's argument, while not stopped
    pthread_t dispatcher;
    bool dispatching;
};

Hub *make_hub() {
    auto *hub = static_cast<Hub *>(calloc(1, sizeof(Hub)));
    if (hub == nullptr) return nullptr;
    hub->references = 1;
    pthread_mutex_init(&hub->lock, nullptr);
    pthread_cond_init(&hub->wake, nullptr);
    return hub;
}

void frame_available(void *context) {
    auto *hub = static_cast<Hub *>(context);
    pthread_mutex_lock(&hub->lock);
    if (!hub->stopped && hub->listening) {
        hub->pending++;
        pthread_cond_signal(&hub->wake);
    }
    pthread_mutex_unlock(&hub->lock);
}

void unref_hub(Hub *hub) {
    pthread_mutex_lock(&hub->lock);
    const bool last = --hub->references == 0;
    pthread_mutex_unlock(&hub->lock);
    if (!last) return;
    pthread_cond_destroy(&hub->wake);
    pthread_mutex_destroy(&hub->lock);
    free(hub);
}

void hub_destroyed(void *context) {
    unref_hub(static_cast<Hub *>(context));
}

}  // namespace

struct AImage {
    AImageReader *reader;
    int32_t slot;
    AHardwareBuffer *buffer;  // with the image's reference; null once closed
    int64_t timestamp;
    int32_t data_space;
    int32_t width, height;  // the producer's crop's when it has one, else the buffer's, as the tag's getBufferWidth
    void *locked;
    AHardwareBuffer_Planes planes;  // a YUV image's, while locked
    AImage *next;  // the reader's images still open
};

struct AImageReader {
    pthread_mutex_t lock;  // the reader's state and its images'
    uint32_t references;   // the reader's own, until its delete, and one per AImage until its delete; under the lock
    bool deleted;
    minato_BufferQueue *queue;
    ANativeWindow *window;
    Hub *hub;
    int32_t width, height, format, hal, max_images;
    uint64_t usage;
    int32_t acquired;
    AImage *images;
    AImageReader_BufferRemovedListener removed_listener;
};

namespace {

void unref(AImageReader *reader) {
    pthread_mutex_lock(&reader->lock);
    const bool last = --reader->references == 0;
    pthread_mutex_unlock(&reader->lock);
    if (!last) return;
    pthread_mutex_destroy(&reader->lock);
    free(reader);
}

// The image's frame back to the queue, under the reader's lock: unlocked, its slot released, its buffer let go.
void close_image(AImage *image) {
    if (image->buffer == nullptr) return;
    if (image->locked != nullptr) AHardwareBuffer_unlock(image->buffer, nullptr);
    image->locked = nullptr;
    queue_functions().releaseSlot(image->reader->queue, image->slot);
    AHardwareBuffer_release(image->buffer);
    image->buffer = nullptr;
    image->reader->acquired--;
}

void *dispatch(void *context) {
    auto *hub = static_cast<Hub *>(context);
    pthread_mutex_lock(&hub->lock);
    // Only the hub is touched once it is stopped: a delete from inside a call frees the reader before this thread has
    // ended, its own reference keeping the hub.
    for (;;) {
        while (hub->pending == 0 && !hub->stopped) pthread_cond_wait(&hub->wake, &hub->lock);
        if (hub->stopped) break;
        hub->pending--;
        const AImageReader_ImageListener listener = hub->listener;
        AImageReader *const reader = hub->reader;
        const bool call = hub->listening && listener.onImageAvailable != nullptr;
        pthread_mutex_unlock(&hub->lock);
        if (call) listener.onImageAvailable(listener.context, reader);
        pthread_mutex_lock(&hub->lock);
    }
    pthread_mutex_unlock(&hub->lock);
    unref_hub(hub);
    return nullptr;
}

// One frame of the queue as an image of the reader, under its lock.
media_status_t acquire_locked(AImageReader *reader, AImage **out) {
    if (reader->deleted) return AMEDIA_ERROR_INVALID_OBJECT;
    if (reader->acquired >= reader->max_images) return AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED;
    minato_BufferQueue_frame frame{};
    const int result = queue_functions().acquire(reader->queue, &frame);
    if (result == -EBUSY) return AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED;
    if (result == -EAGAIN || result == -ENODEV) return AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
    if (result != 0) return AMEDIA_ERROR_UNKNOWN;
    AHardwareBuffer_Desc description{};
    AHardwareBuffer_describe(frame.buffer, &description);
    const bool opaque = reader->hal == kHalPrivate;
    if ((!opaque && (frame.crop_left != 0 || frame.crop_top != 0)) ||
        (!opaque && static_cast<int32_t>(description.format) != reader->hal)) {
        queue_functions().releaseSlot(reader->queue, frame.slot);
        return AMEDIA_ERROR_UNKNOWN;
    }
    auto *image = static_cast<AImage *>(calloc(1, sizeof(AImage)));
    if (image == nullptr) {
        queue_functions().releaseSlot(reader->queue, frame.slot);
        return AMEDIA_ERROR_UNKNOWN;
    }
    image->reader = reader;
    image->slot = frame.slot;
    image->buffer = frame.buffer;
    AHardwareBuffer_acquire(image->buffer);
    image->timestamp = frame.timestamp;
    image->data_space = frame.data_space;
    const bool cropped = frame.crop_right > frame.crop_left && frame.crop_bottom > frame.crop_top;
    image->width = cropped ? frame.crop_right - frame.crop_left : static_cast<int32_t>(description.width);
    image->height = cropped ? frame.crop_bottom - frame.crop_top : static_cast<int32_t>(description.height);
    image->next = reader->images;
    reader->images = image;
    reader->references++;
    reader->acquired++;
    *out = image;
    return AMEDIA_OK;
}

// The image out of its reader's list and freed, under the reader's lock.
void unlink_locked(AImage *image) {
    AImage **link = &image->reader->images;
    while (*link != nullptr && *link != image) link = &(*link)->next;
    if (*link == image) *link = image->next;
    free(image);
}

media_status_t acquire(AImageReader *reader, AImage **image, int *fence, bool latest) {
    if (reader == nullptr || image == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    *image = nullptr;  // as the tag's, also when nothing comes
    pthread_mutex_lock(&reader->lock);
    const media_status_t status = acquire_locked(reader, image);
    // As the tag's: the next one while there is one, the older one let go; maxImages held ends it too.
    while (latest && status == AMEDIA_OK) {
        AImage *newer = nullptr;
        if (acquire_locked(reader, &newer) != AMEDIA_OK) break;
        close_image(*image);
        unlink_locked(*image);
        reader->references--;
        *image = newer;
    }
    pthread_mutex_unlock(&reader->lock);
    if (fence != nullptr) *fence = -1;
    return status;
}

AImage *checked(const AImage *image) {
    return const_cast<AImage *>(image);
}

}  // namespace

extern "C" {

media_status_t AImageReader_newWithUsage(int32_t width, int32_t height, int32_t format, uint64_t usage,
                                         int32_t maxImages, AImageReader **reader) {
    int32_t hal = 0;
    if (reader == nullptr || width < 1 || height < 1 || maxImages < 1 || maxImages > kMaxAcquired ||
        !hal_format(format, &hal)) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (!queue_found()) return AMEDIA_ERROR_UNKNOWN;
    auto *made = static_cast<AImageReader *>(calloc(1, sizeof(AImageReader)));
    Hub *hub = make_hub();
    if (made == nullptr || hub == nullptr) {
        free(made);
        if (hub != nullptr) hub_destroyed(hub);
        return AMEDIA_ERROR_UNKNOWN;
    }
    const Queue &queue = queue_functions();
    if (queue.create(frame_available, hub_destroyed, hub, &made->window, &made->queue) != 0) {
        hub_destroyed(hub);
        free(made);
        return AMEDIA_ERROR_UNKNOWN;
    }
    pthread_mutex_init(&made->lock, nullptr);
    made->references = 1;
    made->hub = hub;
    made->width = width;
    made->height = height;
    made->format = format;
    made->hal = hal;
    made->max_images = maxImages;
    made->usage = usage;
    queue.setConsumer(made->queue, maxImages, usage);
    queue.setDefaultSize(made->queue, width, height);
    queue.setDefaultFormat(made->queue, hal);
    *reader = made;
    return AMEDIA_OK;
}

media_status_t AImageReader_new(int32_t width, int32_t height, int32_t format, int32_t maxImages,
                                AImageReader **reader) {
    return AImageReader_newWithUsage(width, height, format, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, maxImages, reader);
}

// As the tag's: the format and data space as a public format (mapHalFormatDataspaceToPublicFormat), whose data
// space is not kept.
media_status_t AImageReader_newWithDataSpace(int32_t width, int32_t height, uint64_t usage, int32_t maxImages,
                                             uint32_t hardwareBufferFormat, int32_t dataSpace, AImageReader **reader) {
    constexpr int32_t kDepth = 0x1000, kDynamicDepth = 0x1002, kHeif = 0x1004;  // AIDL Dataspace's
    constexpr int32_t kHalY16 = 0x20363159;
    int32_t format = static_cast<int32_t>(hardwareBufferFormat);
    if (format == kHalBlob) {
        format = dataSpace == kDepth           ? AIMAGE_FORMAT_DEPTH_POINT_CLOUD
                 : dataSpace == kDynamicDepth ? AIMAGE_FORMAT_DEPTH_JPEG
                 : dataSpace == kHeif         ? AIMAGE_FORMAT_HEIC
                                              : AIMAGE_FORMAT_JPEG;
    } else if (format == kHalY16 && dataSpace == kDepth) {
        format = AIMAGE_FORMAT_DEPTH16;
    } else if (format == kHalYCbCr420) {
        format = AIMAGE_FORMAT_YUV_420_888;
    }
    return AImageReader_newWithUsage(width, height, format, usage, maxImages, reader);
}

void AImageReader_delete(AImageReader *reader) {
    if (reader == nullptr) return;
    // No callback after the delete: the dispatcher stops, and a call that runs ends first, unless the delete is that
    // call's own, which then lets the thread end by itself.
    Hub *hub = reader->hub;
    pthread_mutex_lock(&hub->lock);
    hub->stopped = true;
    hub->listening = false;
    hub->reader = nullptr;
    const bool dispatching = hub->dispatching;
    const pthread_t dispatcher = hub->dispatcher;
    pthread_cond_broadcast(&hub->wake);
    pthread_mutex_unlock(&hub->lock);
    if (dispatching) {
        if (pthread_equal(pthread_self(), dispatcher)) {
            pthread_detach(dispatcher);
        } else {
            pthread_join(dispatcher, nullptr);
        }
    }
    pthread_mutex_lock(&reader->lock);
    reader->deleted = true;
    for (AImage *image = reader->images; image != nullptr; image = image->next) close_image(image);
    pthread_mutex_unlock(&reader->lock);
    // Abandoned: the window, which an app may still hold, answers -ENODEV; the hub goes at the queue's end.
    queue_functions().release(reader->queue);
    ANativeWindow_release(reader->window);
    unref(reader);
}

media_status_t AImageReader_getWindow(AImageReader *reader, ANativeWindow **window) {
    if (reader == nullptr || window == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    *window = reader->window;
    return AMEDIA_OK;
}

media_status_t AImageReader_getWidth(const AImageReader *reader, int32_t *width) {
    if (reader == nullptr || width == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    *width = reader->width;
    return AMEDIA_OK;
}

media_status_t AImageReader_getHeight(const AImageReader *reader, int32_t *height) {
    if (reader == nullptr || height == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    *height = reader->height;
    return AMEDIA_OK;
}

media_status_t AImageReader_getFormat(const AImageReader *reader, int32_t *format) {
    if (reader == nullptr || format == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    *format = reader->format;
    return AMEDIA_OK;
}

media_status_t AImageReader_getMaxImages(const AImageReader *reader, int32_t *maxImages) {
    if (reader == nullptr || maxImages == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    *maxImages = reader->max_images;
    return AMEDIA_OK;
}

media_status_t AImageReader_acquireNextImage(AImageReader *reader, AImage **image) {
    return acquire(reader, image, nullptr, false);
}

media_status_t AImageReader_acquireLatestImage(AImageReader *reader, AImage **image) {
    return acquire(reader, image, nullptr, true);
}

// A NULL fence asks for the synchronous wait, as the tag's; Minato's frames have no fence to wait for.
media_status_t AImageReader_acquireNextImageAsync(AImageReader *reader, AImage **image, int *acquireFenceFd) {
    return acquire(reader, image, acquireFenceFd, false);
}

media_status_t AImageReader_acquireLatestImageAsync(AImageReader *reader, AImage **image, int *acquireFenceFd) {
    return acquire(reader, image, acquireFenceFd, true);
}

media_status_t AImageReader_setImageListener(AImageReader *reader, AImageReader_ImageListener *listener) {
    if (reader == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Hub *hub = reader->hub;
    // The reader's thread, from its first listener to its delete, started under the hub's lock, which the thread
    // takes first: no call runs before it counts as started.
    pthread_mutex_lock(&hub->lock);
    hub->listening = listener != nullptr;
    hub->listener = listener != nullptr ? *listener : AImageReader_ImageListener{};
    hub->reader = reader;
    if (listener != nullptr && !hub->dispatching && !hub->stopped) {
        hub->references++;  // the thread's; the queue's keeps it above 0 should the start fail
        if (pthread_create(&hub->dispatcher, nullptr, dispatch, hub) == 0) {
            hub->dispatching = true;
        } else {
            hub->references--;
        }
    }
    pthread_mutex_unlock(&hub->lock);
    return AMEDIA_OK;
}

media_status_t AImageReader_setBufferRemovedListener(AImageReader *reader,
                                                    AImageReader_BufferRemovedListener *listener) {
    if (reader == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&reader->lock);
    reader->removed_listener = listener != nullptr ? *listener : AImageReader_BufferRemovedListener{};
    pthread_mutex_unlock(&reader->lock);
    return AMEDIA_OK;
}

void AImage_deleteAsync(AImage *image, int) {
    if (image == nullptr) return;
    AImageReader *reader = image->reader;
    pthread_mutex_lock(&reader->lock);
    close_image(image);
    unlink_locked(image);
    pthread_mutex_unlock(&reader->lock);
    unref(reader);  // the image's reference
}

void AImage_delete(AImage *image) {
    AImage_deleteAsync(image, -1);
}

}  // extern "C"

namespace {

// The image's buffer description under its reader's lock; false for a closed image.
bool describe(const AImage *image, AHardwareBuffer_Desc *description) {
    pthread_mutex_lock(&image->reader->lock);
    const bool open = image->buffer != nullptr;
    if (open) AHardwareBuffer_describe(image->buffer, description);
    pthread_mutex_unlock(&image->reader->lock);
    return open;
}

// The image's planes: one for the RGB formats, three for YUV 420, none for PRIVATE, as ImageUtils counts them.
int32_t planes_of(const AImage *image) {
    return image->reader->hal == kHalPrivate ? 0 : image->reader->hal == kHalYCbCr420 ? 3 : 1;
}

// The image's buffer locked for the CPU at the first call, under its reader's lock: AMEDIA_OK with its address.
media_status_t lock_image(AImage *image, void **address, AHardwareBuffer_Desc *description) {
    AImageReader *reader = image->reader;
    pthread_mutex_lock(&reader->lock);
    media_status_t status = AMEDIA_OK;
    if (image->buffer == nullptr) {
        status = AMEDIA_ERROR_INVALID_OBJECT;
    } else if ((reader->usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) == 0) {
        status = AMEDIA_IMGREADER_CANNOT_LOCK_IMAGE;
    } else if (image->locked == nullptr && reader->hal == kHalYCbCr420) {
        AHardwareBuffer_Planes planes{};
        if (AHardwareBuffer_lockPlanes(image->buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &planes) !=
            0) {
            status = AMEDIA_IMGREADER_CANNOT_LOCK_IMAGE;
        } else if (planes.planeCount != 3) {
            AHardwareBuffer_unlock(image->buffer, nullptr);
            status = AMEDIA_IMGREADER_CANNOT_LOCK_IMAGE;
        } else {
            image->planes = planes;
            image->locked = planes.planes[0].data;
        }
    } else if (image->locked == nullptr &&
               AHardwareBuffer_lock(image->buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr,
                                    &image->locked) != 0) {
        image->locked = nullptr;
        status = AMEDIA_IMGREADER_CANNOT_LOCK_IMAGE;
    }
    if (status == AMEDIA_OK) {
        *address = image->locked;
        AHardwareBuffer_describe(image->buffer, description);
    }
    pthread_mutex_unlock(&reader->lock);
    return status;
}

}  // namespace

extern "C" {

media_status_t AImage_getWidth(const AImage *image, int32_t *width) {
    if (image == nullptr || width == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    AHardwareBuffer_Desc description{};
    if (!describe(image, &description)) {
        *width = -1;
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *width = image->width;
    return AMEDIA_OK;
}

media_status_t AImage_getHeight(const AImage *image, int32_t *height) {
    if (image == nullptr || height == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    AHardwareBuffer_Desc description{};
    if (!describe(image, &description)) {
        *height = -1;
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *height = image->height;
    return AMEDIA_OK;
}

media_status_t AImage_getFormat(const AImage *image, int32_t *format) {
    if (image == nullptr || format == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    AHardwareBuffer_Desc description{};
    if (!describe(image, &description)) {
        *format = -1;
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *format = image->reader->format;
    return AMEDIA_OK;
}

media_status_t AImage_getCropRect(const AImage *image, AImageCropRect *rect) {
    if (image == nullptr || rect == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    AHardwareBuffer_Desc description{};
    if (!describe(image, &description)) return AMEDIA_ERROR_INVALID_OBJECT;
    *rect = {0, 0, image->width, image->height};
    return AMEDIA_OK;
}

media_status_t AImage_getTimestamp(const AImage *image, int64_t *timestampNs) {
    if (image == nullptr || timestampNs == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    AHardwareBuffer_Desc description{};
    if (!describe(image, &description)) {
        *timestampNs = -1;
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *timestampNs = image->timestamp;
    return AMEDIA_OK;
}

media_status_t AImage_getDataSpace(const AImage *image, int32_t *dataSpace) {
    if (image == nullptr || dataSpace == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    AHardwareBuffer_Desc description{};
    if (!describe(image, &description)) {
        *dataSpace = -1;
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *dataSpace = image->data_space;
    return AMEDIA_OK;
}

// One plane for the RGB formats, none for PRIVATE, as ImageUtils counts them.
media_status_t AImage_getNumberOfPlanes(const AImage *image, int32_t *numPlanes) {
    if (image == nullptr || numPlanes == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    AHardwareBuffer_Desc description{};
    if (!describe(image, &description)) {
        *numPlanes = -1;
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *numPlanes = planes_of(image);
    return AMEDIA_OK;
}

media_status_t AImage_getPlanePixelStride(const AImage *image, int planeIdx, int32_t *pixelStride) {
    if (image == nullptr || pixelStride == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    void *address = nullptr;
    AHardwareBuffer_Desc description{};
    const media_status_t locked = lock_image(checked(image), &address, &description);
    if (locked != AMEDIA_OK) return locked;
    if (planeIdx < 0 || planeIdx >= planes_of(image)) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (image->reader->hal == kHalYCbCr420) {
        *pixelStride = static_cast<int32_t>(image->planes.planes[planeIdx].pixelStride);
        return AMEDIA_OK;
    }
    const int32_t bytes = pixel_bytes(description.format);
    if (bytes == 0) return AMEDIA_ERROR_UNSUPPORTED;
    *pixelStride = bytes;
    return AMEDIA_OK;
}

media_status_t AImage_getPlaneRowStride(const AImage *image, int planeIdx, int32_t *rowStride) {
    if (image == nullptr || rowStride == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    void *address = nullptr;
    AHardwareBuffer_Desc description{};
    const media_status_t locked = lock_image(checked(image), &address, &description);
    if (locked != AMEDIA_OK) return locked;
    if (planeIdx < 0 || planeIdx >= planes_of(image)) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (image->reader->hal == kHalYCbCr420) {
        *rowStride = static_cast<int32_t>(image->planes.planes[planeIdx].rowStride);
        return AMEDIA_OK;
    }
    const int32_t bytes = pixel_bytes(description.format);
    if (bytes == 0) return AMEDIA_ERROR_UNSUPPORTED;
    *rowStride = static_cast<int32_t>(description.stride) * bytes;
    return AMEDIA_OK;
}

media_status_t AImage_getPlaneData(const AImage *image, int planeIdx, uint8_t **data, int *dataLength) {
    if (image == nullptr || data == nullptr || dataLength == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    void *address = nullptr;
    AHardwareBuffer_Desc description{};
    const media_status_t locked = lock_image(checked(image), &address, &description);
    if (locked != AMEDIA_OK) return locked;
    if (planeIdx < 0 || planeIdx >= planes_of(image)) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (image->reader->hal == kHalYCbCr420) {  // from the plane's first sample to its last
        const AHardwareBuffer_Plane &plane = image->planes.planes[planeIdx];
        const int64_t width = planeIdx == 0 ? description.width : (description.width + 1) / 2;
        const int64_t height = planeIdx == 0 ? description.height : (description.height + 1) / 2;
        *data = static_cast<uint8_t *>(plane.data);
        *dataLength =
            static_cast<int>(int64_t{plane.rowStride} * (height - 1) + int64_t{plane.pixelStride} * (width - 1) + 1);
        return AMEDIA_OK;
    }
    const int32_t bytes = pixel_bytes(description.format);
    if (bytes == 0) return AMEDIA_ERROR_UNSUPPORTED;
    const int32_t row = static_cast<int32_t>(description.stride) * bytes;
    *data = static_cast<uint8_t *>(address);
    *dataLength = row * static_cast<int32_t>(description.height - 1) + static_cast<int32_t>(description.width) * bytes;
    return AMEDIA_OK;
}

media_status_t AImage_getHardwareBuffer(const AImage *image, AHardwareBuffer **buffer) {
    if (image == nullptr || buffer == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&image->reader->lock);
    *buffer = image->buffer;
    pthread_mutex_unlock(&image->reader->lock);
    return *buffer != nullptr ? AMEDIA_OK : AMEDIA_ERROR_INVALID_OBJECT;
}

}  // extern "C"
