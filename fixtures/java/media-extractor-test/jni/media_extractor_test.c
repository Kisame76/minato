// ADR-0060 e2d's test (media-extractor-test): the JNI library MediaExtractorApplication loads, which opens one mp4
// file through the NDK's AMediaExtractor and logs, so that Minato's lines compare with Android 15's:
// - from the asset's file descriptor with its offset and length: the file's format, each track's format (every key
//   of kKeys it has, by type and value; the whole format's toString to the device tag), every sample of all tracks
//   in the extractor's order (its track, time, flags, size and a CRC32 of its bytes), the first samples' formats, the
//   answers once the samples are out, seeks in each mode to several times with all tracks and with the first alone
//   (the next three samples after each), and the refusals (a track past the count, a second source, a buffer too
//   small). A track format past the count is not asked for: Android's is a format without a message, which crashes
//   on use;
// - from the copy's path and from the bytes through an AMediaDataSource: the same samples, by a digest of their lines;
// - from the bytes cut after 4096 and after 64 bytes, with a box too short for its header after the ftyp (which only
//   MPEG4Extractor's legacy sniff takes), and from bytes that are no movie: what the source answers, and how often
//   the extractor's delete calls the source's close callback;
// - from the bytes with every mdhd's duration 0: each track's format, which then has no duration;
// - from baseline's bytes with other sample descriptions (ADR-0060 e2d step 2a.1): each track's format and the samples'
//   count and digest, where an entry of another codec follows (the last one names the codec, whose own configuration
//   must be there and gives the csd, whatever boxes the entries before it had), an unknown entry (avc3) follows, a
//   first stsd without the avcC comes before the whole one, and the avcC has two SPS and two PPS;
// - a source the sniff refuses and then the fd on the same extractor (the refused source is never closed), and the fd
//   from its second byte (refused; whether a descriptor stays goes to the device tag).
// The AMediaDataSource answers 0 for a read at its end. With -1, as NdkMediaDataSource.h asks, Android 15 finds no
// track in these files, whose moov is last (observed; the cause lies in the extractor service's caching data
// sources): that answer goes to the device tag.
// A line that starts with "device " goes to the device tag.
#include <jni.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define NAME(name) Java_io_github_kisame76_minato_test_mediaextractor_MediaExtractorApplication_##name

// The keys a track's or a sample's format is asked for: those MPEG4Extractor and NuMediaExtractor set, and those
// an app reads.
static const char *const kKeys[] = {
    "mime", "durationUs", "width", "height", "max-width", "max-height", "display-width", "display-height",
    "sar-width", "sar-height", "rotation-degrees", "frame-rate", "capture-rate", "bitrate", "max-bitrate", "profile",
    "level", "aac-profile", "is-adts", "sample-rate", "channel-count", "channel-mask", "pcm-encoding",
    "bits-per-sample", "encoder-delay", "encoder-padding", "language", "track-id", "max-input-size", "csd-0", "csd-1",
    "csd-2", "csd-avc", "csd-hevc", "esds", "color-range", "color-standard", "color-transfer", "hdr-static-info",
    "is-default", "is-autoselect", "is-forced-subtitle", "is-sync-frame", "thumbnail-time", "time-us", "track-index",
    "temporal-layer-id", "temporal-layer-count", "sample-file-offset", "last-sample-index-in-chunk", "target-time",
    "valid-samples", "pssh", "crypto-mode", "frame-count",
};

struct log {
    char *data;
    size_t used, size;
};

static void add(struct log *log, const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (length < 0) return;
    if ((size_t)length >= sizeof line) length = sizeof line - 1;
    if (log->used + (size_t)length + 2 > log->size) {
        size_t size = log->size * 2 + (size_t)length + 2;
        char *grown = realloc(log->data, size);
        if (grown == NULL) return;
        log->data = grown;
        log->size = size;
    }
    memcpy(log->data + log->used, line, (size_t)length);
    log->used += (size_t)length;
    log->data[log->used++] = '\n';
    log->data[log->used] = 0;
}

