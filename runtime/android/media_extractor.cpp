// libmediandk.so's AMediaExtractor and AMediaDataSource (ADR-0060 e2d part 2a): Android 15's NuMediaExtractor and
// MPEG4Source over Minato's ISO BMFF parser (mp4_extractor.h), answering as they do [verified at android-15.0.0_r1:
// NdkMediaExtractor.cpp, NdkMediaDataSource.cpp, NuMediaExtractor.cpp, MPEG4Extractor.cpp, SampleTable.cpp,
// Utils.cpp and avc_utils.cpp; the answers checked against the emulator by media-extractor-test]:
// - Sources: setDataSourceFd reads a duplicate of the fd from its offset, for its length (the file's rest for a
//   length below 0); setDataSourceCustom reads through the app's AMediaDataSource, whose functions it copies, as
//   NdkDataSource does; setDataSource(location) answers AMEDIA_ERROR_UNSUPPORTED, as Android's NDK does for a path
//   (no http service is made for it) and Minato for any URL (named limit). A second source after one was taken
//   answers AMEDIA_ERROR_UNKNOWN (-EINVAL).
// - A source opens when MPEG4Extractor's sniffers take it (BetterSniffMPEG4's boxes in the first 128 bytes with a
//   compatible brand, then LegacySniffMPEG4's ftyp at 4), else AMEDIA_ERROR_UNKNOWN: no other container is read
//   (named limit: mp3, adts, ogg and webm come later). An opened source whose parse fails has no track and an empty
//   file format, as MPEG4Extractor's failed readMetaData gives.
// - Formats: the keys MPEG4Extractor and convertMetaDataToMessage give AVC, HEVC and AAC tracks, in their order.
// - Reading, as NuMediaExtractor: selectTrack takes no sample yet; the next sample is the selected tracks' earliest
//   by time (on a tie, the track selected first); each track reads its samples in decode order from where its last
//   seek put it, a sample's time after the track's edits (below 0 for an encoder's priming). A track whose sample
//   fails ends there with the failure, which the getters answer once no other track has a sample.
// - seekTo: each selected track as MPEG4Source seeks (mp4::seek), the next sample as above; AMEDIA_ERROR_END_OF_STREAM
//   when no track has one. A time below 0 takes no seek.
// - Not here: encryption (getPsshInfo and getSampleCryptoInfo answer null), cached durations (-1, as Android's for a
//   file), and fragmented files (e2d2: no track).
// One lock per extractor, as NuMediaExtractor's. Bionic's C library alone, as the rest of runtime/android.
#include <errno.h>
#include <fcntl.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mp4_extractor.h"
#include "ogg_extractor.h"

struct AMediaDataSource {
    void *userdata;
    AMediaDataSourceReadAt read_at;
    AMediaDataSourceGetSize get_size;
    AMediaDataSourceClose close;
    AMediaDataSourceGetAvailableSize available_size;
};

namespace {

namespace mp4 = minato::mp4;

// A selected track as NuMediaExtractor's TrackInfo keeps it, with one sample read ahead at most.
struct Selected {
    size_t track;
    size_t next;   // the sample its reading goes on with
    int final;     // 0, mp4::kEndOfStream, or the failure that ended it
    bool held;     // `sample` and `data` hold its next sample
    mp4::Sample sample;
    int64_t time_us;
    bool sync;
    uint8_t *data;
    size_t size, capacity;
};

}  // namespace

struct AMediaExtractor {
    pthread_mutex_t mutex;
    bool opened;  // a source was taken (NuMediaExtractor's mImpl)
    bool parsed;  // and its movie parsed
    int fd;       // a duplicate of setDataSourceFd's, or -1
    uint64_t start;
    AMediaDataSource custom;
    bool has_custom;
    mp4::Source source;
    bool sized;          // the source's length is its size, not unknown
    mp4::Tracks tracks;
    minato::ogg::Reader *ogg;  // an Ogg Opus source's one track, else NULL
    bool ogg_file;       // an Ogg source, with a track or without
    Selected selected[mp4::kMaxTracks];
    size_t selected_count;
};

