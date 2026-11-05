// ADR-0060 e2b's test (media-codec-test): the JNI library MediaCodecApplication loads, which decodes a stream
// through the NDK's AMediaCodec in ByteBuffer mode, with the decoder the Java side names, and logs each frame as the
// Java side does: its time, flags and a CRC32 of its Y, U and V samples inside the crop, walked by the output
// format's stride, slice height and color format (planar or semi-planar), so that decoders of either layout compare.
// An output format is logged when its size or crop differs from the last one's: a decoder may announce the same
// format again (AOSP's c2.android.avc.decoder does after the first frame, as its timing has it). A line that starts
// with "device " goes to the device tag. ADR-0060 e2c: the same through an AImageReader's window in YUV_420_888, each
// frame rendered and the reader's image logged, walked by its planes' strides; a reader of one image held while the
// codec renders on (how long each release takes, what a lock of the window answers, which frame comes after the
// hold); and a reader in RGB_565, whose format a decoder cannot draw.
#include <EGL/egl.h>
#include <android/native_window.h>
#include <jni.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#define NAME(name) Java_io_github_kisame76_minato_test_mediacodec_MediaCodecApplication_##name

enum { kPlanar = 19, kSemiPlanar = 21, kFlexible = 0x7f420888 };

struct layout {
    int32_t width, height, stride, slice, color, left, top, right, bottom;
};

static void append(char *log, size_t size, const char *line) {
    const size_t used = strlen(log);
    if (used + strlen(line) + 2 < size) {
        strcat(log, line);
        strcat(log, "\n");
    }
}

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void read_format(AMediaCodec *codec, struct layout *layout, char *log, size_t size) {
    AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
    const struct layout last = *layout;
    memset(layout, 0, sizeof *layout);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &layout->width);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &layout->height);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &layout->stride);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &layout->slice);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &layout->color);
    char line[300];
    const int cropped = AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &layout->left, &layout->top,
                                             &layout->right, &layout->bottom);
    if (!cropped) layout->right = layout->width - 1, layout->bottom = layout->height - 1;
    const int same = last.width == layout->width && last.height == layout->height && last.left == layout->left &&
                     last.top == layout->top && last.right == layout->right && last.bottom == layout->bottom;
    if (cropped) {
        snprintf(line, sizeof line, "ndk: output format %d x %d, crop %d %d %d %d", layout->width, layout->height,
                 layout->left, layout->top, layout->right, layout->bottom);
    } else {
        snprintf(line, sizeof line, "ndk: output format %d x %d, no crop", layout->width, layout->height);
    }
    if (!same) append(log, size, line);
    snprintf(line, sizeof line, "device ndk: color format %#x, stride %d, slice height %d", layout->color,
             layout->stride, layout->slice);
    if (!same) append(log, size, line);
    if (layout->stride <= 0) layout->stride = layout->width;
    if (layout->slice <= 0) layout->slice = layout->height;
    AMediaFormat_delete(format);
}

// The CRC32 of one plane's samples inside the crop: rows of `stride` bytes from `plane`, a sample every `step`.
static unsigned long plane_crc(const uint8_t *plane, int32_t stride, int32_t step, int32_t left, int32_t top,
                               int32_t right, int32_t bottom) {
    unsigned long crc = crc32(0L, Z_NULL, 0);
    for (int32_t y = top; y <= bottom; y++) {
        for (int32_t x = left; x <= right; x++) {
            crc = crc32(crc, plane + (size_t)y * stride + (size_t)x * step, 1);
        }
    }
    return crc;
}

static void frame(const struct layout *layout, const uint8_t *data, size_t size, const AMediaCodecBufferInfo *info,
                  char *log, size_t log_size) {
    char line[300];
    const int32_t stride = layout->stride, slice = layout->slice;
    const uint8_t *y = data + info->offset, *chroma = y + (size_t)stride * slice;
    const int32_t l = layout->left, t = layout->top, r = layout->right, b = layout->bottom;
    unsigned long cy = plane_crc(y, stride, 1, l, t, r, b), cu, cv;
    if (layout->color == kSemiPlanar) {
        cu = plane_crc(chroma, stride, 2, l / 2, t / 2, r / 2, b / 2);
        cv = plane_crc(chroma + 1, stride, 2, l / 2, t / 2, r / 2, b / 2);
    } else {  // planar, as the flexible format's buffers are in ByteBuffer mode
        const uint8_t *v = chroma + (size_t)(stride / 2) * (slice / 2);
        cu = plane_crc(chroma, stride / 2, 1, l / 2, t / 2, r / 2, b / 2);
        cv = plane_crc(v, stride / 2, 1, l / 2, t / 2, r / 2, b / 2);
    }
    (void)size;
    snprintf(line, sizeof line, "ndk: pts %lld, flags %u, Y %lx U %lx V %lx", (long long)info->presentationTimeUs,
             info->flags, cy, cu, cv);
    append(log, log_size, line);
}

