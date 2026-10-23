// ADR-0060 e2c's iPhone measurement (media-bench-test): the JNI library MediaBenchApplication loads, which decodes a
// stream through the NDK's AMediaCodec with the decoder the Java side names, into ByteBuffers, a YUV_420_888 or an
// RGBA_8888 AImageReader, and logs what it measured: frames per second from the first unit to the end of the stream,
// and the time each releaseOutputBuffer takes (p50, p99), which with a reader is the host's copy into its buffer. The
// background run decodes the stream again and again for a while and logs what came through.
#include <jni.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NAME(name) Java_io_github_kisame76_minato_test_mediabench_MediaBenchApplication_##name

enum { kRounds = 3, kMaxSamples = 4096 };

static const char *const kOutputs[] = {"bytes", "yuv", "rgba"};

static int64_t now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static void append(char *log, size_t size, const char *line) {
    const size_t used = strlen(log);
    if (used + strlen(line) + 2 < size) {
        strcat(log, line);
        strcat(log, "\n");
    }
}

static int compare(const void *a, const void *b) {
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* A stream's bytes and its index of access units (offset, size, time, key). */
struct stream {
    const jbyte *bytes;
    const jlong *units;
    jsize count;
};

/* A codec of the decoder for the stream's size, its output into `window` (NULL: ByteBuffers), started; NULL else. */
static AMediaCodec *started(JNIEnv *env, jstring decoder, int width, int height, ANativeWindow *window) {
    const char *chosen = decoder != NULL ? (*env)->GetStringUTFChars(env, decoder, NULL) : NULL;
    AMediaCodec *codec = chosen != NULL ? AMediaCodec_createCodecByName(chosen) : NULL;
    if (chosen != NULL) (*env)->ReleaseStringUTFChars(env, decoder, chosen);
    if (codec == NULL) return NULL;
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    const media_status_t configured = AMediaCodec_configure(codec, format, window, NULL, 0);
    AMediaFormat_delete(format);
    if (configured != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
        AMediaCodec_delete(codec);
        return NULL;
    }
    return codec;
}

/* The next unit into a free input buffer, the end of the stream after the last: 1 once the end is queued. */
static int feed(AMediaCodec *codec, const struct stream *stream, jsize *next) {
    const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 2000);
    if (in < 0) return 0;
    size_t capacity = 0;
    uint8_t *input = AMediaCodec_getInputBuffer(codec, (size_t)in, &capacity);
    if (*next < stream->count && input != NULL && (size_t)stream->units[*next * 4 + 1] <= capacity) {
        const jlong *unit = stream->units + *next * 4;
        memcpy(input, stream->bytes + unit[0], (size_t)unit[1]);
        AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, (size_t)unit[1], (uint64_t)unit[2], 0);
        (*next)++;
        return 0;
    }
    AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    return 1;
}

/* What one pass of the stream did. */
struct pass {
    int frames, images, errors;
    int64_t elapsed_us;
    int ended;
};

/* One pass of the stream through `codec`, each frame released (rendered with a reader) and its image taken; each
   release's time into `samples` while there is room; the time of the last image and the longest time between two. */
static struct pass decode_once(AMediaCodec *codec, const struct stream *stream, AImageReader *reader,
                               int64_t *samples, int *sampled, int64_t *last_image, int64_t *longest_gap) {
    struct pass pass = {0, 0, 0, 0, 0};
    jsize next = 0;
    int sent_end = 0;
    const int64_t start = now_us(), deadline = start + 20000000;
    while (!pass.ended && now_us() < deadline) {
        if (!sent_end) sent_end = feed(codec, stream, &next);
        AMediaCodecBufferInfo info;
        const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 2000);
        if (out < 0) {
            if (out != AMEDIACODEC_INFO_TRY_AGAIN_LATER && out != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
                out != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                pass.errors++;
            }
            continue;
        }
        pass.ended = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        const int frame = !pass.ended || info.size > 0;
        if (reader == NULL && frame) {
            size_t size = 0;
            (void)AMediaCodec_getOutputBuffer(codec, (size_t)out, &size);
        }
        const int64_t before = now_us();
        if (AMediaCodec_releaseOutputBuffer(codec, (size_t)out, reader != NULL && frame) != AMEDIA_OK) pass.errors++;
        const int64_t took = now_us() - before;
        if (!frame) continue;
        pass.frames++;
        if (*sampled < kMaxSamples) samples[(*sampled)++] = took;
        if (reader != NULL) {
            AImage *image = NULL;
            for (const int64_t wait = now_us() + 100000; image == NULL && now_us() < wait;) {
                if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK) {
                    image = NULL;
                    struct timespec pause = {0, 500000};
                    nanosleep(&pause, NULL);
                }
            }
            if (image != NULL) {
                const int64_t at = now_us();
                if (*last_image != 0 && at - *last_image > *longest_gap) *longest_gap = at - *last_image;
                pass.images++;
                *last_image = at;
                AImage_delete(image);
            }
        }
    }
    pass.elapsed_us = now_us() - start;
    return pass;
}

static void stream_of(JNIEnv *env, jbyteArray bytes, jlongArray index, struct stream *stream) {
    stream->bytes = (*env)->GetByteArrayElements(env, bytes, NULL);
    stream->units = (*env)->GetLongArrayElements(env, index, NULL);
    stream->count = (*env)->GetArrayLength(env, index) / 4;
}

static void release_stream(JNIEnv *env, jbyteArray bytes, jlongArray index, struct stream *stream) {
    (*env)->ReleaseLongArrayElements(env, index, (jlong *)stream->units, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, bytes, (jbyte *)stream->bytes, JNI_ABORT);
}