// Every key of kKeys the format has, one line each, and its toString to the device tag.
static void format_lines(struct log *log, const char *prefix, AMediaFormat *format) {
    if (format == NULL) {
        add(log, "%s: no format", prefix);
        return;
    }
    int entries = 0;
    for (size_t index = 0; index < sizeof kKeys / sizeof kKeys[0]; index++) {
        const char *key = kKeys[index];
        int32_t int32;
        int64_t int64;
        float float32;
        double float64;
        size_t size;
        const char *string;
        void *data;
        if (AMediaFormat_getInt32(format, key, &int32)) {
            add(log, "%s %s: int32 %d", prefix, key, int32);
        } else if (AMediaFormat_getInt64(format, key, &int64)) {
            add(log, "%s %s: int64 %lld", prefix, key, (long long)int64);
        } else if (AMediaFormat_getFloat(format, key, &float32)) {
            add(log, "%s %s: float %g", prefix, key, (double)float32);
        } else if (AMediaFormat_getDouble(format, key, &float64)) {
            add(log, "%s %s: double %g", prefix, key, float64);
        } else if (AMediaFormat_getString(format, key, &string)) {
            add(log, "%s %s: string %s", prefix, key, string);
        } else if (AMediaFormat_getBuffer(format, key, &data, &size)) {
            add(log, "%s %s: buffer %zu bytes, crc %08lx", prefix, key, size,
                crc32(0, (const Bytef *)data, (uInt)size));
        } else if (AMediaFormat_getSize(format, key, &size)) {
            add(log, "%s %s: size %zu", prefix, key, size);
        } else {
            continue;
        }
        entries++;
    }
    if (entries == 0) add(log, "%s: no keys", prefix);
    add(log, "device %s: %s", prefix, AMediaFormat_toString(format));
}

// The sample the extractor is at, as one line: its track, time, flags, size and its bytes' CRC32.
static void sample_line(AMediaExtractor *extractor, char *line, size_t size) {
    const int track = AMediaExtractor_getSampleTrackIndex(extractor);
    const int64_t time = AMediaExtractor_getSampleTime(extractor);
    const int32_t flags = (int32_t)AMediaExtractor_getSampleFlags(extractor);
    const ssize_t length = AMediaExtractor_getSampleSize(extractor);
    uint8_t *bytes = malloc(length > 0 ? (size_t)length : 1);
    ssize_t read = -2;
    unsigned long crc = 0;
    if (bytes != NULL) {
        read = AMediaExtractor_readSampleData(extractor, bytes, length > 0 ? (size_t)length : 0);
        if (read > 0) crc = crc32(0, bytes, (uInt)read);
        free(bytes);
    }
    snprintf(line, size, "track %d time %lld flags %d size %zd read %zd crc %08lx", track, (long long)time, flags,
             length, read, crc);
}

// Every sample from where the extractor is, at most 10000: a line each when `verbose`, the count and a CRC32 of the
// lines always.
static unsigned long walk(struct log *log, const char *prefix, AMediaExtractor *extractor, int verbose,
                          int *count) {
    unsigned long digest = 0;
    *count = 0;
    while (*count < 10000 && AMediaExtractor_getSampleTrackIndex(extractor) >= 0) {
        char line[256];
        sample_line(extractor, line, sizeof line);
        digest = crc32(digest, (const Bytef *)line, (uInt)strlen(line));
        if (verbose) add(log, "%s s%d: %s", prefix, *count, line);
        if (verbose && *count < 2) {
            AMediaFormat *format = AMediaFormat_new();
            char name[128];
            snprintf(name, sizeof name, "%s s%d format", prefix, *count);
            add(log, "%s: status %d", name, AMediaExtractor_getSampleFormat(extractor, format));
            format_lines(log, name, format);
            AMediaFormat_delete(format);
        }
        (*count)++;
        if (!AMediaExtractor_advance(extractor)) break;
    }
    return digest;
}

// What the extractor answers once its samples are out.
static void end_lines(struct log *log, const char *prefix, AMediaExtractor *extractor) {
    uint8_t byte;
    add(log, "%s end: advance %d, track %d, time %lld, flags %d, size %zd, read %zd", prefix,
        AMediaExtractor_advance(extractor), AMediaExtractor_getSampleTrackIndex(extractor),
        (long long)AMediaExtractor_getSampleTime(extractor), (int32_t)AMediaExtractor_getSampleFlags(extractor),
        AMediaExtractor_getSampleSize(extractor), AMediaExtractor_readSampleData(extractor, &byte, 1));
}

static const int64_t kTimes[] = {-500000, 0, 200000, 433333, 450000, 966667, 2000000};