namespace {

struct Lock {
    explicit Lock(pthread_mutex_t *mutex) : mutex(mutex) { pthread_mutex_lock(mutex); }
    ~Lock() { pthread_mutex_unlock(mutex); }
    pthread_mutex_t *mutex;
};

// Android's status_t answers as NdkMediaError's translate_error gives them.
media_status_t translate(int status) {
    switch (status) {
        case 0: return AMEDIA_OK;
        case mp4::kEndOfStream: return AMEDIA_ERROR_END_OF_STREAM;
        case mp4::kIo: return AMEDIA_ERROR_IO;
        case mp4::kMalformed: return AMEDIA_ERROR_MALFORMED;
        default: return AMEDIA_ERROR_UNKNOWN;
    }
}

ssize_t read_fd(void *context, uint64_t offset, void *data, size_t size) {
    auto *extractor = static_cast<AMediaExtractor *>(context);
    if (offset >= extractor->source.length) return 0;
    const uint64_t left = extractor->source.length - offset;
    if (size > left) size = static_cast<size_t>(left);
    const ssize_t got = pread64(extractor->fd, data, size, static_cast<off64_t>(extractor->start + offset));
    return got < 0 ? -1 : got;
}

ssize_t read_custom(void *context, uint64_t offset, void *data, size_t size) {
    auto *extractor = static_cast<AMediaExtractor *>(context);
    const ssize_t got = extractor->custom.read_at(extractor->custom.userdata, static_cast<off64_t>(offset), data, size);
    return got > static_cast<ssize_t>(size) ? -1 : got;
}

// One read of the source, as the sniffers' DataSourceHelper::readAt gives it: the bytes read, or below 0.
ssize_t read_once(const mp4::Source &source, uint64_t offset, void *data, size_t size) {
    return source.read(source.context, offset, data, size);
}

uint32_t be32(const uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 |
           static_cast<uint32_t>(bytes[2]) << 8 | static_cast<uint32_t>(bytes[3]);
}

uint32_t fourcc(const char (&name)[5]) {
    return static_cast<uint32_t>(name[0]) << 24 | static_cast<uint32_t>(name[1]) << 16 |
           static_cast<uint32_t>(name[2]) << 8 | static_cast<uint32_t>(name[3]);
}

// MPEG4Extractor's isCompatibleBrand.
bool compatible(uint32_t brand) {
    static const char *const brands[] = {"isom", "iso2", "avc1", "hvc1", "hev1", "av01", "vp09", "3gp4",
                                         "mp41", "mp42", "dash", "nvr1", "qt  ", "MSNV", "wmf ", "3g2a",
                                         "3g2b", "mif1", "heic", "msf1", "hevc", "avif", "avis"};
    for (const char *name : brands) {
        if (be32(reinterpret_cast<const uint8_t *>(name)) == brand) return true;
    }
    return false;
}

// MPEG4Extractor's BetterSniffMPEG4: the boxes that start in the first 128 bytes, until a moov, need an ftyp with a
// compatible brand.
bool better_sniff(const mp4::Source &source) {
    uint64_t offset = 0;
    bool good = false, done = false;
    while (!done && offset < 128) {
        uint8_t header[8];
        if (read_once(source, offset, header, 8) < 8) return false;
        uint64_t size = be32(header);
        const uint32_t type = be32(header + 4);
        uint64_t data_offset = offset + 8;
        if (size == 1) {
            uint8_t large[8];
            if (read_once(source, offset + 8, large, 8) < 8) return false;
            size = static_cast<uint64_t>(be32(large)) << 32 | be32(large + 4);
            data_offset += 8;
            if (size < 16 || size > static_cast<uint64_t>(INT64_MAX)) return false;
        } else if (size < 8) {
            return false;
        }
        const uint64_t data_size = size - (data_offset - offset);
        if (type == fourcc("ftyp")) {
            if (data_size < 8) return false;
            const uint64_t brands = (data_size - 8) / 4;
            for (uint64_t index = 0; index < brands + 2; index++) {
                if (index == 1) continue;  // the minor version
                uint8_t brand[4];
                if (read_once(source, data_offset + 4 * index, brand, 4) < 4) return false;
                if (compatible(be32(brand))) {
                    good = true;
                    break;
                }
            }
            if (!good) return false;
        } else if (type == fourcc("moov")) {
            uint64_t end = 0;
            if (__builtin_add_overflow(offset, size, &end) || end > static_cast<uint64_t>(INT64_MAX)) return false;
            done = true;
        }
        if (__builtin_add_overflow(offset, size, &offset) || offset > static_cast<uint64_t>(INT64_MAX)) return false;
    }
    return good;
}

// MPEG4Extractor's LegacySniffMPEG4: an ftyp of a known brand at 4.
bool legacy_sniff(const mp4::Source &source) {
    uint8_t header[8];
    if (read_once(source, 4, header, 8) < 8) return false;
    static const char *const types[] = {"ftypmp42", "ftyp3gr6", "ftyp3gs6", "ftyp3ge6", "ftyp3gg6", "ftypisom",
                                        "ftypM4V ", "ftypM4A ", "ftypf4v ", "ftypkddi", "ftypM4VP", "ftypmif1",
                                        "ftypheic", "ftypmsf1", "ftyphevc", "ftypavif", "ftypavis"};
    if (memcmp(header, "ftyp3gp", 7) == 0) return true;
    for (const char *type : types) {
        if (memcmp(header, type, 8) == 0) return true;
    }
    return false;
}

// The source taken: the sniff, then the parse (initMediaExtractor and the extractor's readMetaData). MPEG4Extractor's
// sniffers first, then OggExtractor's ("OggS" at the start): Ogg Opus has its track, another Ogg none; Ogg Vorbis is
// refused as a source no sniff takes (named limit: Vorbis waits for its decoder).
media_status_t open_source(AMediaExtractor *extractor) {
    if (!better_sniff(extractor->source) && !legacy_sniff(extractor->source)) {
        minato::ogg::Kind kind = minato::ogg::Kind::kNotOgg;
        if (minato::ogg::open(extractor->source, extractor->sized, &kind, &extractor->ogg) != 0) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        if (kind == minato::ogg::Kind::kNotOgg || kind == minato::ogg::Kind::kVorbis) return AMEDIA_ERROR_UNKNOWN;
        extractor->opened = true;
        extractor->ogg_file = true;
        extractor->parsed = extractor->ogg != nullptr;
        return AMEDIA_OK;
    }
    extractor->opened = true;
    extractor->parsed = mp4::parse(extractor->source, &extractor->tracks) == 0;
    if (!extractor->parsed) extractor->tracks.clear();
    return AMEDIA_OK;
}

bool is_video(const mp4::Track &track) { return strncmp(track.mime, "video/", 6) == 0; }
bool is_avc(const mp4::Track &track) { return strcmp(track.mime, "video/avc") == 0; }
bool is_hevc(const mp4::Track &track) { return strcmp(track.mime, "video/hevc") == 0; }

// Utils.cpp's parseAvcProfileLevelFromAvcc, with MediaCodecInfo's values.
void avc_profile(AMediaFormat *format, const mp4::Track &track) {
    if (track.config_size < 4 || track.config[0] != 1) return;
    const uint8_t profile = track.config[1], constraints = track.config[2], level = track.config[3];
    static const uint8_t profiles[][2] = {{66, 0}, {77, 1}, {88, 2}, {100, 3}, {110, 4}, {122, 5}, {244, 6}};
    int32_t code = -1;
    for (const auto &entry : profiles) {
        if (entry[0] == profile) code = 1 << entry[1];  // Baseline 1, Main 2, Extended 4, High 8, 0x10, 0x20, 0x40
    }
    if (code < 0) return;
    if (profile == 66 && (constraints & 0x40)) code = 0x10000;                  // ConstrainedBaseline
    if (profile == 100 && (constraints & 0x0c) == 0x0c) code = 0x80000;        // ConstrainedHigh
    AMediaFormat_setInt32(format, "profile", code);
    static const uint8_t levels[] = {10, 0, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
    int32_t value = -1;
    if (level == 9) value = 2;  // Level1b
    for (size_t index = 0; index < sizeof levels; index++) {
        if (levels[index] == level && level != 0) value = 1 << index;  // Level1 1, Level11 4, ... Level62 0x80000
    }
    if (level == 11 && (profile == 66 || profile == 77 || profile == 88)) value = (constraints & 0x10) ? 2 : 4;
    if (value > 0) AMediaFormat_setInt32(format, "level", value);
}

// Utils.cpp's parseHevcProfileLevelFromHvcc, after the colour keys: Main10 is Main10HDR10 for a color-transfer of
// ST 2084 (isHdr10or10Plus; no hdr-static-info or HDR10+ info is read).
void hevc_profile(AMediaFormat *format, const mp4::Track &track) {
    if (track.config_size < 13 || track.config[0] != 1) return;
    const uint8_t profile = track.config[1] & 0x1f, tier = (track.config[1] & 0x20) >> 5, level = track.config[12];
    int32_t code = profile == 1 || profile == 3 ? 1 : profile == 2 ? 2 : 0;  // Main, Main10
    if (code == 0) {
        if (track.config[2] & 0x40) code = 1;
        else if (track.config[2] & 0x20) code = 2;
        else return;
    }
    int32_t transfer = 0;
    if (code == 2 && AMediaFormat_getInt32(format, "color-transfer", &transfer) && transfer == 6) code = 0x1000;
    AMediaFormat_setInt32(format, "profile", code);
    static const uint8_t levels[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
    for (size_t index = 0; index < sizeof levels; index++) {
        // Main tier: 1, 4, 0x10, ...; high tier: twice the main tier's.
        if (levels[index] != level) continue;
        AMediaFormat_setInt32(format, "level", static_cast<int32_t>(1u << (2 * index + tier)));
    }
}

// Utils.cpp's parseAacProfileFromCsd on csd-0, the final esds's AudioSpecificConfig.
void aac_profile(AMediaFormat *format, const mp4::Track &track) {
    const int32_t profile = mp4::aac_profile(track.csd0.data, track.csd0.size);
    if (profile != 0) AMediaFormat_setInt32(format, "profile", profile);
}

// convertMetaDataToMessage's bit rates of a track that is not video: the mp4a esds's (btrt is not read), the maximum
// whatever the average (its key mapping copies it both ways); then, each when the mp4a gave none, the final esds's,
// the maximum when at least the average there is (the esds's own when the format has none).
void audio_bit_rates(AMediaFormat *format, const mp4::Track &track) {
    const uint32_t avg = track.avg_bitrate, max = track.max_bitrate;
    if (avg > 0) AMediaFormat_setInt32(format, "bitrate", static_cast<int32_t>(avg));
    if (max > 0) AMediaFormat_setInt32(format, "max-bitrate", static_cast<int32_t>(max));
    uint32_t final_avg = track.esds_avg_bitrate;
    if (avg == 0 && final_avg > 0 && final_avg <= INT32_MAX) {
        AMediaFormat_setInt32(format, "bitrate", static_cast<int32_t>(final_avg));
    } else if (avg > 0) {
        final_avg = avg;
    }
    const uint32_t final_max = track.esds_max_bitrate;
    if (max == 0 && final_max > 0 && final_max <= INT32_MAX && final_max >= final_avg) {
        AMediaFormat_setInt32(format, "max-bitrate", static_cast<int32_t>(final_max));
    }
}

// MPEG4Extractor's getTrackMetaData elst step: an AAC track's encoder delay and padding from its first edit.
void gapless(AMediaFormat *format, const mp4::Track &track, int64_t duration_us) {
    if (!track.edit_listed || track.movie_timescale == 0 || track.sample_rate == 0) return;
    if (track.edit_first_segment > static_cast<uint64_t>(INT64_MAX)) return;
    const int64_t segment = static_cast<int64_t>(track.edit_first_segment), media_time = track.edit_first_media_time;
    const int64_t half = track.timescale / 2, rate = track.sample_rate;
    int64_t delay = 0;
    if (media_time > 0) {
        if (__builtin_mul_overflow(media_time, rate, &delay) || __builtin_add_overflow(delay, half, &delay)) return;
        delay /= track.timescale;
        if (delay > INT32_MAX || delay < INT32_MIN) return;
    }
    AMediaFormat_setInt32(format, "encoder-delay", static_cast<int32_t>(delay));
    int64_t padding = 0;
    if (segment > 0) {
        const int64_t movie = track.movie_timescale;
        int64_t scaled = 0, segment_e6 = 0, media_scaled = 0, media_e6 = 0, end = 0, left = 0;
        if (__builtin_mul_overflow(duration_us, movie, &scaled) ||
            __builtin_mul_overflow(segment, 1000000, &segment_e6) ||
            __builtin_mul_overflow(media_time, movie, &media_scaled) ||
            __builtin_mul_overflow(media_scaled, 1000000, &media_e6)) {
            return;
        }
        media_e6 /= track.timescale;
        if (__builtin_add_overflow(segment_e6, media_e6, &end) || __builtin_sub_overflow(scaled, end, &left)) return;
        if (left > 0) {
            int64_t half_e6 = 0, scale_e6 = 0;
            if (__builtin_mul_overflow(left, rate, &padding) ||
                __builtin_mul_overflow(movie / 2, 1000000, &half_e6) ||
                __builtin_mul_overflow(movie, 1000000, &scale_e6) ||
                __builtin_add_overflow(padding, half_e6, &padding)) {
                return;
            }
            padding /= scale_e6;
            if (padding > INT32_MAX) return;
        }
        AMediaFormat_setInt32(format, "encoder-padding", static_cast<int32_t>(padding));
    }
}

// convertMetaDataToMessage's codec data, of the track's own box (the parser's csd): an AVC track's avcC's profile and
// level, then its SPS as csd-0 and its PPS as csd-1; an HEVC track's hvcC's sets as csd-0, then its profile and
// level; an AAC track's DecoderSpecificInfo as csd-0, then its profile. Each csd is there when it is empty too. A
// plain csd-0 of MPEG4Extractor's stands for the box (the parser's plain_config), and its plain csd-1 and csd-2 stay
// unless an avcC's csd-1 replaces the first.
void codec_keys(AMediaFormat *format, const mp4::Track &track) {
    const bool avcc = is_avc(track) && !track.csd_opaque;
    if (avcc) avc_profile(format, track);
    AMediaFormat_setBuffer(format, "csd-0", track.csd0.data, track.csd0.size);  // empty for no sets or no info
    if (avcc) {
        AMediaFormat_setBuffer(format, "csd-1", track.csd1.data, track.csd1.size);
    } else if (track.has_opaque[1]) {
        AMediaFormat_setBuffer(format, "csd-1", track.opaque[1].data, track.opaque[1].size);
    }
    if (track.has_opaque[2]) AMediaFormat_setBuffer(format, "csd-2", track.opaque[2].data, track.opaque[2].size);
    if (is_hevc(track)) hevc_profile(format, track);
    if (!is_video(track)) aac_profile(format, track);
}

// A track's format: MPEG4Extractor's keys through convertMetaDataToMessage, in the order Android 15 lists them.
void track_format(AMediaFormat *format, const mp4::Track &track) {
    const bool video = is_video(track);
    AMediaFormat_setString(format, "mime", track.mime);
    const bool timed = track.duration_us >= 0;
    const int64_t duration_us = track.duration_us;
    if (timed) AMediaFormat_setInt64(format, "durationUs", duration_us);
    if (video) AMediaFormat_setInt32(format, "frame-count", static_cast<int32_t>(track.frame_count));
    AMediaFormat_setInt32(format, "track-id", static_cast<int32_t>(track.id));
    if (!video) audio_bit_rates(format, track);
    AMediaFormat_setString(format, "language", track.language);
    // max-input-size: the largest sample with room for 10 grown NAL lengths, else a size by the picture or 128 KiB.
    uint64_t input = track.max_size + 20ull;
    if (track.max_size == 0) {
        const uint64_t width = video ? static_cast<uint32_t>(track.width) : 1920;
        const uint64_t height = video ? static_cast<uint32_t>(track.height) : 1080;
        input = !video                         ? 128 * 1024
                : is_avc(track) || is_hevc(track) ? ((width + 15) / 16) * ((height + 15) / 16) * 192
                                                   : width * height * 3 / 2;
        input += input / 10;
    }
    if (video) {
        AMediaFormat_setInt32(format, "width", track.width);
        AMediaFormat_setInt32(format, "height", track.height);
        // A pasp's spacings, which the extractor service's convertMessageToMetaData takes as int32_t: above 0 they are
        // the format's; else it refuses the format (BAD_VALUE) with the keys above taken, and the client's format is
        // those alone (observed: a spacing of 2^31 on Android 15's emulator).
        const int32_t sar_width = static_cast<int32_t>(track.sar_width);
        const int32_t sar_height = static_cast<int32_t>(track.sar_height);
        if (track.has_sar) {
            if (sar_width <= 0 || sar_height <= 0) return;
            AMediaFormat_setInt32(format, "sar-width", sar_width);
            AMediaFormat_setInt32(format, "sar-height", sar_height);
        }
        if (track.rotation != 0) AMediaFormat_setInt32(format, "rotation-degrees", track.rotation);
        AMediaFormat_setInt32(format, "display-width", track.display_width);
        AMediaFormat_setInt32(format, "display-height", track.display_height);
        const mp4::ColorKeys colour = mp4::color_keys(track);
        if (colour.range >= 0) AMediaFormat_setInt32(format, "color-range", colour.range);
        if (colour.standard >= 0) AMediaFormat_setInt32(format, "color-standard", colour.standard);
        if (colour.transfer >= 0) AMediaFormat_setInt32(format, "color-transfer", colour.transfer);
        if (colour.hdr >= 0) AMediaFormat_setInt32(format, "android._is-hdr", colour.hdr);
        AMediaFormat_setInt32(format, "max-input-size", static_cast<int32_t>(input));
        if (track.frame_count > 0 && timed && duration_us > 0) {
            const int64_t rate = (static_cast<int64_t>(track.frame_count) * 1000000 + (duration_us >> 1)) / duration_us;
            AMediaFormat_setInt32(format, "frame-rate", static_cast<int32_t>(rate));
        }
        codec_keys(format, track);
    } else {
        AMediaFormat_setInt32(format, "channel-count", track.channels);
        AMediaFormat_setInt32(format, "sample-rate", track.sample_rate);
        // An alac entry's before the mp4a and an mhaC's, as its meta keeps them.
        if (track.bits_per_sample >= 0) AMediaFormat_setInt32(format, "bits-per-sample", track.bits_per_sample);
        if (track.mpegh_profile >= 0) {
            AMediaFormat_setInt32(format, "mpegh-profile-level-indication", track.mpegh_profile);
            AMediaFormat_setInt32(format, "mpegh-reference-channel-layout", track.mpegh_layout);
        }
        if (timed) gapless(format, track, duration_us);
        if (track.has_audio_object) {  // an mp4a esds's, none for one in a QuickTime wave alone
            AMediaFormat_setInt32(format, "aac-profile", static_cast<int32_t>(track.audio_object));
        }
        AMediaFormat_setInt32(format, "max-input-size", static_cast<int32_t>(input));
        codec_keys(format, track);
    }
}

// MyOpusExtractor's meta through convertMetaDataToMessage: OpusHead as csd-0, the pre-skip and seek pre-roll in
// nanoseconds as csd-1 and csd-2, its channels, their mask (audio_channel_out_mask_from_count's) and the duration
// with the source's size.
void ogg_format(AMediaFormat *format, const minato::ogg::Reader *reader) {
    const minato::ogg::Format &ogg = minato::ogg::format(reader);
    AMediaFormat_setString(format, "mime", "audio/opus");
    AMediaFormat_setInt32(format, "track-id", 1);  // the extractor service's numbering
    if (ogg.duration_us >= 0) AMediaFormat_setInt64(format, "durationUs", ogg.duration_us);
    AMediaFormat_setInt32(format, "channel-count", static_cast<int32_t>(ogg.channels));
    static const int32_t masks[] = {0, 0x1, 0x3, 0x7, 0x33, 0x37, 0x3f, 0x13f, 0x63f};
    AMediaFormat_setInt32(format, "channel-mask", ogg.channels <= 8 ? masks[ogg.channels] : 0);
    AMediaFormat_setInt32(format, "sample-rate", 48000);
    AMediaFormat_setBuffer(format, "csd-0", ogg.head, ogg.head_size);
    AMediaFormat_setBuffer(format, "csd-1", &ogg.delay_ns, sizeof ogg.delay_ns);
    AMediaFormat_setBuffer(format, "csd-2", &ogg.pre_roll_ns, sizeof ogg.pre_roll_ns);
}

size_t track_count(const AMediaExtractor *extractor) {
    return extractor->ogg != nullptr ? 1 : extractor->ogg_file ? 0 : extractor->tracks.size();
}

// avc_utils's getNextNALUnit with a start code following the data: the next unit after a start code, or false.
bool next_nal(const uint8_t **data, size_t *size, const uint8_t **start, size_t *length) {
    const uint8_t *bytes = *data;
    const size_t count = *size;
    if (count < 3) return false;
    size_t offset = 0;
    while (offset + 2 < count && !(bytes[offset] == 0 && bytes[offset + 1] == 0 && bytes[offset + 2] == 1)) offset++;
    if (offset + 2 >= count) return false;
    offset += 3;
    const size_t first = offset;
    for (;;) {
        while (offset < count && bytes[offset] != 1) offset++;
        if (offset == count) {
            offset = count + 2;
            break;
        }
        if (bytes[offset - 1] == 0 && bytes[offset - 2] == 0) break;
        offset++;
    }
    size_t end = offset - 2;
    while (end > first + 1 && bytes[end - 1] == 0) end--;
    *start = bytes + first;
    *length = end - first;
    if (offset + 2 < count) {
        *data = bytes + offset - 2;
        *size = count - offset + 2;
    } else {
        *data = nullptr;
        *size = 0;
    }
    return true;
}

// MPEG4Source's temporal layer of a sample in Annex B: AVC's FindAVCLayerId (an SVC unit, type 14, in its first 32
// bytes), HEVC's parseHEVCLayerId (its first unit after the NAL length's bytes, as MPEG4Source skips them), or -1.
int32_t layer_of(const mp4::Track &track, const uint8_t *data, size_t size) {
    if (is_avc(track)) {
        const uint8_t *bytes = data, *start = nullptr;
        size_t left = size > 32 ? 32 : size, length = 0;
        while (bytes != nullptr && next_nal(&bytes, &left, &start, &length)) {
            if (length > 0 && (start[0] & 0x1f) == 0xe) return length >= 4 ? (start[3] >> 5) & 7 : 0;
        }
        return 0;
    }
    if (size < track.nal_length_size + 2) return -1;
    const uint16_t header = static_cast<uint16_t>(data[track.nal_length_size] << 8 | data[track.nal_length_size + 1]);
    const uint16_t type = (header >> 9) & 0x3f;
    return type >= 2 && type <= 5 ? static_cast<int32_t>(header & 7) - 1 : 0;
}

// The selected track's next sample read, as MPEG4Source::read gives it, unless it holds one or has ended.
void fetch(AMediaExtractor *extractor, Selected &selected) {
    if (selected.held || selected.final != 0) return;
    if (extractor->ogg != nullptr) {
        minato::ogg::Packet packet{};
        const int read = minato::ogg::next(extractor->ogg, &packet);
        if (read != 0) {
            selected.final = read;
            return;
        }
        if (selected.capacity < packet.size + 1) {
            auto *grown = static_cast<uint8_t *>(realloc(selected.data, packet.size + 1));
            if (grown == nullptr) {
                selected.final = mp4::kNoMemory;
                return;
            }
            selected.data = grown;
            selected.capacity = packet.size + 1;
        }
        memcpy(selected.data, packet.data, packet.size);
        selected.size = packet.size;
        selected.time_us = packet.time_us;
        selected.sync = true;  // OggSource marks every packet a sync frame
        selected.held = true;
        selected.next++;
        return;
    }
    const mp4::Track &track = extractor->tracks[selected.track];
    if (selected.next >= track.samples.count()) {
        selected.final = mp4::kEndOfStream;
        return;
    }
    mp4::Sample sample{};
    if (track.samples.at(selected.next, &sample) != 0 || sample.composition() < 0) {
        selected.final = mp4::kUnknown;  // getMetaDataForSample's failure
        return;
    }
    const long longest = mp4::output_size(track, selected.next);
    if (longest < 0) {
        selected.final = mp4::kUnknown;  // a sample past the buffer's size
        return;
    }
    if (selected.capacity < static_cast<size_t>(longest)) {
        auto *grown = static_cast<uint8_t *>(realloc(selected.data, static_cast<size_t>(longest) + 1));
        if (grown == nullptr) {
            selected.final = mp4::kNoMemory;
            return;
        }
        selected.data = grown;
        selected.capacity = static_cast<size_t>(longest) + 1;
    }
    const long read = mp4::read_sample(extractor->source, track, selected.next, selected.data, selected.capacity);
    if (read < 0) {
        selected.final = static_cast<int>(read);
        return;
    }
    selected.sample = sample;
    selected.size = static_cast<size_t>(read);
    selected.time_us = mp4::sample_time_us(track, sample);
    selected.sync = track.samples.is_sync(selected.next);
    selected.held = true;
    selected.next++;
}

// NuMediaExtractor's fetchAllTrackSamples without a seek: the selected track with the earliest sample, the first
// such on a tie, else a track's failure or kEndOfStream.
int earliest(AMediaExtractor *extractor) {
    int found = mp4::kEndOfStream;
    for (size_t index = 0; index < extractor->selected_count; index++) {
        Selected &selected = extractor->selected[index];
        fetch(extractor, selected);
        if (selected.final != 0 && selected.final != mp4::kEndOfStream && !selected.held) return selected.final;
        if (!selected.held) continue;
        if (found < 0 || selected.time_us < extractor->selected[found].time_us) found = static_cast<int>(index);
    }
    return found;
}

void drop(Selected &selected) { selected.held = false; }

}  // namespace

extern "C" {

AMediaExtractor *AMediaExtractor_new() {
    auto *extractor = static_cast<AMediaExtractor *>(calloc(1, sizeof(AMediaExtractor)));
    if (extractor == nullptr) return nullptr;
    pthread_mutex_init(&extractor->mutex, nullptr);
    extractor->fd = -1;
    return extractor;
}

media_status_t AMediaExtractor_delete(AMediaExtractor *extractor) {
    if (extractor == nullptr) return AMEDIA_OK;
    // ~NuMediaExtractor closes the source it took (one a sniff accepted): an app's close callback, with its userdata.
    if (extractor->opened && extractor->has_custom) AMediaDataSource_close(&extractor->custom);
    for (size_t index = 0; index < extractor->selected_count; index++) free(extractor->selected[index].data);
    extractor->tracks.clear();
    minato::ogg::close(extractor->ogg);
    if (extractor->fd >= 0) close(extractor->fd);
    pthread_mutex_destroy(&extractor->mutex);
    free(extractor);
    return AMEDIA_OK;
}

media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *extractor, int fd, off64_t offset, off64_t length) {
    Lock lock(&extractor->mutex);
    if (extractor->opened) return AMEDIA_ERROR_UNKNOWN;  // -EINVAL
    const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) return AMEDIA_ERROR_UNKNOWN;
    struct stat status;
    if (offset < 0 || fstat(copy, &status) != 0) {
        close(copy);
        return AMEDIA_ERROR_UNKNOWN;
    }
    if (length < 0) length = status.st_size > offset ? status.st_size - offset : 0;
    if (extractor->fd >= 0) close(extractor->fd);
    extractor->fd = copy;
    extractor->start = static_cast<uint64_t>(offset);
    extractor->source = mp4::Source{extractor, read_fd, static_cast<uint64_t>(length)};
    extractor->sized = true;
    const media_status_t answer = open_source(extractor);
    if (answer != AMEDIA_OK) {
        // As NuMediaExtractor drops the FileSource of a source no sniff takes.
        close(copy);
        extractor->fd = -1;
    }
    return answer;
}

media_status_t AMediaExtractor_setDataSource(AMediaExtractor *extractor, const char *location) {
    (void)extractor;
    (void)location;
    return AMEDIA_ERROR_UNSUPPORTED;  // "can't create http service"
}

media_status_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *extractor, AMediaDataSource *source) {
    Lock lock(&extractor->mutex);
    if (extractor->opened) return AMEDIA_ERROR_UNKNOWN;
    if (source == nullptr || source->read_at == nullptr || source->userdata == nullptr) return AMEDIA_ERROR_UNKNOWN;
    extractor->custom = *source;  // the sniff reads through it
    ssize_t size = source->get_size != nullptr ? source->get_size(source->userdata) : -1;
    extractor->source = mp4::Source{extractor, read_custom, size < 0 ? static_cast<uint64_t>(INT64_MAX)
                                                                     : static_cast<uint64_t>(size)};
    extractor->sized = size >= 0;
    const media_status_t answer = open_source(extractor);
    // Only a source the extractor took is closed when it goes; Android never closes one a sniff refused.
    extractor->has_custom = answer == AMEDIA_OK;
    return answer;
}

