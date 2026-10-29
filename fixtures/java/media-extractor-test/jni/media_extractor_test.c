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
    "mime", "durationUs", "width", "height", "max-width", "max-height", "display-width", "display-height", "sar-width",
    "sar-height", "rotation-degrees", "frame-rate", "capture-rate", "bitrate", "max-bitrate", "profile", "level",
    "aac-profile", "is-adts", "sample-rate", "channel-count", "channel-mask", "pcm-encoding", "bits-per-sample",
    "encoder-delay", "encoder-padding", "language", "track-id", "max-input-size", "csd-0", "csd-1", "csd-2", "csd-avc",
    "csd-hevc", "esds", "color-range", "color-standard", "color-transfer", "android._is-hdr", "hdr-static-info",
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

// The offset in an esds box of its DecoderConfigDescriptor's maximum bit rate (the average follows), or 0: version
// and flags, the ES_Descriptor (tag 3) with its flags' fields, the DecoderConfigDescriptor (tag 4), descriptor
// lengths of up to 4 bytes.
static size_t esds_rates_at(const uint8_t *esds, size_t size) {
    size_t at = 12;
    const size_t end = be32(esds) <= size ? be32(esds) : size;
    for (int descriptor = 0; descriptor < 2; descriptor++) {
        if (at >= end || esds[at] != (descriptor == 0 ? 3 : 4)) return 0;
        at++;
        for (int part = 0; part < 4 && at < end && (esds[at++] & 0x80) != 0; part++) {}
        if (descriptor == 0) {
            if (at + 3 > end) return 0;
            const uint8_t flags = esds[at + 2];
            at += 3;
            if (flags & 0x80) at += 2;
            if ((flags & 0x40) && at < end) at += 1 + esds[at];
            if (flags & 0x20) at += 2;
        }
    }
    return at + 13 <= end ? at + 5 : 0;
}

// One description variant through an AMediaDataSource: each track's format, and the samples' count and digest.
static void variant_lines_but(struct log *log, const char *prefix, struct buffer *file, size_t formatless);

static void variant_lines(struct log *log, const char *prefix, struct buffer *file) {
    variant_lines_but(log, prefix, file, (size_t)-1);
}

// As variant_lines, without track `formatless`'s format: Android's NDK gives a format of no message for a track
// whose format does not convert, and its getters dereference it.
static void variant_lines_but(struct log *log, const char *prefix, struct buffer *file, size_t formatless) {
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
        if (track != formatless) {
            AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, track);
            format_lines(log, what, format);
            if (format != NULL) AMediaFormat_delete(format);
        }
        add(log, "%s: select %zu: status %d", prefix, track, AMediaExtractor_selectTrack(extractor, track));
    }
    int count = 0;
    const unsigned long digest = walk(log, prefix, extractor, 0, &count);
    add(log, "%s: %d samples, digest %08lx", prefix, count, digest);
    AMediaExtractor_delete(extractor);
    AMediaDataSource_delete(source);
    free(file->data);
}

// A box for a sample entry: its type and body.
struct extra {
    const char *type;
    const uint8_t *body;
    size_t size;
};
#define EXTRA(type, body) {type, body, sizeof body}

