/* ADR-0058 e1)'s image-ndk-test of libmediandk's AImageReader and AImage (the graphics breadth's G3c), an NDK
   program:
   - an RGBA_8888 reader of 64 x 48 with maxImages 2: its getters and its window's size and format, nothing to
     acquire, a CPU frame through ANativeWindow_lock with the listener's call (whether on the posting thread), the
     image's size, format, timestamp, crop, planes, pixels and hardware buffer, two held and a third refused, the
     recovery after a delete, acquireLatestImage over three frames, the Async variants' fence;
   - newWithUsage's buffer usage and newWithDataSpace's data space; a PRIVATE reader: its planes, its hardware buffer,
     and what a CPU lock of its window answers;
   - a YUV_420_888 reader; a delete with an image still held.
   Each wait is bounded (two seconds). The row stride and the buffer's size are the allocator's choice (G3a's
   departure): only that they fit is printed. */
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int available;
static int on_posting_thread;
static pthread_t poster;

static void image_available(void *context, AImageReader *reader) {
    (void)context;
    (void)reader;
    pthread_mutex_lock(&lock);
    available++;
    if (pthread_equal(pthread_self(), poster)) on_posting_thread = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

/* Waits until `count` calls came, at most two seconds: how many came. */
static int await_calls(int count) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    pthread_mutex_lock(&lock);
    while (available < count && pthread_cond_timedwait(&changed, &lock, &until) == 0) {
    }
    const int came = available;
    pthread_mutex_unlock(&lock);
    return came;
}

static void reset_calls(void) {
    pthread_mutex_lock(&lock);
    available = 0;
    pthread_mutex_unlock(&lock);
}

/* A CPU frame of one color (bytes R, G, B, A) through the window. */
static int post(ANativeWindow *window, uint32_t rgba) {
    ANativeWindow_Buffer buffer;
    const int locked = ANativeWindow_lock(window, &buffer, NULL);
    if (locked != 0) return locked;
    for (int32_t y = 0; y < buffer.height; y++) {
        for (int32_t x = 0; x < buffer.width; x++) ((uint32_t *)buffer.bits)[y * buffer.stride + x] = rgba;
    }
    return ANativeWindow_unlockAndPost(window);
}

static void describe_image(const char *what, AImage *image) {
    int32_t width = -1, height = -1, format = -1, planes = -1, pixel_stride = -1, row_stride = -1, length = -1;
    int64_t timestamp = -1;
    AImageCropRect crop = {-1, -1, -1, -1};
    uint8_t *data = NULL;
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);
    AImage_getFormat(image, &format);
    AImage_getTimestamp(image, &timestamp);
    AImage_getCropRect(image, &crop);
    AImage_getNumberOfPlanes(image, &planes);
    media_status_t pixel = AImage_getPlanePixelStride(image, 0, &pixel_stride);
    media_status_t row = AImage_getPlaneRowStride(image, 0, &row_stride);
    media_status_t got = AImage_getPlaneData(image, 0, &data, &length);
    printf("%s: %d x %d, format %d, timestamp set %d, crop %d %d %d %d, %d planes, pixel stride %d (%d), "
           "row stride fits %d (%d), data %d", what, width, height, format, timestamp > 0, crop.left, crop.top,
           crop.right, crop.bottom, planes, pixel_stride, pixel, row_stride >= width * pixel_stride, row, got);
    if (got == AMEDIA_OK && data != NULL) {
        printf(", pixel (1, 1) %02x%02x%02x%02x, length covers %d", data[row_stride + 4], data[row_stride + 5],
               data[row_stride + 6], data[row_stride + 7], length >= row_stride * (height - 1) + width * pixel_stride);
    }
    printf("\n");
    AHardwareBuffer *buffer = NULL;
    media_status_t hardware = AImage_getHardwareBuffer(image, &buffer);
    if (hardware == AMEDIA_OK && buffer != NULL) {
        AHardwareBuffer_Desc description;
        AHardwareBuffer_describe(buffer, &description);
        printf("%s's hardware buffer: %u x %u, format %u, CPU read %d, GPU sampled %d\n", what, description.width,
               description.height, description.format,
               (description.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) != 0,
               (description.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) != 0);
    } else {
        printf("%s's hardware buffer: status %d\n", what, hardware);
    }
}