AMediaFormat *AMediaExtractor_getFileFormat(AMediaExtractor *extractor) {
    Lock lock(&extractor->mutex);
    AMediaFormat *format = AMediaFormat_new();
    if (!extractor->parsed || format == nullptr) return format;
    if (extractor->ogg != nullptr) {
        AMediaFormat_setString(format, "mime", "audio/ogg");  // MEDIA_MIMETYPE_CONTAINER_OGG, the file meta's
        return format;
    }
    bool video = false, audio = false;
    for (const mp4::Track &track : extractor->tracks) {
        video |= is_video(track);
        audio |= !is_video(track);
    }
    AMediaFormat_setString(format, "mime", video ? "video/mp4" : audio ? "audio/mp4" : "application/octet-stream");
    return format;
}

size_t AMediaExtractor_getTrackCount(AMediaExtractor *extractor) {
    Lock lock(&extractor->mutex);
    return track_count(extractor);
}

AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *extractor, size_t index) {
    Lock lock(&extractor->mutex);
    AMediaFormat *format = AMediaFormat_new();
    if (extractor->ogg != nullptr) {
        if (format != nullptr && index == 0) ogg_format(format, extractor->ogg);
        return format;
    }
    // A track whose format Utils' conversion refuses: Android's NDK gives a format of no message, whose getters
    // dereference it; Minato's has no keys (and the JNI's getTrackFormat throws, as Android's).
    if (format != nullptr && index < extractor->tracks.size() && !extractor->tracks[index].format_refused) {
        track_format(format, extractor->tracks[index]);
    }
    return format;
}