// Colour and aspect boxes (step 2a.2): colr and pasp after the avc1's boxes and the mp4a's esds, the first
// nclx or nclc colr the track's, one too short for its fields failing the file; and hvc1s whose hvcC has, after its
// VPS, SPSs (written for this test: Main profile, 320 x 180) whose VUI has a colour description or not.
static void colour_lines(struct log *log, const char *name, const uint8_t *data, size_t size, const uint8_t *video,
                         size_t video_size, const uint8_t *audio, size_t audio_size, const uint8_t *hvcc_head) {
    static const uint8_t kHdr10[] = {'n', 'c', 'l', 'x', 0, 9, 0, 16, 0, 9, 0x80};
    static const uint8_t kBt709[] = {'n', 'c', 'l', 'c', 0, 1, 0, 1, 0, 1};
    static const uint8_t kExtended[] = {'n', 'c', 'l', 'x', 0, 11, 0, 17, 0, 0, 0x80};
    static const uint8_t kShortNclx[] = {'n', 'c', 'l', 'x', 0, 9, 0, 16, 0, 9};
    static const uint8_t kProfile[] = {'p', 'r', 'o', 'f', 0, 0, 0, 0};
    static const uint8_t kUnknownTransfer[] = {'n', 'c', 'l', 'x', 0, 1, 0, 3, 0, 1, 0};
    static const uint8_t kShortColr[] = {'n', 'c', 'l'};
    static const uint8_t k4by3[] = {0, 0, 0, 4, 0, 0, 0, 3}, k0by3[] = {0, 0, 0, 0, 0, 0, 0, 3};
    static const uint8_t k16by9[] = {0, 0, 0, 16, 0, 0, 0, 9}, k1by0[] = {0, 0, 0, 1, 0, 0, 0, 0};
    static const uint8_t kHuge[] = {0x80, 0, 0, 0, 0, 0, 0, 1}, kShortPasp[] = {0, 0, 0, 4, 0, 0, 0};
    static const uint8_t kHugeHeight[] = {0, 0, 0, 1, 0x80, 0, 0, 0};
    const struct {
        const char *what;
        struct extra first, second;
    } kinds[] = {
        {"colr nclx of HDR10", EXTRA("colr", kHdr10), {NULL, NULL, 0}},
        {"colr nclc of BT.709", EXTRA("colr", kBt709), {NULL, NULL, 0}},
        {"colr nclx of extended values", EXTRA("colr", kExtended), {NULL, NULL, 0}},
        {"colr nclx of unknown transfer", EXTRA("colr", kUnknownTransfer), {NULL, NULL, 0}},
        {"colr nclc, then nclx", EXTRA("colr", kBt709), EXTRA("colr", kHdr10)},
        {"colr short nclx, then nclc", EXTRA("colr", kShortNclx), EXTRA("colr", kBt709)},
        {"colr prof, then nclx", EXTRA("colr", kProfile), EXTRA("colr", kHdr10)},
        {"colr of 3 bytes", EXTRA("colr", kShortColr), {NULL, NULL, 0}},
        {"pasp 4:3", EXTRA("pasp", k4by3), {NULL, NULL, 0}},
        {"pasp 0:3", EXTRA("pasp", k0by3), {NULL, NULL, 0}},
        {"pasp 4:3, then 16:9", EXTRA("pasp", k4by3), EXTRA("pasp", k16by9)},
        {"pasp 4:3, then 1:0", EXTRA("pasp", k4by3), EXTRA("pasp", k1by0)},
        {"pasp 2^31:1", EXTRA("pasp", kHuge), {NULL, NULL, 0}},
        {"pasp 1:2^31", EXTRA("pasp", kHugeHeight), {NULL, NULL, 0}},
        {"pasp of 7 bytes", EXTRA("pasp", kShortPasp), {NULL, NULL, 0}},
    };
    char prefix[96];
    for (size_t kind = 0; kind < sizeof kinds / sizeof kinds[0]; kind++) {
        struct buffer boxes = {NULL, 0}, entries = {NULL, 0};
        append(&boxes, video + 86, video_size - 86);
        append_box(&boxes, kinds[kind].first.type, kinds[kind].first.body, kinds[kind].first.size);
        if (kinds[kind].second.type != NULL) {
            append_box(&boxes, kinds[kind].second.type, kinds[kind].second.body, kinds[kind].second.size);
        }
        append_entry(&entries, "avc1", video, boxes.data, boxes.size);
        struct buffer stsd = stsd_of(&entries, 1);
        struct buffer file = with_stsd(data, size, 0, &stsd);
        snprintf(prefix, sizeof prefix, "custom %s avc1 with %s", name, kinds[kind].what);
        variant_lines(log, prefix, &file);
        free(stsd.data);
        free(entries.data);
        free(boxes.data);
    }

    // The mp4a with a colr and a pasp: an audio track's format has neither's keys.
    {
        struct buffer body = {NULL, 0}, entries = {NULL, 0};
        append(&body, audio + 8, audio_size - 8);
        append_box(&body, "colr", kHdr10, sizeof kHdr10);
        append_box(&body, "pasp", k4by3, sizeof k4by3);
        append_box(&entries, "mp4a", body.data, body.size);
        struct buffer stsd = stsd_of(&entries, 1);
        struct buffer file = with_stsd(data, size, 1, &stsd);
        snprintf(prefix, sizeof prefix, "custom %s mp4a with colr and pasp", name);
        variant_lines(log, prefix, &file);
        free(stsd.data);
        free(entries.data);
        free(body.data);
    }

    // SPS NAL units with emulation prevention bytes, their VUI's video signal type of format 5 (unspecified), the
    // full range flag, and the colour description's primaries, transfer and matrix: 9, 16, 9 (HDR10); 1, 1, 1 with
    // full range; none with full range; 2, 2, 2 (unspecified); 2, 1, 2; 9 alone, the unit ending there; 9, 18, 9
    // (HLG); and 1, 1, 1 after a chroma_format_idc of 259, which parseSps keeps as a uint8_t (3: a
    // separate_colour_plane_flag follows).
    static const uint8_t kSpsPq[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00,
                                     0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0, 0x0a, 0x08, 0x0b,
                                     0x56, 0x59, 0x7a, 0xbc, 0xae, 0x6a, 0x12, 0x20, 0x13, 0x00};
    static const uint8_t kSpsFull[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00,
                                       0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0, 0x0a, 0x08, 0x0b,
                                       0x56, 0x59, 0x7a, 0xbc, 0xae, 0x6e, 0x02, 0x02, 0x03, 0x00};
    static const uint8_t kSpsUndescribed[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90,
                                              0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0, 0x0a,
                                              0x08, 0x0b, 0x56, 0x59, 0x7a, 0xbc, 0xae, 0x6d, 0x00};
    static const uint8_t kSpsUnspecified[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00,
                                              0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0, 0x0a, 0x08, 0x0b,
                                              0x56, 0x59, 0x7a, 0xbc, 0xae, 0x6a, 0x04, 0x04, 0x05, 0x00};
    static const uint8_t kSpsPartly[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00,
                                         0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0, 0x0a, 0x08, 0x0b,
                                         0x56, 0x59, 0x7a, 0xbc, 0xae, 0x6a, 0x04, 0x02, 0x05, 0x00};
    static const uint8_t kSpsCut[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90,
                                      0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0, 0x0a,
                                      0x08, 0x0b, 0x56, 0x59, 0x7a, 0xbc, 0xae, 0x6a, 0x12};
    static const uint8_t kSpsHlg[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00,
                                      0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0, 0x0a, 0x08, 0x0b,
                                      0x56, 0x59, 0x7a, 0xbc, 0xae, 0x6a, 0x12, 0x24, 0x13, 0x00};
    static const uint8_t kSpsChroma[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
                                         0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0x80, 0x41, 0x00, 0x14, 0x10, 0x16,
                                         0xac, 0xb2, 0xf5, 0x79, 0x5c, 0xd4, 0x04, 0x04, 0x06, 0x00};
    // Each with the hvcC's profile byte (0 keeps Main) and first compatibility byte (0 keeps Main's).
    const struct {
        const char *what;
        struct extra first, second, colr;
        int then_avc1;
        uint8_t profile, compatibility;
    } hevc[] = {
        {"of an HDR10 VUI", EXTRA("", kSpsPq), {NULL, NULL, 0}, {NULL, NULL, 0}, 0},
        {"of a full range BT.709 VUI", EXTRA("", kSpsFull), {NULL, NULL, 0}, {NULL, NULL, 0}, 0},
        {"of a VUI without a description", EXTRA("", kSpsUndescribed), {NULL, NULL, 0}, {NULL, NULL, 0}, 0},
        {"of an unspecified VUI", EXTRA("", kSpsUnspecified), {NULL, NULL, 0}, {NULL, NULL, 0}, 0},
        {"of a VUI partly unspecified, a colr", EXTRA("", kSpsPartly), {NULL, NULL, 0}, EXTRA("colr", kHdr10), 0},
        {"of a VUI cut after its primaries", EXTRA("", kSpsCut), {NULL, NULL, 0}, {NULL, NULL, 0}, 0},
        {"of two SPSs", EXTRA("", kSpsPq), EXTRA("", kSpsUndescribed), {NULL, NULL, 0}, 0},
        {"of chroma format 259", EXTRA("", kSpsChroma), {NULL, NULL, 0}, {NULL, NULL, 0}, 0},
        {"with a colr alone", {NULL, NULL, 0}, {NULL, NULL, 0}, EXTRA("colr", kBt709), 0},
        {"of an HDR10 VUI, then avc1", EXTRA("", kSpsPq), {NULL, NULL, 0}, {NULL, NULL, 0}, 1},
        {"Main10 of an HDR10 VUI", EXTRA("", kSpsPq), {NULL, NULL, 0}, {NULL, NULL, 0}, 0, 0x02},
        {"Main10 with an HDR10 colr", {NULL, NULL, 0}, {NULL, NULL, 0}, EXTRA("colr", kHdr10), 0, 0x02},
        {"Main10 of a BT.709 VUI", EXTRA("", kSpsFull), {NULL, NULL, 0}, {NULL, NULL, 0}, 0, 0x02},
        {"Main10 of an HLG VUI", EXTRA("", kSpsHlg), {NULL, NULL, 0}, {NULL, NULL, 0}, 0, 0x02},
        {"Main10 by its flag, an HDR10 VUI", EXTRA("", kSpsPq), {NULL, NULL, 0}, {NULL, NULL, 0}, 0, 0x04, 0x20},
    };
    for (size_t kind = 0; kind < sizeof hevc / sizeof hevc[0]; kind++) {
        // The hvcC's 22 bytes, then an array of the VPS and one of the SPSs.
        struct buffer body = {NULL, 0}, boxes = {NULL, 0}, entries = {NULL, 0};
        const struct extra *units[] = {&hevc[kind].first, &hevc[kind].second};
        uint8_t head[3] = {0x21, 0, 0};
        for (int unit = 0; unit < 2; unit++) head[2] = (uint8_t)(head[2] + (units[unit]->body != NULL));
        const uint8_t arrays = head[2] != 0 ? 2 : 1;
        append(&body, hvcc_head, 22);
        if (hevc[kind].profile != 0) body.data[body.size - 21] = hevc[kind].profile;
        if (hevc[kind].compatibility != 0) body.data[body.size - 20] = hevc[kind].compatibility;
        append(&body, &arrays, 1);
        append(&body, hvcc_head + 23, 7);
        if (head[2] != 0) append(&body, head, 3);
        for (int unit = 0; unit < 2; unit++) {
            if (units[unit]->body == NULL) continue;
            const uint8_t length[2] = {(uint8_t)(units[unit]->size >> 8), (uint8_t)units[unit]->size};
            append(&body, length, 2);
            append(&body, units[unit]->body, units[unit]->size);
        }
        append_box(&boxes, "hvcC", body.data, body.size);
        if (hevc[kind].colr.body != NULL) append_box(&boxes, "colr", hevc[kind].colr.body, hevc[kind].colr.size);
        append_entry(&entries, "hvc1", video, boxes.data, boxes.size);
        if (hevc[kind].then_avc1) append(&entries, video, video_size);
        struct buffer stsd = stsd_of(&entries, hevc[kind].then_avc1 ? 2 : 1);
        struct buffer file = with_stsd(data, size, 0, &stsd);
        snprintf(prefix, sizeof prefix, "custom %s hvc1 %s", name, hevc[kind].what);
        variant_lines(log, prefix, &file);
        free(stsd.data);
        free(entries.data);
        free(boxes.data);
        free(body.data);
    }
}