// An image of the reader, logged as a frame is: its time in microseconds, crop and the CRC32 of each plane's samples.
static void image_line(AImage *image, char *log, size_t log_size) {
    int64_t time = 0;
    int32_t format = 0;
    AImageCropRect crop = {0, 0, 0, 0};
    AImage_getTimestamp(image, &time);
    AImage_getFormat(image, &format);
    AImage_getCropRect(image, &crop);
    unsigned long crcs[3] = {0, 0, 0};
    for (int p = 0; p < 3; p++) {
        uint8_t *data = NULL;
        int length = 0;
        int32_t row = 0, step = 0;
        AImage_getPlaneData(image, p, &data, &length);
        AImage_getPlaneRowStride(image, p, &row);
        AImage_getPlanePixelStride(image, p, &step);
        const int shift = p == 0 ? 0 : 1;
        crcs[p] = data == NULL ? 0
                               : plane_crc(data, row, step, crop.left >> shift, crop.top >> shift,
                                           ((crop.right + shift) >> shift) - 1, ((crop.bottom + shift) >> shift) - 1);
    }
    char line[300];
    snprintf(line, sizeof line, "ndk reader: pts %lld, %d x %d, format %d, Y %lx U %lx V %lx",
             (long long)(time / 1000), crop.right - crop.left, crop.bottom - crop.top, format, crcs[0], crcs[1],
             crcs[2]);
    append(log, log_size, line);
}

// Decodes the stream with the named decoder: into ByteBuffers, or with `to_reader` into an AImageReader's window.
static jstring decode_with(JNIEnv *env, jstring decoder, jbyteArray stream, jlongArray index, int to_reader) {
    static char log[16384];
    log[0] = 0;
    jbyte *bytes = (*env)->GetByteArrayElements(env, stream, NULL);
    jlong *units = (*env)->GetLongArrayElements(env, index, NULL);
    const jsize count = (*env)->GetArrayLength(env, index) / 4;
    const char *name = decoder != NULL ? (*env)->GetStringUTFChars(env, decoder, NULL) : NULL;
    AMediaCodec *codec = name != NULL ? AMediaCodec_createCodecByName(name) : NULL;
    if (name != NULL) (*env)->ReleaseStringUTFChars(env, decoder, name);
    if (codec == NULL) {
        append(log, sizeof log, "ndk: no decoder");
    } else {
        AMediaFormat *format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 320);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 180);
        char line[200];
        AImageReader *reader = NULL;
        ANativeWindow *window = NULL;
        if (to_reader) {
            snprintf(line, sizeof line, "ndk reader: new %d, window %d",
                     AImageReader_new(320, 180, AIMAGE_FORMAT_YUV_420_888, 3, &reader),
                     reader != NULL ? AImageReader_getWindow(reader, &window) : -1);
            append(log, sizeof log, line);
        }
        snprintf(line, sizeof line, "%s: configure %d, start %d", to_reader ? "ndk reader" : "ndk",
                 AMediaCodec_configure(codec, format, window, NULL, 0), AMediaCodec_start(codec));
        append(log, sizeof log, line);
        AMediaFormat_delete(format);
        struct layout layout = {0};
        jsize next = 0;
        int frames = 0, sent_end = 0, ended = 0;
        const int64_t deadline = now_ms() + 10000;
        while (!ended && now_ms() < deadline) {
            if (!sent_end) {
                const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 10000);
                if (in >= 0) {
                    size_t capacity = 0;
                    uint8_t *input = AMediaCodec_getInputBuffer(codec, (size_t)in, &capacity);
                    if (next < count && input != NULL && (size_t)units[next * 4 + 1] <= capacity) {
                        memcpy(input, bytes + units[next * 4], (size_t)units[next * 4 + 1]);
                        AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, (size_t)units[next * 4 + 1],
                                                     (uint64_t)units[next * 4 + 2], 0);
                        next++;
                    } else {
                        AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, 0, 0,
                                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                        sent_end = 1;
                    }
                }
            }
            AMediaCodecBufferInfo info;
            const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
            if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                if (!to_reader) read_format(codec, &layout, log, sizeof log);
            } else if (out >= 0 && to_reader) {
                ended = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
                const int render = !ended || info.size > 0;  // a frame, whatever size a surface's buffer has
                if (frames == 0) {
                    snprintf(line, sizeof line, "device ndk reader: a rendered buffer's size %d", info.size);
                    append(log, sizeof log, line);
                }
                AMediaCodec_releaseOutputBuffer(codec, (size_t)out, render);
                if (!render) continue;
                AImage *image = NULL;
                for (const int64_t wait = now_ms() + 2000; image == NULL && now_ms() < wait;) {
                    if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK) {
                        image = NULL;
                        struct timespec pause = {0, 2000000};
                        nanosleep(&pause, NULL);
                    }
                }
                if (image == NULL) {
                    snprintf(line, sizeof line, "ndk reader: pts %lld, no image", (long long)info.presentationTimeUs);
                    append(log, sizeof log, line);
                } else {
                    image_line(image, log, sizeof log);
                    AImage_delete(image);
                    frames++;
                }
            } else if (out >= 0) {
                size_t size = 0;
                const uint8_t *data = AMediaCodec_getOutputBuffer(codec, (size_t)out, &size);
                if (data != NULL && info.size > 0) {
                    frame(&layout, data, size, &info, log, sizeof log);
                    frames++;
                }
                AMediaCodec_releaseOutputBuffer(codec, (size_t)out, false);
                ended = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            }
        }
        snprintf(line, sizeof line, "%s: %d frames%s, stop %d, delete %d", to_reader ? "ndk reader" : "ndk", frames,
                 ended ? ", end of stream" : "", AMediaCodec_stop(codec), AMediaCodec_delete(codec));
        append(log, sizeof log, line);
        if (reader != NULL) AImageReader_delete(reader);
    }
    (*env)->ReleaseLongArrayElements(env, index, units, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, stream, bytes, JNI_ABORT);
    return (*env)->NewStringUTF(env, log);
}