media_status_t AMediaExtractor_selectTrack(AMediaExtractor *extractor, size_t index) {
    Lock lock(&extractor->mutex);
    if (!extractor->opened || index >= track_count(extractor)) return AMEDIA_ERROR_UNKNOWN;  // -EINVAL, -ERANGE
    for (size_t at = 0; at < extractor->selected_count; at++) {
        if (extractor->selected[at].track == index) return AMEDIA_OK;
    }
    if (extractor->ogg != nullptr) {
        Selected &selected = extractor->selected[extractor->selected_count++];
        memset(&selected, 0, sizeof selected);
        return AMEDIA_OK;
    }
    // getTrack fails for a track whose empty edit does not convert or whose avcC or hvcC it refuses, and
    // MPEG4Source::start for one whose max-input-size passes 64 MiB: all are malformed.
    const mp4::Track &track = extractor->tracks[index];
    if (track.edit_overflow || track.select_refused || track.max_size + 20ull > mp4::kMaxInputBytes) {
        return AMEDIA_ERROR_MALFORMED;
    }
    Selected &selected = extractor->selected[extractor->selected_count++];
    memset(&selected, 0, sizeof selected);
    selected.track = index;
    return AMEDIA_OK;
}

media_status_t AMediaExtractor_unselectTrack(AMediaExtractor *extractor, size_t index) {
    Lock lock(&extractor->mutex);
    if (!extractor->opened || index >= track_count(extractor)) return AMEDIA_ERROR_UNKNOWN;
    for (size_t at = 0; at < extractor->selected_count; at++) {
        if (extractor->selected[at].track != index) continue;
        free(extractor->selected[at].data);
        memmove(&extractor->selected[at], &extractor->selected[at + 1],
                (extractor->selected_count - at - 1) * sizeof(Selected));
        extractor->selected_count--;
        break;
    }
    return AMEDIA_OK;
}

