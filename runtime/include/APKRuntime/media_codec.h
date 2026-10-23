/* libmediandk's private functions for the framework's MediaCodec, MediaCodecList and MediaFormat natives
   (ADR-0060 e2b3), which run over libmediandk's AMediaCodec and AMediaFormat (runtime/android/media_codec.cpp and
   media_format.cpp) and reach these through dlsym, as the NDK's stub lacks them. Guest code; unversioned, as
   libmediandk.so's public names are. */
#ifndef APKRUNTIME_MEDIA_CODEC_H
#define APKRUNTIME_MEDIA_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AMediaFormat;

/* The kind of an AMediaFormat entry, which picks its getter (AMediaFormat_getInt32 and the others). */
enum minato_media_format_kind {
    MINATO_MEDIA_FORMAT_INT32 = 1,
    MINATO_MEDIA_FORMAT_INT64,
    MINATO_MEDIA_FORMAT_SIZE,
    MINATO_MEDIA_FORMAT_FLOAT,
    MINATO_MEDIA_FORMAT_DOUBLE,
    MINATO_MEDIA_FORMAT_STRING,
    MINATO_MEDIA_FORMAT_BUFFER,
    MINATO_MEDIA_FORMAT_RECT,
};

/* The number of entries, in the order they were first set. */
size_t minato_AMediaFormat_count(struct AMediaFormat *format);
/* Entry `index`'s name (valid until the format changes) and kind; 0, or -1 past the last. */
int minato_AMediaFormat_entry(struct AMediaFormat *format, size_t index, const char **name, int *kind);

/* One of Minato's codecs (MediaCodecList's): its name, its one media type, and whether it encodes. */
struct minato_media_codec_info {
    const char *name;
    const char *type;
    int encoder;
    int hardware;  // decodes on the host's hardware where it has one (MediaCodecInfo.isHardwareAccelerated)
};

/* The number of Minato's codecs, and codec `index`'s info; NULL past the last. */
size_t minato_media_codec_count(void);
const struct minato_media_codec_info *minato_media_codec_at(size_t index);

/* A frame's time in nanoseconds from its microseconds, as a surface's queue and MediaCodec's images take it, clamped
   to int64_t's range where a guest's time would overflow (ADR-0060 e2c2). */
static inline int64_t minato_media_time_ns(int64_t us) {
    if (us > INT64_MAX / 1000) return INT64_MAX;
    if (us < INT64_MIN / 1000) return INT64_MIN;
    return us * 1000;
}

#ifdef __cplusplus
}
#endif

#endif  // APKRUNTIME_MEDIA_CODEC_H