JNIEXPORT jstring JNICALL NAME(decodeNative)(JNIEnv *env, jclass type, jstring decoder, jbyteArray stream,
                                             jlongArray index) {
    (void)type;
    return decode_with(env, decoder, stream, index, 0);
}

JNIEXPORT jstring JNICALL NAME(decodeToReaderNative)(JNIEnv *env, jclass type, jstring decoder, jbyteArray stream,
                                                     jlongArray index) {
    (void)type;
    return decode_with(env, decoder, stream, index, 1);
}

// The next access unit into an input buffer if one is free, the end of the stream after the last. Whether the end is
// sent.
static int feed(AMediaCodec *codec, const jbyte *bytes, const jlong *units, jsize count, jsize *next) {
    const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 10000);
    if (in < 0) return 0;
    size_t capacity = 0;
    uint8_t *input = AMediaCodec_getInputBuffer(codec, (size_t)in, &capacity);
    if (*next < count && input != NULL && (size_t)units[*next * 4 + 1] <= capacity) {
        memcpy(input, bytes + units[*next * 4], (size_t)units[*next * 4 + 1]);
        AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, (size_t)units[*next * 4 + 1],
                                     (uint64_t)units[*next * 4 + 2], 0);
        (*next)++;
        return 0;
    }
    AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    return 1;
}

static AMediaCodec *configured(JNIEnv *env, jstring decoder, ANativeWindow *window, const char *name, char *log,
                               size_t size) {
    const char *chosen = decoder != NULL ? (*env)->GetStringUTFChars(env, decoder, NULL) : NULL;
    AMediaCodec *codec = chosen != NULL ? AMediaCodec_createCodecByName(chosen) : NULL;
    if (chosen != NULL) (*env)->ReleaseStringUTFChars(env, decoder, chosen);
    if (codec == NULL) return NULL;
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 320);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 180);
    const media_status_t configure = AMediaCodec_configure(codec, format, window, NULL, 0);
    AMediaFormat_delete(format);
    const media_status_t start = configure == AMEDIA_OK ? AMediaCodec_start(codec) : AMEDIA_ERROR_INVALID_OPERATION;
    char line[200];
    snprintf(line, sizeof line, "%s: configure %d, start %d", name, configure, start);
    append(log, size, line);
    return codec;
}