static void rgba(void) {
    AImageReader *reader = NULL;
    printf("new: %d\n", AImageReader_new(64, 48, AIMAGE_FORMAT_RGBA_8888, 2, &reader));
    int32_t width, height, format, max;
    AImageReader_getWidth(reader, &width);
    AImageReader_getHeight(reader, &height);
    AImageReader_getFormat(reader, &format);
    AImageReader_getMaxImages(reader, &max);
    ANativeWindow *window = NULL;
    media_status_t got_window = AImageReader_getWindow(reader, &window);
    printf("reader: %d x %d, format %d, maxImages %d; window %d, %d x %d, format %d\n", width, height, format, max,
           got_window, ANativeWindow_getWidth(window), ANativeWindow_getHeight(window),
           ANativeWindow_getFormat(window));
    AImage *image = NULL;
    printf("nothing queued: next %d, latest %d\n", AImageReader_acquireNextImage(reader, &image),
           AImageReader_acquireLatestImage(reader, &image));

    AImageReader_ImageListener listener = {NULL, image_available};
    printf("setImageListener: %d\n", AImageReader_setImageListener(reader, &listener));
    reset_calls();
    poster = pthread_self();
    printf("post: %d\n", post(window, 0xff0000ffu));
    printf("onImageAvailable: %d, on the posting thread %d\n", await_calls(1), on_posting_thread);
    AImage *first = NULL;
    printf("acquireNextImage: %d\n", AImageReader_acquireNextImage(reader, &first));
    describe_image("first", first);

    post(window, 0xff00ff00u);
    await_calls(2);
    AImage *second = NULL;
    printf("second while the first is held: %d\n", AImageReader_acquireNextImage(reader, &second));
    post(window, 0xffff0000u);
    await_calls(3);
    AImage *third = NULL;
    printf("a third while two are held: %d\n", AImageReader_acquireNextImage(reader, &third));
    AImage_delete(first);
    printf("after a delete: %d\n", AImageReader_acquireNextImage(reader, &third));
    AImage_delete(second);
    AImage_delete(third);

    reset_calls();
    post(window, 0xffffffffu);
    post(window, 0xff808080u);
    post(window, 0xff000000u);
    const int calls = await_calls(3);
    AImage *latest = NULL;
    media_status_t got_latest = AImageReader_acquireLatestImage(reader, &latest);
    uint8_t *data = NULL;
    int32_t length = 0;
    AImage_getPlaneData(latest, 0, &data, &length);
    printf("three frames: onImageAvailable %d, latest %d, its pixel %02x, then next %d\n", calls, got_latest,
           data != NULL ? data[0] : 0, AImageReader_acquireNextImage(reader, &image));
    AImage_delete(latest);

    reset_calls();
    post(window, 0xff0000ffu);
    await_calls(1);
    int fence = -2;
    AImage *async = NULL;
    printf("acquireNextImageAsync: %d, fence %d\n", AImageReader_acquireNextImageAsync(reader, &async, &fence), fence);
    AImage_deleteAsync(async, -1);
    AImageReader_setImageListener(reader, NULL);
    AImageReader_delete(reader);
}

static void usage_and_data_space(void) {
    AImageReader *reader = NULL;
    printf("newWithUsage: %d\n", AImageReader_newWithUsage(16, 16, AIMAGE_FORMAT_RGBA_8888,
                                                            AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                                                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                                                            1, &reader));
    ANativeWindow *window = NULL;
    AImageReader_getWindow(reader, &window);
    post(window, 0xff00ff00u);
    AImage *image = NULL;
    for (int tries = 0; tries < 200 && AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK; tries++) {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    describe_image("with usage", image);
    AImage_delete(image);
    AImageReader_delete(reader);

    reader = NULL;
    printf("newWithDataSpace: %d\n", AImageReader_newWithDataSpace(16, 16, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 1,
                                                                   AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                                                   ADATASPACE_SRGB, &reader));
    AImageReader_getWindow(reader, &window);
    printf("its window's data space: %d\n", ANativeWindow_getBuffersDataSpace(window));
    post(window, 0xff00ff00u);
    image = NULL;
    for (int tries = 0; tries < 200 && AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK; tries++) {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    int32_t data_space = -1;
    printf("image data space: %d, %d\n", AImage_getDataSpace(image, &data_space), data_space);
    AImage_delete(image);
    AImageReader_delete(reader);
}

static void opaque(void) {
    AImageReader *reader = NULL;
    printf("PRIVATE: %d\n", AImageReader_new(16, 16, AIMAGE_FORMAT_PRIVATE, 1, &reader));
    ANativeWindow *window = NULL;
    AImageReader_getWindow(reader, &window);
    printf("its window's format: %d\n", ANativeWindow_getFormat(window));
    ANativeWindow_Buffer buffer;
    const int locked = ANativeWindow_lock(window, &buffer, NULL);
    printf("a CPU lock of its window: %d\n", locked);
    if (locked == 0) ANativeWindow_unlockAndPost(window);
    AImageReader_delete(reader);
}

/* A YUV reader is made (our buffers have no YUV format, so a producer cannot dequeue for it: G3a's named limit).
   maxImages 0 is not asked: Android's validation is not checked, and BufferItemConsumer aborts the process. */
static void invalid(void) {
    AImageReader *reader = NULL;
    printf("YUV_420_888: %d\n", AImageReader_new(16, 16, AIMAGE_FORMAT_YUV_420_888, 1, &reader));
    if (reader != NULL) AImageReader_delete(reader);
}

static void delete_while_held(void) {
    AImageReader *reader = NULL;
    AImageReader_new(16, 16, AIMAGE_FORMAT_RGBA_8888, 1, &reader);
    ANativeWindow *window = NULL;
    AImageReader_getWindow(reader, &window);
    ANativeWindow_acquire(window);
    post(window, 0xff00ff00u);
    AImage *image = NULL;
    for (int tries = 0; tries < 200 && AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK; tries++) {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    AImageReader_delete(reader);
    printf("deleted with an image held; its window after: lock %d\n", post(window, 0xff00ff00u));
    ANativeWindow_release(window);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    rgba();
    usage_and_data_space();
    opaque();
    invalid();
    delete_while_held();
    printf("done\n");
    return 0;
}