// Seeks in each mode to each of kTimes, with the next three samples after each.
static void seek_lines(struct log *log, const char *prefix, AMediaExtractor *extractor) {
    for (int mode = AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC; mode <= AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC; mode++) {
        for (size_t index = 0; index < sizeof kTimes / sizeof kTimes[0]; index++) {
            const media_status_t status = AMediaExtractor_seekTo(extractor, kTimes[index], (SeekMode)mode);
            char line[512];
            int used = snprintf(line, sizeof line, "%s seek mode %d to %lld: status %d", prefix, mode,
                                (long long)kTimes[index], status);
            for (int next = 0; next < 3 && used > 0 && (size_t)used < sizeof line; next++) {
                used += snprintf(line + used, sizeof line - (size_t)used, "; track %d time %lld flags %d",
                                 AMediaExtractor_getSampleTrackIndex(extractor),
                                 (long long)AMediaExtractor_getSampleTime(extractor),
                                 (int32_t)AMediaExtractor_getSampleFlags(extractor));
                if (!AMediaExtractor_advance(extractor)) break;
            }
            add(log, "%s", line);
        }
    }
}

struct bytes {
    const uint8_t *data;
    size_t size;
    int end;     // what a read at the end answers
    int closed;  // how often the close callback was called
};

static ssize_t read_at(void *userdata, off64_t offset, void *buffer, size_t size) {
    const struct bytes *bytes = userdata;
    if (offset < 0 || (uint64_t)offset >= bytes->size) return bytes->end;
    const size_t left = bytes->size - (size_t)offset;
    const size_t count = size < left ? size : left;
    memcpy(buffer, bytes->data + offset, count);
    return (ssize_t)count;
}

static ssize_t get_size(void *userdata) { return (ssize_t)((const struct bytes *)userdata)->size; }

static void on_close(void *userdata) { ((struct bytes *)userdata)->closed++; }

// An extractor on `bytes` through an AMediaDataSource: its status, track count and, with tracks, the samples' count
// and digest.
static void custom_lines(struct log *log, const char *prefix, struct bytes *bytes, unsigned long whole) {
    AMediaDataSource *source = AMediaDataSource_new();
    AMediaDataSource_setUserdata(source, bytes);
    AMediaDataSource_setReadAt(source, read_at);
    AMediaDataSource_setGetSize(source, get_size);
    AMediaDataSource_setClose(source, on_close);
    AMediaExtractor *extractor = AMediaExtractor_new();
    const media_status_t status = AMediaExtractor_setDataSourceCustom(extractor, source);
    const size_t tracks = AMediaExtractor_getTrackCount(extractor);
    for (size_t track = 0; track < tracks; track++) AMediaExtractor_selectTrack(extractor, track);
    int count = 0;
    const unsigned long digest = walk(log, prefix, extractor, 0, &count);
    add(log, "%s%s: status %d, tracks %zu, %d samples%s", bytes->end == 0 ? "" : "device ", prefix, status, tracks,
        count, count == 0 ? "" : digest == whole ? ", the same as from the fd" : ", others than from the fd");
    if (status == AMEDIA_OK && bytes->end == 0) {
        char what[96];
        snprintf(what, sizeof what, "%s file", prefix);
        AMediaFormat *format = AMediaExtractor_getFileFormat(extractor);
        format_lines(log, what, format);
        AMediaFormat_delete(format);
    }
    bytes->closed = 0;
    AMediaExtractor_delete(extractor);
    add(log, "%s%s: the source closed %d times on delete", bytes->end == 0 ? "" : "device ", prefix, bytes->closed);
    AMediaDataSource_delete(source);
}