// The next image of the reader within `wait_ms`, or NULL.
static AImage *next_image(AImageReader *reader, int wait_ms, media_status_t *status) {
    AImage *image = NULL;
    const int64_t until = now_ms() + wait_ms;
    do {
        *status = AImageReader_acquireNextImage(reader, &image);
        if (*status == AMEDIA_OK) return image;
        struct timespec pause = {0, 2000000};
        nanosleep(&pause, NULL);
    } while (now_ms() < until);
    return NULL;
}

// Whether EGL makes a window surface on the window; its error to the device tag (EGL_BAD_ALLOC on Android and Minato,
// as the window's connect fails).
static void window_surface(ANativeWindow *window, char *log, size_t size) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    const EGLint spec[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                           EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = NULL;
    EGLint configs = 0;
    eglChooseConfig(display, spec, &config, 1, &configs);
    const EGLSurface surface = configs > 0 ? eglCreateWindowSurface(display, config, window, NULL) : EGL_NO_SURFACE;
    const EGLint error = eglGetError();
    char line[200];
    snprintf(line, sizeof line, "ndk hold: a window surface while the codec draws %s",
             surface != EGL_NO_SURFACE ? "made" : "none");
    append(log, size, line);
    snprintf(line, sizeof line, "device ndk hold: the window surface's error 0x%x", error);
    append(log, size, line);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
}

// A reader of one image whose first image the test holds while the codec renders every frame of the stream: what a
// CPU lock of the window, a second codec's configure and an EGL window surface answer while the codec is its producer
// and a lock after it,
// whether each release returns within 100 ms, whether the stream still ends, what acquiring a second image answers,
// and the times of the images after the hold.
static jstring hold(JNIEnv *env, jstring decoder, jbyteArray stream, jlongArray index) {
    static char log[8192];
    log[0] = 0;
    char line[200];
    jbyte *bytes = (*env)->GetByteArrayElements(env, stream, NULL);
    jlong *units = (*env)->GetLongArrayElements(env, index, NULL);
    const jsize count = (*env)->GetArrayLength(env, index) / 4;
    AImageReader *reader = NULL;
    ANativeWindow *window = NULL;
    snprintf(line, sizeof line, "ndk hold: new %d, window %d",
             AImageReader_new(320, 180, AIMAGE_FORMAT_YUV_420_888, 1, &reader),
             reader != NULL ? AImageReader_getWindow(reader, &window) : -1);
    append(log, sizeof log, line);
    AMediaCodec *codec = window != NULL ? configured(env, decoder, window, "ndk hold", log, sizeof log) : NULL;
    if (codec != NULL) {
        ANativeWindow_Buffer locked;
        const int32_t lock = ANativeWindow_lock(window, &locked, NULL);
        if (lock == 0) ANativeWindow_unlockAndPost(window);
        snprintf(line, sizeof line, "ndk hold: a lock while the codec draws %d", lock);
        append(log, sizeof log, line);
        AMediaCodec *other = configured(env, decoder, window, "ndk hold: a second codec", log, sizeof log);
        if (other != NULL) AMediaCodec_delete(other);
        window_surface(window, log, sizeof log);
        AImage *held = NULL;
        jsize next = 0;
        int rendered = 0, sent_end = 0, ended = 0;
        int64_t longest = 0;
        const int64_t deadline = now_ms() + 5000;
        while (!ended && now_ms() < deadline) {
            if (!sent_end) sent_end = feed(codec, bytes, units, count, &next);
            AMediaCodecBufferInfo info;
            const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
            if (out < 0) continue;
            ended = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            const int render = !ended || info.size > 0;
            const int64_t before = now_ms();
            AMediaCodec_releaseOutputBuffer(codec, (size_t)out, render);
            if (now_ms() - before > longest) longest = now_ms() - before;
            if (!render) continue;
            rendered++;
            media_status_t status;
            if (held == NULL) held = next_image(reader, 2000, &status);
        }
        snprintf(line, sizeof line, "ndk hold: %d rendered%s, every release within 100 ms %s, first held %s",
                 rendered, ended ? ", end of stream" : "", longest <= 100 ? "yes" : "no", held != NULL ? "yes" : "no");
        append(log, sizeof log, line);
        snprintf(line, sizeof line, "device ndk hold: longest release %lld ms", (long long)longest);
        append(log, sizeof log, line);
        AImage *second = NULL;
        snprintf(line, sizeof line, "ndk hold: a second image while held %d", AImageReader_acquireNextImage(reader,
                                                                                                          &second));
        append(log, sizeof log, line);
        if (second != NULL) AImage_delete(second);
        if (held != NULL) AImage_delete(held);
        media_status_t status = AMEDIA_OK;
        for (AImage *image; (image = next_image(reader, 500, &status)) != NULL;) {
            int64_t time = 0;
            AImage_getTimestamp(image, &time);
            snprintf(line, sizeof line, "ndk hold: after the hold pts %lld", (long long)(time / 1000));
            append(log, sizeof log, line);
            AImage_delete(image);
        }
        snprintf(line, sizeof line, "ndk hold: then %d, stop %d, delete %d", status, AMediaCodec_stop(codec),
                 AMediaCodec_delete(codec));
        append(log, sizeof log, line);
        const int32_t after = ANativeWindow_lock(window, &locked, NULL);
        if (after == 0) ANativeWindow_unlockAndPost(window);
        snprintf(line, sizeof line, "ndk hold: a lock after the codec %d", after);
        append(log, sizeof log, line);
    }
    if (reader != NULL) AImageReader_delete(reader);
    (*env)->ReleaseLongArrayElements(env, index, units, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, stream, bytes, JNI_ABORT);
    return (*env)->NewStringUTF(env, log);
}