ssize_t AMediaExtractor_readSampleData(AMediaExtractor *extractor, uint8_t *buffer, size_t capacity) {
    Lock lock(&extractor->mutex);
    const int found = earliest(extractor);
    if (found < 0) return -1;
    const Selected &selected = extractor->selected[found];
    if (capacity < selected.size) return -1;  // -ENOMEM
    memcpy(buffer, selected.data, selected.size);
    return static_cast<ssize_t>(selected.size);
}

ssize_t AMediaExtractor_getSampleSize(AMediaExtractor *extractor) {
    Lock lock(&extractor->mutex);
    const int found = earliest(extractor);
    return found < 0 ? -1 : static_cast<ssize_t>(extractor->selected[found].size);
}

uint32_t AMediaExtractor_getSampleFlags(AMediaExtractor *extractor) {
    Lock lock(&extractor->mutex);
    const int found = earliest(extractor);
    if (found < 0) return static_cast<uint32_t>(-1);
    return extractor->selected[found].sync ? AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC : 0;
}

int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *extractor) {
    Lock lock(&extractor->mutex);
    const int found = earliest(extractor);
    return found < 0 ? -1 : static_cast<int>(extractor->selected[found].track);
}

int64_t AMediaExtractor_getSampleTime(AMediaExtractor *extractor) {
    Lock lock(&extractor->mutex);
    const int found = earliest(extractor);
    return found < 0 ? -1 : extractor->selected[found].time_us;
}