// The bytes with every mdhd's duration 0, through an AMediaDataSource: each track's format.
static void durationless_lines(struct log *log, const char *prefix, const uint8_t *data, size_t size) {
    uint8_t *copy = malloc(size > 0 ? size : 1);
    if (copy == NULL) return;
    memcpy(copy, data, size);
    for (size_t at = 4; at + 32 <= size; at++) {
        if (memcmp(copy + at, "mdhd", 4) != 0) continue;
        // After the type: version and flags, the times (8 or 16 bytes), the timescale, the duration (4 or 8 bytes).
        const int version = copy[at + 4];
        memset(copy + at + (version == 1 ? 28 : 20), 0, version == 1 ? 8 : 4);
    }
    struct bytes bytes = {copy, size, 0, 0};
    AMediaDataSource *source = AMediaDataSource_new();
    AMediaDataSource_setUserdata(source, &bytes);
    AMediaDataSource_setReadAt(source, read_at);
    AMediaDataSource_setGetSize(source, get_size);
    AMediaExtractor *extractor = AMediaExtractor_new();
    const media_status_t status = AMediaExtractor_setDataSourceCustom(extractor, source);
    const size_t tracks = AMediaExtractor_getTrackCount(extractor);
    add(log, "%s: status %d, tracks %zu", prefix, status, tracks);
    for (size_t track = 0; track < tracks; track++) {
        char what[96];
        snprintf(what, sizeof what, "%s track %zu", prefix, track);
        AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, track);
        format_lines(log, what, format);
        if (format != NULL) AMediaFormat_delete(format);
    }
    AMediaExtractor_delete(extractor);
    AMediaDataSource_delete(source);
    free(copy);
}

// A growing buffer of bytes.
struct buffer {
    uint8_t *data;
    size_t size;
};

static void append(struct buffer *buffer, const void *data, size_t size) {
    uint8_t *grown = realloc(buffer->data, buffer->size + size + 1);
    if (grown == NULL) return;
    memcpy(grown + buffer->size, data, size);
    buffer->data = grown;
    buffer->size += size;
}

static uint32_t be32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

static void put_be32(uint8_t *bytes, uint32_t value) {
    for (int index = 0; index < 4; index++) bytes[index] = (uint8_t)(value >> (24 - 8 * index));
}

// A box of `type` around the body.
static void append_box(struct buffer *buffer, const char *type, const void *body, size_t size) {
    uint8_t header[8];
    put_be32(header, (uint32_t)(size + 8));
    memcpy(header + 4, type, 4);
    append(buffer, header, 8);
    append(buffer, body, size);
}