/* A reader of the stream's size for `output` (1 YUV_420_888, 2 RGBA_8888) and its window; NULL for ByteBuffers. */
static AImageReader *reader_for(int output, int width, int height, ANativeWindow **window) {
    AImageReader *reader = NULL;
    *window = NULL;
    if (output == 0) return NULL;
    const int32_t format = output == 1 ? AIMAGE_FORMAT_YUV_420_888 : AIMAGE_FORMAT_RGBA_8888;
    if (AImageReader_new(width, height, format, 4, &reader) != AMEDIA_OK) return NULL;
    if (AImageReader_getWindow(reader, window) != AMEDIA_OK) {
        AImageReader_delete(reader);
        return NULL;
    }
    return reader;
}

JNIEXPORT jstring JNICALL NAME(benchNative)(JNIEnv *env, jclass type, jstring decoder, jstring name,
                                            jbyteArray bytes, jlongArray index, jint width, jint height,
                                            jint output) {
    (void)type;
    static char log[2048];
    static int64_t samples[kMaxSamples];
    log[0] = 0;
    char line[300];
    const char *label = (*env)->GetStringUTFChars(env, name, NULL);
    struct stream stream;
    stream_of(env, bytes, index, &stream);
    ANativeWindow *window = NULL;
    AImageReader *reader = reader_for(output, width, height, &window);
    int frames = 0, images = 0, errors = 0, sampled = 0, made = output == 0 || reader != NULL;
    int64_t elapsed = 0, last_image = 0, gap = 0;
    for (int round = 0; made && round <= kRounds; round++) {
        AMediaCodec *codec = started(env, decoder, width, height, window);
        if (codec == NULL) {
            made = 0;
            break;
        }
        const struct pass pass = decode_once(codec, &stream, reader, samples, &sampled, &last_image, &gap);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        if (round == 0) {
            snprintf(line, sizeof line, "%s %s: warm-up %d frames, %d images in %lld ms", label, kOutputs[output],
                     pass.frames, pass.images, (long long)(pass.elapsed_us / 1000));
            append(log, sizeof log, line);
            sampled = 0;
            continue;
        }
        frames += pass.frames, images += pass.images, errors += pass.errors, elapsed += pass.elapsed_us;
        if (!pass.ended) errors++;
    }
    if (!made) {
        snprintf(line, sizeof line, "%s %s: no codec or reader", label, kOutputs[output]);
    } else {
        qsort(samples, (size_t)sampled, sizeof *samples, compare);
        const int64_t p50 = sampled > 0 ? samples[sampled / 2] : 0;
        const int64_t p99 = sampled > 0 ? samples[(sampled * 99) / 100 < sampled ? (sampled * 99) / 100 : sampled - 1]
                                        : 0;
        snprintf(line, sizeof line,
                 "%s %s: %d rounds, %d frames, %d images, %d errors, %.1f fps, release p50 %.2f ms, p99 %.2f ms",
                 label, kOutputs[output], kRounds, frames, images, errors,
                 elapsed > 0 ? frames * 1000000.0 / (double)elapsed : 0.0, p50 / 1000.0, p99 / 1000.0);
    }
    append(log, sizeof log, line);
    if (reader != NULL) AImageReader_delete(reader);
    release_stream(env, bytes, index, &stream);
    (*env)->ReleaseStringUTFChars(env, name, label);
    return (*env)->NewStringUTF(env, log);
}

JNIEXPORT jstring JNICALL NAME(backgroundNative)(JNIEnv *env, jclass type, jstring decoder, jbyteArray bytes,
                                                 jlongArray index, jint width, jint height, jint output,
                                                 jint seconds) {
    (void)type;
    static char log[1024];
    static int64_t samples[kMaxSamples];
    log[0] = 0;
    char line[300];
    struct stream stream;
    stream_of(env, bytes, index, &stream);
    ANativeWindow *window = NULL;
    AImageReader *reader = reader_for(output, width, height, &window);
    AMediaCodec *codec = reader != NULL ? started(env, decoder, width, height, window) : NULL;
    if (codec == NULL) {
        snprintf(line, sizeof line, "background %s: no codec or reader", kOutputs[output]);
        append(log, sizeof log, line);
    } else {
        int frames = 0, images = 0, errors = 0, passes = 0, flushes_failed = 0, sampled = 0;
        int64_t longest_gap = 0, last_image = now_us();
        const int64_t end = now_us() + (int64_t)seconds * 1000000;
        while (now_us() < end) {
            const struct pass pass = decode_once(codec, &stream, reader, samples, &sampled, &last_image, &longest_gap);
            frames += pass.frames, images += pass.images, errors += pass.errors, passes++;
            sampled = 0;
            // After the end of the stream the codec takes input again only after a flush, as Android's.
            if (AMediaCodec_flush(codec) != AMEDIA_OK) flushes_failed++;
        }
        if (now_us() - last_image > longest_gap) longest_gap = now_us() - last_image;
        const int recent = now_us() - last_image < 3000000;
        snprintf(line, sizeof line,
                 "background %s: %d passes, %d frames, %d images, %d codec errors, %d failed flushes, the longest "
                 "time without an image %.1f s, images in the last 3 s %s",
                 kOutputs[output], passes, frames, images, errors, flushes_failed, longest_gap / 1000000.0,
                 recent ? "yes" : "no");
        append(log, sizeof log, line);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    if (reader != NULL) AImageReader_delete(reader);
    release_stream(env, bytes, index, &stream);
    return (*env)->NewStringUTF(env, log);
}