bool AMediaExtractor_advance(AMediaExtractor *extractor) {
    Lock lock(&extractor->mutex);
    const int found = earliest(extractor);
    if (found < 0) return false;
    drop(extractor->selected[found]);
    return earliest(extractor) >= 0;
}

media_status_t AMediaExtractor_seekTo(AMediaExtractor *extractor, int64_t time_us, SeekMode mode) {
    Lock lock(&extractor->mutex);
    const mp4::SeekMode seek_mode = mode == AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC  ? mp4::kSeekPreviousSync
                                    : mode == AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC ? mp4::kSeekClosestSync
                                                                                : mp4::kSeekNextSync;
    // fetchAllTrackSamples with a seek: each track in turn seeks and reads, and a failure but the end stops the seek
    // there, the tracks after it left as they were.
    if (time_us >= 0) {
        for (size_t index = 0; index < extractor->selected_count; index++) {
            Selected &selected = extractor->selected[index];
            selected.held = false;
            selected.final = 0;
            size_t sample = 0;
            // OggSource's seek, in any mode: a failure is UNKNOWN_ERROR.
            const int answer = extractor->ogg != nullptr
                                       ? minato::ogg::seek(extractor->ogg, time_us)
                                       : mp4::seek(extractor->tracks[selected.track], time_us, seek_mode, &sample);
            if (answer != 0) {
                selected.final = answer;
            } else {
                selected.next = sample;
                fetch(extractor, selected);
            }
            if (selected.final != 0 && selected.final != mp4::kEndOfStream && !selected.held) {
                return AMEDIA_ERROR_END_OF_STREAM;
            }
        }
    }
    return earliest(extractor) < 0 ? AMEDIA_ERROR_END_OF_STREAM : AMEDIA_OK;
}