// The offsets of the moov, trak number `track`, its mdia, minf, stbl and stsd; 0 when one is missing.
static int stsd_path(const uint8_t *data, size_t size, size_t track, size_t path[6]) {
    static const char *const kPath[] = {"moov", "trak", "mdia", "minf", "stbl", "stsd"};
    size_t start = 0, end = size;
    for (int level = 0; level < 6; level++) {
        size_t seen = 0;
        int found = 0;
        for (size_t at = start; at + 8 <= end && !found;) {
            uint64_t box = be32(data + at);
            size_t header = 8;
            if (box == 1 && at + 16 <= end) {
                box = (uint64_t)be32(data + at + 8) << 32 | be32(data + at + 12);
                header = 16;
            }
            if (box < header || box > end - at) return 0;
            if (memcmp(data + at + 4, kPath[level], 4) == 0 && (level != 1 || seen++ == track)) {
                path[level] = at;
                start = at + header;
                end = at + (size_t)box;
                found = 1;
            } else {
                at += (size_t)box;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

// The file with track `track`'s stsd box in place of `bytes`, the boxes that hold it grown or shrunk to match.
static struct buffer with_stsd(const uint8_t *data, size_t size, size_t track, const struct buffer *bytes) {
    struct buffer out = {NULL, 0};
    size_t path[6];
    if (!stsd_path(data, size, track, path)) return out;
    const size_t old = be32(data + path[5]);
    append(&out, data, path[5]);
    append(&out, bytes->data, bytes->size);
    append(&out, data + path[5] + old, size - path[5] - old);
    if (out.data == NULL) return out;
    for (int level = 0; level < 5; level++) {
        put_be32(out.data + path[level], (uint32_t)(be32(out.data + path[level]) + bytes->size - old));
    }
    return out;
}

// Track `track`'s first sample entry, whole, and its size.
static const uint8_t *first_entry(const uint8_t *data, size_t size, size_t track, size_t *length) {
    size_t path[6];
    if (!stsd_path(data, size, track, path) || be32(data + path[5]) < 24) return NULL;
    *length = be32(data + path[5] + 16);
    return *length >= 36 && *length <= be32(data + path[5]) - 16 ? data + path[5] + 16 : NULL;
}

// A sample entry of `type` with the visual entry's 78 bytes and `boxes`.
static void append_entry(struct buffer *buffer, const char *type, const uint8_t *visual, const void *boxes,
                         size_t size) {
    struct buffer body = {NULL, 0};
    append(&body, visual + 8, 78);
    append(&body, boxes, size);
    append_box(buffer, type, body.data, body.size);
    free(body.data);
}

// An stsd of the entries, each given whole.
static struct buffer stsd_of(const struct buffer *entries, uint32_t count) {
    struct buffer body = {NULL, 0}, out = {NULL, 0};
    uint8_t head[8] = {0};
    put_be32(head + 4, count);
    append(&body, head, 8);
    append(&body, entries->data, entries->size);
    append_box(&out, "stsd", body.data, body.size);
    free(body.data);
    return out;
}

// One description variant through an AMediaDataSource: each track's format, and the samples' count and digest.
static void variant_lines(struct log *log, const char *prefix, struct buffer *file) {
    if (file->data == NULL) {
        add(log, "%s: not made", prefix);
        return;
    }
    struct bytes bytes = {file->data, file->size, 0, 0};
    AMediaDataSource *source = AMediaDataSource_new();
    AMediaDataSource_setUserdata(source, &bytes);
    AMediaDataSource_setReadAt(source, read_at);
    AMediaDataSource_setGetSize(source, get_size);
    AMediaExtractor *extractor = AMediaExtractor_new();
    const media_status_t status = AMediaExtractor_setDataSourceCustom(extractor, source);
    const size_t tracks = AMediaExtractor_getTrackCount(extractor);
    add(log, "%s: status %d, tracks %zu", prefix, status, tracks);
    for (size_t track = 0; track < tracks; track++) {
        char what[128];
        snprintf(what, sizeof what, "%s track %zu", prefix, track);
        AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, track);
        format_lines(log, what, format);
        if (format != NULL) AMediaFormat_delete(format);
        add(log, "%s: select %zu: status %d", prefix, track, AMediaExtractor_selectTrack(extractor, track));
    }
    int count = 0;
    const unsigned long digest = walk(log, prefix, extractor, 0, &count);
    add(log, "%s: %d samples, digest %08lx", prefix, count, digest);
    AMediaExtractor_delete(extractor);
    AMediaDataSource_delete(source);
    free(file->data);
}

// baseline's bytes with other sample descriptions: its video track's avc1 (avcC, fiel and chrm) and its audio
// track's mp4a (esds) in other stsds.
static void description_lines(struct log *log, const char *name, const uint8_t *data, size_t size) {
    size_t video_size = 0, audio_size = 0;
    const uint8_t *video = first_entry(data, size, 0, &video_size);
    const uint8_t *audio = first_entry(data, size, 1, &audio_size);
    if (video == NULL || audio == NULL || video_size < 86 + 16 || memcmp(video + 4, "avc1", 4) != 0 ||
        memcmp(audio + 4, "mp4a", 4) != 0 || memcmp(video + 90, "avcC", 4) != 0 || be32(video + 86) < 16 ||
        be32(video + 86) > video_size - 86) {
        add(log, "custom %s descriptions: no avc1 and mp4a", name);
        return;
    }
    const uint8_t *avcc = video + 86;  // the avcC box, the visual entry's first
    const size_t avcc_size = be32(avcc);
    // An hvcC: version 1, Main profile, level 93, NAL length size 4, an array of one VPS of 2 bytes.
    static const uint8_t kHvcc[] = {1, 1, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 93, 0xf0, 0, 0xfc, 0xfd, 0xf8, 0xf8,
                                    0, 0, 0x0f, 1, 0x20, 0, 1, 0, 2, 0x40, 1};
    struct buffer hvcc = {NULL, 0};
    append_box(&hvcc, "hvcC", kHvcc, sizeof kHvcc);
    char prefix[96];
    struct buffer entries, stsd, file;

    // The audio track's stsd of the avc1, then the mp4a: an AAC track, its csd its esds's.
    entries = (struct buffer){NULL, 0};
    append(&entries, video, video_size);
    append(&entries, audio, audio_size);
    stsd = stsd_of(&entries, 2);
    file = with_stsd(data, size, 1, &stsd);
    snprintf(prefix, sizeof prefix, "custom %s avc1 then mp4a", name);
    variant_lines(log, prefix, &file);
    free(stsd.data);
    free(entries.data);

    // The audio track's stsd of the mp4a, then the avc1 as avc3, an entry Android does not know: AAC as before.
    entries = (struct buffer){NULL, 0};
    append(&entries, audio, audio_size);
    append_entry(&entries, "avc3", video, video + 86, video_size - 86);
    stsd = stsd_of(&entries, 2);
    file = with_stsd(data, size, 1, &stsd);
    snprintf(prefix, sizeof prefix, "custom %s mp4a then avc3", name);
    variant_lines(log, prefix, &file);
    free(stsd.data);
    free(entries.data);

    // The video track's stsd of the avc1, then an hvc1 with an hvcC: HEVC, its csd and NAL length the hvcC's.
    entries = (struct buffer){NULL, 0};
    append(&entries, video, video_size);
    append_entry(&entries, "hvc1", video, hvcc.data, hvcc.size);
    stsd = stsd_of(&entries, 2);
    file = with_stsd(data, size, 0, &stsd);
    snprintf(prefix, sizeof prefix, "custom %s avc1 then hvc1", name);
    variant_lines(log, prefix, &file);
    free(stsd.data);
    free(entries.data);

    // The video track's stsd of the avc1, then an hvc1 without an hvcC: no video track.
    entries = (struct buffer){NULL, 0};
    append(&entries, video, video_size);
    append_entry(&entries, "hvc1", video, NULL, 0);
    stsd = stsd_of(&entries, 2);
    file = with_stsd(data, size, 0, &stsd);
    snprintf(prefix, sizeof prefix, "custom %s avc1 then hvc1 without hvcC", name);
    variant_lines(log, prefix, &file);
    free(stsd.data);
    free(entries.data);

    // The video track's stsd of an hvc1 with an hvcC alone: HEVC of the hvcC's csd and NAL length.
    entries = (struct buffer){NULL, 0};
    append_entry(&entries, "hvc1", video, hvcc.data, hvcc.size);
    stsd = stsd_of(&entries, 1);
    file = with_stsd(data, size, 0, &stsd);
    snprintf(prefix, sizeof prefix, "custom %s hvc1", name);
    variant_lines(log, prefix, &file);
    free(stsd.data);
    free(entries.data);

    // An stsd of the avc1 without its avcC, then the whole one: the track has its avcC at its end.
    entries = (struct buffer){NULL, 0};
    append_entry(&entries, "avc1", video, NULL, 0);
    stsd = stsd_of(&entries, 1);
    free(entries.data);
    entries = (struct buffer){NULL, 0};
    append(&entries, video, video_size);
    struct buffer whole = stsd_of(&entries, 1);
    append(&stsd, whole.data, whole.size);
    file = with_stsd(data, size, 0, &stsd);
    snprintf(prefix, sizeof prefix, "custom %s avc1 without avcC, then a second stsd", name);
    variant_lines(log, prefix, &file);
    free(whole.data);
    free(stsd.data);
    free(entries.data);

    // The avcC with its SPS and its PPS twice each: csd-0 and csd-1 hold both.
    const size_t sps = (size_t)avcc[14] << 8 | avcc[15];
    const size_t pps = avcc_size >= 16 + sps + 3 ? (size_t)avcc[17 + sps] << 8 | avcc[18 + sps] : 0;
    if (avcc_size >= 16 + sps + 3 + pps && (avcc[13] & 0x1f) == 1 && avcc[16 + sps] == 1) {
        const uint8_t *pps_at = avcc + 16 + sps + 1;  // the PPS's length, then the PPS
        struct buffer body = {NULL, 0};
        append(&body, avcc + 8, 5);
        const uint8_t two_sps = (uint8_t)((avcc[13] & 0xe0) | 2), two = 2;
        append(&body, &two_sps, 1);
        append(&body, avcc + 14, 2 + sps);
        append(&body, avcc + 14, 2 + sps);
        append(&body, &two, 1);
        append(&body, pps_at, 2 + pps);
        append(&body, pps_at, 2 + pps);
        struct buffer boxes = {NULL, 0};
        append_box(&boxes, "avcC", body.data, body.size);
        append(&boxes, avcc + avcc_size, video_size - 86 - avcc_size);
        entries = (struct buffer){NULL, 0};
        append_entry(&entries, "avc1", video, boxes.data, boxes.size);
        stsd = stsd_of(&entries, 1);
        file = with_stsd(data, size, 0, &stsd);
        snprintf(prefix, sizeof prefix, "custom %s two SPS and two PPS", name);
        variant_lines(log, prefix, &file);
        free(stsd.data);
        free(entries.data);
        free(boxes.data);
        free(body.data);
    }
    free(hvcc.data);
}

JNIEXPORT jstring JNICALL NAME(extract)(JNIEnv *env, jclass clazz, jstring jname, jint fd, jlong offset,
                                         jlong length, jstring jpath, jbyteArray jbytes) {
    (void)clazz;
    struct log log = {NULL, 0, 0};
    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    const jsize size = (*env)->GetArrayLength(env, jbytes);
    uint8_t *data = malloc(size > 0 ? (size_t)size : 1);
    (*env)->GetByteArrayRegion(env, jbytes, 0, size, (jbyte *)data);
    char prefix[64];

    // From the asset's file descriptor, with its offset and length.
    snprintf(prefix, sizeof prefix, "fd %s", name);
    AMediaExtractor *extractor = AMediaExtractor_new();
    add(&log, "%s: status %d", prefix, AMediaExtractor_setDataSourceFd(extractor, fd, offset, length));
    const size_t tracks = AMediaExtractor_getTrackCount(extractor);
    add(&log, "%s: %zu tracks, cached duration %lld", prefix, tracks,
        (long long)AMediaExtractor_getCachedDuration(extractor));
    char what[96];
    snprintf(what, sizeof what, "%s file", prefix);
    AMediaFormat *format = AMediaExtractor_getFileFormat(extractor);
    format_lines(&log, what, format);
    AMediaFormat_delete(format);
    for (size_t track = 0; track < tracks; track++) {
        snprintf(what, sizeof what, "%s track %zu", prefix, track);
        format = AMediaExtractor_getTrackFormat(extractor, track);
        format_lines(&log, what, format);
        if (format != NULL) AMediaFormat_delete(format);
    }
    add(&log, "%s: before a track is selected: track %d, time %lld", prefix,
        AMediaExtractor_getSampleTrackIndex(extractor), (long long)AMediaExtractor_getSampleTime(extractor));
    for (size_t track = 0; track < tracks; track++) {
        add(&log, "%s: select %zu: status %d", prefix, track, AMediaExtractor_selectTrack(extractor, track));
    }
    add(&log, "%s: select %zu: status %d, unselect %zu: status %d", prefix, tracks,
        AMediaExtractor_selectTrack(extractor, tracks), tracks, AMediaExtractor_unselectTrack(extractor, tracks));
    int count = 0;
    const unsigned long whole = walk(&log, prefix, extractor, 1, &count);
    add(&log, "%s: %d samples", prefix, count);
    end_lines(&log, prefix, extractor);
    seek_lines(&log, prefix, extractor);
    uint8_t byte;
    add(&log, "%s: seek to 0 %d, read into 1 byte %zd, pssh %s, crypto info %s", prefix,
        AMediaExtractor_seekTo(extractor, 0, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC),
        AMediaExtractor_readSampleData(extractor, &byte, 1),
        AMediaExtractor_getPsshInfo(extractor) == NULL ? "none" : "some",
        AMediaExtractor_getSampleCryptoInfo(extractor) == NULL ? "none" : "some");
    if (tracks > 1) {
        for (size_t track = 1; track < tracks; track++) {
            add(&log, "%s: unselect %zu: status %d", prefix, track, AMediaExtractor_unselectTrack(extractor, track));
        }
        snprintf(what, sizeof what, "%s track 0 alone", prefix);
        seek_lines(&log, what, extractor);
    }
    add(&log, "%s: a second source: status %d", prefix,
        AMediaExtractor_setDataSourceFd(extractor, fd, offset, length));
    AMediaExtractor_delete(extractor);

    // From the copy's path.
    snprintf(prefix, sizeof prefix, "path %s", name);
    extractor = AMediaExtractor_new();
    const media_status_t status = AMediaExtractor_setDataSource(extractor, path);
    const size_t path_tracks = AMediaExtractor_getTrackCount(extractor);
    for (size_t track = 0; track < path_tracks; track++) AMediaExtractor_selectTrack(extractor, track);
    const unsigned long digest = walk(&log, prefix, extractor, 0, &count);
    add(&log, "%s: status %d, tracks %zu, %d samples%s", prefix, status, path_tracks, count,
        digest == whole ? ", the same as from the fd" : ", others than from the fd");
    AMediaExtractor_delete(extractor);

    // From the bytes, whole, cut, and bytes that are no movie.
    struct bytes bytes = {data, (size_t)size, 0, 0};
    snprintf(prefix, sizeof prefix, "custom %s", name);
    custom_lines(&log, prefix, &bytes, whole);
    struct bytes ending = {data, (size_t)size, -1, 0};
    snprintf(prefix, sizeof prefix, "custom %s ending with -1", name);
    custom_lines(&log, prefix, &ending, whole);
    for (size_t cut = 4096; cut >= 64; cut = cut == 4096 ? 64 : 0) {
        struct bytes part = {data, cut < (size_t)size ? cut : (size_t)size, 0, 0};
        snprintf(prefix, sizeof prefix, "custom %s cut after %zu", name, cut);
        custom_lines(&log, prefix, &part, whole);
    }
    // The short box right after the ftyp, whose size the file's first 4 bytes give.
    const size_t ftyp = size >= 4 ? (size_t)data[0] << 24 | (size_t)data[1] << 16 | (size_t)data[2] << 8 | data[3] : 0;
    uint8_t *odd = malloc((size_t)size + 8);
    if (odd != NULL && ftyp >= 8 && ftyp < (size_t)size) {
        static const uint8_t kShort[] = {0, 0, 0, 2, 'f', 'r', 'e', 'e'};
        memcpy(odd, data, ftyp);
        memcpy(odd + ftyp, kShort, sizeof kShort);
        memcpy(odd + ftyp + 8, data + ftyp, (size_t)size - ftyp);
        struct bytes shorter = {odd, (size_t)size + 8, 0, 0};
        snprintf(prefix, sizeof prefix, "custom %s with a short box", name);
        custom_lines(&log, prefix, &shorter, whole);
    }
    free(odd);
    uint8_t text[1000];
    memset(text, 'x', sizeof text);
    struct bytes other = {text, sizeof text, 0, 0};
    snprintf(prefix, sizeof prefix, "custom %s no movie", name);
    custom_lines(&log, prefix, &other, whole);
    snprintf(prefix, sizeof prefix, "custom %s without durations", name);
    durationless_lines(&log, prefix, data, (size_t)size);
    if (strcmp(name, "baseline") == 0) description_lines(&log, name, data, (size_t)size);

    // A source the sniff refuses, then the fd on the same extractor: the refused source is never closed.
    struct bytes refused = {text, sizeof text, 0, 0};
    AMediaDataSource *source = AMediaDataSource_new();
    AMediaDataSource_setUserdata(source, &refused);
    AMediaDataSource_setReadAt(source, read_at);
    AMediaDataSource_setGetSize(source, get_size);
    AMediaDataSource_setClose(source, on_close);
    extractor = AMediaExtractor_new();
    const media_status_t first = AMediaExtractor_setDataSourceCustom(extractor, source);
    const media_status_t then = AMediaExtractor_setDataSourceFd(extractor, fd, offset, length);
    const size_t then_tracks = AMediaExtractor_getTrackCount(extractor);
    AMediaExtractor_delete(extractor);
    AMediaDataSource_delete(source);
    add(&log, "custom %s no movie, then the fd: status %d then %d, %zu tracks, the refused source closed %d times",
        name, first, then, then_tracks, refused.closed);

    // The fd from its second byte, which no sniff takes: whether the extractor keeps a descriptor of it (the lowest
    // free descriptor number before and after). To the device tag: Minato closes its copy at once, Android 15's
    // FileSource stays while the extractor service's binder references to it are released (observed: kept).
    const int probe = dup(fd);
    close(probe);
    extractor = AMediaExtractor_new();
    const media_status_t shifted = AMediaExtractor_setDataSourceFd(extractor, fd, offset + 1, length - 1);
    const int after = dup(fd);
    close(after);
    AMediaExtractor_delete(extractor);
    add(&log, "device fd %s from its second byte: status %d, its descriptor %s", name, shifted,
        after == probe ? "closed" : "kept");

    free(data);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    jstring result = (*env)->NewStringUTF(env, log.data != NULL ? log.data : "");
    free(log.data);
    return result;
}