// A reader in RGB_565: configure's answer, and what the reader gives for the first frames rendered.
static jstring rgb565(JNIEnv *env, jstring decoder, jbyteArray stream, jlongArray index) {
    static char log[4096];
    log[0] = 0;
    char line[200];
    jbyte *bytes = (*env)->GetByteArrayElements(env, stream, NULL);
    jlong *units = (*env)->GetLongArrayElements(env, index, NULL);
    const jsize count = (*env)->GetArrayLength(env, index) / 4;
    AImageReader *reader = NULL;
    ANativeWindow *window = NULL;
    snprintf(line, sizeof line, "ndk rgb565: new %d, window %d",
             AImageReader_new(320, 180, AIMAGE_FORMAT_RGB_565, 2, &reader),
             reader != NULL ? AImageReader_getWindow(reader, &window) : -1);
    append(log, sizeof log, line);
    AMediaCodec *codec = window != NULL ? configured(env, decoder, window, "ndk rgb565", log, sizeof log) : NULL;
    if (codec != NULL) {
        jsize next = 0;
        int rendered = 0, sent_end = 0, ended = 0, images = 0;
        media_status_t status = AMEDIA_OK;
        const int64_t deadline = now_ms() + 5000;
        while (!ended && rendered < 3 && now_ms() < deadline) {
            if (!sent_end) sent_end = feed(codec, bytes, units, count, &next);
            AMediaCodecBufferInfo info;
            const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
            if (out < 0) continue;
            ended = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            const media_status_t release = AMediaCodec_releaseOutputBuffer(codec, (size_t)out, 1);
            rendered++;
            AImage *image = next_image(reader, 500, &status);
            int32_t format = 0;
            if (image != NULL) AImage_getFormat(image, &format);
            snprintf(line, sizeof line, "ndk rgb565: release %d, image %s, format %d", release,
                     image != NULL ? "yes" : "no", format);
            append(log, sizeof log, line);
            if (image != NULL) {
                images++;
                AImage_delete(image);
            }
        }
        snprintf(line, sizeof line, "ndk rgb565: %d rendered, %d images, last %d, stop %d, delete %d", rendered,
                 images, status, AMediaCodec_stop(codec), AMediaCodec_delete(codec));
        append(log, sizeof log, line);
    }
    if (reader != NULL) AImageReader_delete(reader);
    (*env)->ReleaseLongArrayElements(env, index, units, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, stream, bytes, JNI_ABORT);
    return (*env)->NewStringUTF(env, log);
}

JNIEXPORT jstring JNICALL NAME(holdNative)(JNIEnv *env, jclass type, jstring decoder, jbyteArray stream,
                                           jlongArray index) {
    (void)type;
    return hold(env, decoder, stream, index);
}

JNIEXPORT jstring JNICALL NAME(rgb565Native)(JNIEnv *env, jclass type, jstring decoder, jbyteArray stream,
                                             jlongArray index) {
    (void)type;
    return rgb565(env, decoder, stream, index);
}