PsshInfo *AMediaExtractor_getPsshInfo(AMediaExtractor *extractor) {
    (void)extractor;
    return nullptr;
}

AMediaCodecCryptoInfo *AMediaExtractor_getSampleCryptoInfo(AMediaExtractor *extractor) {
    (void)extractor;
    return nullptr;
}

int64_t AMediaExtractor_getCachedDuration(AMediaExtractor *extractor) {
    (void)extractor;
    return -1;
}

media_status_t AMediaExtractor_getSampleFormat(AMediaExtractor *extractor, AMediaFormat *format) {
    if (format == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock lock(&extractor->mutex);
    const int found = earliest(extractor);
    if (found < 0) return translate(found);
    const Selected &selected = extractor->selected[found];
    AMediaFormat_clear(format);
    if (extractor->ogg != nullptr) {
        // The track and its mime (unverified on Android, whose OggSource packets carry their time and sync flag).
        AMediaFormat_setInt32(format, "track-index", 0);
        AMediaFormat_setString(format, "mime", "audio/opus");
        return AMEDIA_OK;
    }
    const mp4::Track &track = extractor->tracks[selected.track];
    if (is_avc(track) || is_hevc(track)) {
        const int32_t layer = layer_of(track, selected.data, selected.size);
        if (layer >= 0) AMediaFormat_setInt32(format, "temporal-layer-id", layer);
    }
    AMediaFormat_setInt32(format, "track-index", static_cast<int32_t>(selected.track));
    AMediaFormat_setString(format, "mime", track.mime);
    AMediaFormat_setInt64(format, "durationUs", mp4::duration_us(track, selected.sample.duration));
    AMediaFormat_setInt64(format, "sample-file-offset", static_cast<int64_t>(selected.sample.offset));
    if (selected.next - 1 == selected.sample.chunk_last) {
        AMediaFormat_setInt64(format, "last-sample-index-in-chunk", static_cast<int64_t>(selected.sample.chunk_last));
    }
    return AMEDIA_OK;
}

AMediaDataSource *AMediaDataSource_new() {
    return static_cast<AMediaDataSource *>(calloc(1, sizeof(AMediaDataSource)));
}

AMediaDataSource *AMediaDataSource_newUri(const char *uri, int count, const char *const *headers) {
    (void)uri;
    (void)count;
    (void)headers;
    return nullptr;  // no http service (named limit)
}

void AMediaDataSource_delete(AMediaDataSource *source) { free(source); }

void AMediaDataSource_setUserdata(AMediaDataSource *source, void *userdata) { source->userdata = userdata; }

void AMediaDataSource_setReadAt(AMediaDataSource *source, AMediaDataSourceReadAt read_at) {
    source->read_at = read_at;
}

void AMediaDataSource_setGetSize(AMediaDataSource *source, AMediaDataSourceGetSize get_size) {
    source->get_size = get_size;
}

void AMediaDataSource_setClose(AMediaDataSource *source, AMediaDataSourceClose close) { source->close = close; }

void AMediaDataSource_close(AMediaDataSource *source) {
    if (source->close != nullptr && source->userdata != nullptr) source->close(source->userdata);
}

void AMediaDataSource_setGetAvailableSize(AMediaDataSource *source, AMediaDataSourceGetAvailableSize available) {
    source->available_size = available;
}

}  // extern "C"