// Broken codec configurations (step 2a.2): MPEG4Extractor keeps an avcC, hvcC or esds as its bytes, so its track
// stays; the format's conversion and MPEG4Source's start are what refuse it. The broken track's format is not read
// (variant_lines_but). An avcC of version 0, of 6 and of 4 bytes, an SPS or PPS length past its end, none of its PPS
// count; an hvcC of versions 0 and 2, of 22 and 21 bytes, a unit past its end, 128 arrays of one there (and of all
// 128 there, which keeps its format); an esds whose ES_Descriptor tag is not 3, in the mp4a and alone in a QuickTime
// wave.
static void broken_lines(struct log *log, const char *name, const uint8_t *data, size_t size, const uint8_t *video,
                         size_t video_size, const uint8_t *audio, size_t audio_size, const uint8_t *hvcc_head,
                         size_t hvcc_head_size) {
    const uint8_t *avcc = video + 86;
    const size_t avcc_size = be32(avcc), body_size = avcc_size - 8;
    const size_t sps = avcc_size >= 16 ? (size_t)avcc[14] << 8 | avcc[15] : 0;
    if (avcc_size < 17 + sps + 3) {
        add(log, "custom %s broken: no avcC of a set each", name);
        return;
    }
    char prefix[96];
    const struct {
        const char *what;
        size_t size;  // of the body, 0 for all of it
        size_t at;    // a byte to change, 0 for none
        uint8_t value;
    } avc[] = {
        {"avcC of version 0", 0, 8, 0},
        {"avcC of 6 bytes", 6, 0, 0},
        {"avcC of 4 bytes", 4, 0, 0},
        {"avcC of an SPS past its end", 0, 8 + 6, 0xff},
        {"avcC without its PPS count", 8 + sps, 0, 0},
        {"avcC of a PPS past its end", 0, 8 + 8 + sps + 1, 0xff},
    };
    for (size_t kind = 0; kind < sizeof avc / sizeof avc[0]; kind++) {
        struct buffer config = {NULL, 0}, boxes = {NULL, 0}, entries = {NULL, 0};
        append(&config, avcc + 8, avc[kind].size != 0 ? avc[kind].size : body_size);
        if (avc[kind].at != 0) config.data[avc[kind].at - 8] = avc[kind].value;
        append_box(&boxes, "avcC", config.data, config.size);
        append(&boxes, avcc + avcc_size, video_size - 86 - avcc_size);  // fiel and chrm
        append_entry(&entries, "avc1", video, boxes.data, boxes.size);
        struct buffer stsd = stsd_of(&entries, 1);
        struct buffer file = with_stsd(data, size, 0, &stsd);
        snprintf(prefix, sizeof prefix, "custom %s %s", name, avc[kind].what);
        variant_lines_but(log, prefix, &file, 0);
        free(stsd.data);
        free(entries.data);
        free(boxes.data);
        free(config.data);
    }
    const struct {
        const char *what;
        size_t size;
        size_t at;  // of the body, (size_t)-1 for none
        uint8_t value;
    } hevc[] = {
        {"hvcC of version 0", 0, 0, 0},           {"hvcC of version 2", 0, 0, 2},
        {"hvcC of 22 bytes", 22, (size_t)-1, 0},  {"hvcC of 21 bytes", 21, (size_t)-1, 0},
        {"hvcC of a unit past its end", 0, 26, 0xff}, {"hvcC of 128 arrays", 0, 22, 0x80},
    };
    for (size_t kind = 0; kind < sizeof hevc / sizeof hevc[0]; kind++) {
        struct buffer config = {NULL, 0}, boxes = {NULL, 0}, entries = {NULL, 0};
        append(&config, hvcc_head, hevc[kind].size != 0 ? hevc[kind].size : hvcc_head_size);
        if (hevc[kind].at != (size_t)-1) config.data[hevc[kind].at] = hevc[kind].value;
        append_box(&boxes, "hvcC", config.data, config.size);
        append_entry(&entries, "hvc1", video, boxes.data, boxes.size);
        struct buffer stsd = stsd_of(&entries, 1);
        struct buffer file = with_stsd(data, size, 0, &stsd);
        snprintf(prefix, sizeof prefix, "custom %s %s", name, hevc[kind].what);
        variant_lines_but(log, prefix, &file, 0);
        free(stsd.data);
        free(entries.data);
        free(boxes.data);
        free(config.data);
    }
    {
        // 128 arrays of no units, all there: the count is a char, unsigned on arm64, so the format stays.
        struct buffer config = {NULL, 0}, boxes = {NULL, 0}, entries = {NULL, 0};
        append(&config, hvcc_head, 22);
        append(&config, "\x80", 1);
        for (int array = 0; array < 128; array++) append(&config, "\x20\0\0", 3);
        append_box(&boxes, "hvcC", config.data, config.size);
        append_entry(&entries, "hvc1", video, boxes.data, boxes.size);
        struct buffer stsd = stsd_of(&entries, 1);
        struct buffer file = with_stsd(data, size, 0, &stsd);
        snprintf(prefix, sizeof prefix, "custom %s hvcC of 128 arrays, all there", name);
        variant_lines(log, prefix, &file);
        free(stsd.data);
        free(entries.data);
        free(boxes.data);
        free(config.data);
    }
    const size_t esds_at = 36;  // the mp4a's esds, after the sound entry's 28 bytes
    if (audio_size < esds_at + 13 || memcmp(audio + esds_at + 4, "esds", 4) != 0 || audio[esds_at + 12] != 3) {
        add(log, "custom %s broken: no esds in the mp4a", name);
        return;
    }
    for (int wave = 0; wave < 2; wave++) {
        struct buffer esds = {NULL, 0}, body = {NULL, 0}, entries = {NULL, 0};
        append(&esds, audio + esds_at, be32(audio + esds_at));
        esds.data[12] = 5;  // the ES_Descriptor's tag
        append(&body, audio + 8, 28);
        if (wave) {
            struct buffer inside = {NULL, 0};
            append_box(&inside, "frma", "mp4a", 4);
            append(&inside, esds.data, esds.size);
            append_box(&body, "wave", inside.data, inside.size);
            free(inside.data);
        } else {
            append(&body, esds.data, esds.size);
        }
        append_box(&entries, "mp4a", body.data, body.size);
        struct buffer stsd = stsd_of(&entries, 1);
        struct buffer file = with_stsd(data, size, 1, &stsd);
        snprintf(prefix, sizeof prefix, "custom %s esds of tag 5%s", name, wave ? " in a wave" : "");
        variant_lines_but(log, prefix, &file, 1);
        free(stsd.data);
        free(entries.data);
        free(body.data);
        free(esds.data);
    }
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

    // QuickTime's AAC (step 2a.2): the mp4a's esds in a wave (no aac-profile, the bit rates the format's
    // conversion takes), an mp4a whose esds has bit rates of 0 after one with them (they stay), alone, and before an
    // mp4a with its esds in a wave; maximums below the average, in the mp4a's esds and in the final one, a final
    // one whose maximum is above its own average but below the mp4a's, one of twice the mp4a's average after an
    // mp4a that has its own, and an esds of MP3's object type, which gives no bit rates, before one of 0.
    const size_t esds_at = 36;  // the mp4a's esds, after the sound entry's 28 bytes
    if (audio_size >= esds_at + 8 && memcmp(audio + esds_at + 4, "esds", 4) == 0 &&
        be32(audio + esds_at) <= audio_size - esds_at) {
        const uint8_t *esds = audio + esds_at;
        const size_t esds_size = be32(esds);
        const size_t rates = esds_rates_at(esds, esds_size);
        struct buffer wave_body = {NULL, 0}, wave_entry = {NULL, 0}, zero_entry = {NULL, 0};
        struct buffer below_entry = {NULL, 0}, wave_below = {NULL, 0}, max_zero_entry = {NULL, 0};
        struct buffer wave_quarter = {NULL, 0}, wave_double = {NULL, 0}, mp3_entry = {NULL, 0};
        append_box(&wave_body, "frma", "mp4a", 4);
        append(&wave_body, esds, esds_size);
        struct buffer sound = {NULL, 0};
        append(&sound, audio + 8, 28);
        append_box(&sound, "wave", wave_body.data, wave_body.size);
        append_box(&wave_entry, "mp4a", sound.data, sound.size);
        append(&zero_entry, audio, audio_size);
        if (rates != 0) memset(zero_entry.data + esds_at + rates, 0, 8);
        const size_t wave_esds_at = 56;  // the wave entry's esds, after its sound entry, the wave's header and frma
        append(&below_entry, audio, audio_size);
        append(&max_zero_entry, audio, audio_size);
        append(&wave_below, wave_entry.data, wave_entry.size);
        append(&wave_quarter, wave_entry.data, wave_entry.size);
        append(&wave_double, wave_entry.data, wave_entry.size);
        append(&mp3_entry, audio, audio_size);
        if (rates != 0) mp3_entry.data[esds_at + rates - 5] = 0x6b;  // the DecoderConfigDescriptor's object type
        if (rates != 0) {
            const uint32_t average = be32(audio + esds_at + rates + 4);
            put_be32(below_entry.data + esds_at + rates, average / 2);
            put_be32(max_zero_entry.data + esds_at + rates, 0);
            put_be32(wave_below.data + wave_esds_at + rates, average / 2);
            put_be32(wave_quarter.data + wave_esds_at + rates, average / 2);
            put_be32(wave_quarter.data + wave_esds_at + rates + 4, average / 4);
            put_be32(wave_double.data + wave_esds_at + rates, average * 2);
        }
        const struct {
            const char *what;
            const struct buffer *first, *second;
        } kinds[] = {
            {"mp4a with its esds in a wave", &wave_entry, NULL},
            {"mp4a, then mp4a of bit rates 0", NULL, &zero_entry},
            {"mp4a of bit rates 0", &zero_entry, NULL},
            {"mp4a of bit rates 0, then mp4a with its esds in a wave", &zero_entry, &wave_entry},
            {"mp4a of a maximum below its average", &below_entry, NULL},
            {"mp4a of bit rates 0, then wave of maximum below average", &zero_entry, &wave_below},
            {"mp4a of maximum 0, then wave of half and quarter rates", &max_zero_entry, &wave_quarter},
            {"mp4a, then wave of twice its average as maximum", NULL, &wave_double},
            {"mp4a of MP3's object type, then mp4a of bit rates 0", &mp3_entry, &zero_entry},
        };
        for (size_t kind = 0; kind < sizeof kinds / sizeof kinds[0]; kind++) {
            entries = (struct buffer){NULL, 0};
            if (kinds[kind].first != NULL) append(&entries, kinds[kind].first->data, kinds[kind].first->size);
            else append(&entries, audio, audio_size);
            if (kinds[kind].second != NULL) append(&entries, kinds[kind].second->data, kinds[kind].second->size);
            stsd = stsd_of(&entries, kinds[kind].second != NULL ? 2 : 1);
            file = with_stsd(data, size, 1, &stsd);
            snprintf(prefix, sizeof prefix, "custom %s %s", name, kinds[kind].what);
            variant_lines(log, prefix, &file);
            free(stsd.data);
            free(entries.data);
        }
        free(sound.data);
        free(wave_body.data);
        free(wave_entry.data);
        free(zero_entry.data);
        free(below_entry.data);
        free(max_zero_entry.data);
        free(wave_below.data);
        free(wave_quarter.data);
        free(wave_double.data);
        free(mp3_entry.data);
    } else {
        add(log, "custom %s descriptions: no esds in the mp4a", name);
    }

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

    colour_lines(log, name, data, size, video, video_size, audio, audio_size, kHvcc);
    broken_lines(log, name, data, size, video, video_size, audio, audio_size, kHvcc, sizeof kHvcc);

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
