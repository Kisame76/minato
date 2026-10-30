// ADR-0060 e2d: an ISO BMFF (mp4, m4a, 3gp) parser of Minato's for libmediandk's AMediaExtractor, in place of
// Android's MPEG4Extractor (decision 6). Its input is the app's file and untrusted: every size, count and offset is
// checked against the box that holds it and the file's length, and what it keeps is capped (the k* limits below), so
// that a hostile file ends with an error, never with a read past a buffer or an allocation the file decides. It reads
// the movie box (moov) into memory, at most kMaxMovieBytes, keeps it, and finds a sample in its tables when the
// sample is asked for (SampleTable), as Android's SampleTable and SampleIterator do; the samples are read from the
// file.
// Not parsed in this step: fragmented files (moof, e2d2), which answer kUnsupported, as encrypted tracks (encv,
// enca) do; tracks of other codecs than AVC, HEVC and AAC are left out.
// It is part of libmediandk, built as the rest of runtime/android: bionic's C library alone, without a C++ library
// or exceptions, so memory comes from malloc and a failed allocation is kNoMemory.
#ifndef MINATO_MP4_EXTRACTOR_H
#define MINATO_MP4_EXTRACTOR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

namespace minato::mp4 {

// Android's status_t values of MediaErrors.h, which libmediandk turns into AMEDIA_ERROR_*.
constexpr int kMalformed = -1007;    // ERROR_MALFORMED
constexpr int kUnsupported = -1010;  // ERROR_UNSUPPORTED
constexpr int kIo = -1004;           // ERROR_IO
constexpr int kNoMemory = -12;       // NO_MEMORY
constexpr int kEndOfStream = -1011;  // ERROR_END_OF_STREAM
constexpr int kUnknown = INT32_MIN;  // UNKNOWN_ERROR

// The caps (each a reason to refuse a file, never a size the file asks for).
constexpr uint64_t kMaxMovieBytes = 64u << 20;  // the moov box, read whole; any box but the mdat
constexpr size_t kMaxTracks = 64;
constexpr size_t kMaxCodecData = 1u << 16;  // a track's csd bytes
constexpr int kMaxDepth = 12;  // boxes inside boxes
constexpr uint64_t kMaxInputBytes = 64u << 20;  // a track's largest sample plus 20 (MPEG4Source::start's buffer)
constexpr uint64_t kMaxTableBytes = 200u << 20;  // a track's tables and seek index, as SampleTable's kMaxTotalSize

// Where the bytes come from: `read` fills `size` bytes at `offset` of the source (an fd's range or a path's file) and
// answers how many it read, below 0 for an error; `length` is the source's size in bytes.
struct Source {
    void* context;
    ssize_t (*read)(void* context, uint64_t offset, void* data, size_t size);
    uint64_t length;
};

// One sample, as SampleTable::at gives it.
struct Sample {
    uint64_t offset;             // in the source
    int64_t decode;              // in the track's timescale
    uint32_t size;               // in the file, before any conversion
    int32_t composition_offset;  // ctts's, signed as Android takes it
    uint32_t duration;           // stts's delta, in the track's timescale
    uint64_t chunk_last;         // the last sample of its chunk

    // decode + composition_offset, saturated past int64_t's range.
    int64_t composition() const {
        int64_t sum = 0;
        if (!__builtin_add_overflow(decode, composition_offset, &sum)) return sum;
        return composition_offset > 0 ? INT64_MAX : INT64_MIN;
    }
};

// A track's sample tables in the movie's bytes, which it shares with the other tracks: no memory per sample. A sample
// is found when it is asked for, as Android's SampleIterator finds it: cursors on the chunk runs (stsc), the time runs
// (stts) and the offset runs (ctts) move forward from the last sample asked for, so that reading in decode order
// costs O(1) a sample (and the chunk's earlier sizes once a chunk), and start again from the first run for an earlier
// sample. The cursors move in const calls: one table serves one thread at a time, as an AMediaExtractor does.
struct SampleTable {
    // Where a table's entries begin in `movie`, and how many there are.
    struct Entries {
        size_t at = 0;
        uint32_t count = 0;
    };

    size_t count() const { return samples; }
    // Sample `index`: 0, or kMalformed for one past the count or one whose bytes would end past the 64-bit range or
    // whose time would pass int64_t's.
    int at(size_t index, Sample* out) const;
    // Sample `index`'s times only (decode, composition_offset and duration of `out`), which need no offset: 0, or
    // kMalformed as at.
    int timing(size_t index, Sample* out) const;
    // Whether sample `index` is a sync sample: one stss lists (an entry 0 names the first, as Android reads it), or
    // every sample of a track without an stss.
    bool is_sync(size_t index) const;
    // Sample `index`'s size in the file; `index` below count().
    uint32_t size(size_t index) const;

    // What the parse fills in.
    const uint8_t* movie = nullptr;  // the moov's body, which Tracks owns
    size_t samples = 0;           // as far as all tables describe samples
    uint32_t constant_size = 0;   // stsz's, 0 for one size per sample in `sizes`
    Entries sizes;                // stsz/stz2
    uint32_t size_bits = 32;      // stz2's field size: 4, 8 or 16; stsz's 32
    Entries chunks;               // stco/co64
    uint32_t chunk_bytes = 4;     // 8 for co64
    Entries chunk_runs;           // stsc: first chunk, samples per chunk, description (12 bytes)
    Entries time_runs;            // stts: count, delta
    Entries offset_runs;          // ctts: count, offset (taken as signed)
    bool has_syncs = false;       // false: every sample is a sync sample
    Entries syncs;                // stss, 1-based, when its entries rise
    uint32_t* sorted_syncs = nullptr;  // stss sorted, when its entries do not rise (syncs.count of them; Tracks owns
                                       // them)

    // stss's entry `index` (in its rising order) as SampleTable reads it: 0-based, an entry 0 kept as 0.
    uint32_t sync_entry(size_t index) const;

private:
    uint32_t word(const Entries& entries, size_t index, size_t bytes, size_t field) const;
    bool listed(uint32_t entry) const;  // whether stss has `entry`

    struct Cursor {
        size_t run = 0;        // the run's entry
        uint64_t first = 0;    // the run's first sample
        uint64_t start = 0;    // the run's first time (time runs), or its first chunk's first sample (chunk runs)
    };
    mutable Cursor chunk_cursor_, time_cursor_, offset_cursor_;
    // The last sample found in a chunk and where it is, so that the next sample of the chunk follows from it.
    mutable uint64_t cached_chunk_ = 0, cached_sample_ = 0, cached_offset_ = 0;
};

// Bytes the track owns (Tracks frees them).
struct Bytes {
    uint8_t* data = nullptr;
    size_t size = 0;
};

// A track's samples by composition time, Android's SampleTimeEntry table: made on the first seek, under
// kMaxTableBytes with the tables, and kept with the track (Tracks frees it).
struct SeekIndex {
    struct Entry {
        uint64_t time;    // composition time, in the track's timescale
        uint32_t sample;
    };
    Entry* entries = nullptr;  // by time
    size_t count = 0;
    bool made = false;  // tried: `entries` is null when it could not be made
};

struct Track {
    uint32_t id = 0;            // tkhd's track ID
    const char* mime = nullptr;  // "video/avc", "video/hevc" or "audio/mp4a-latm"
    uint32_t timescale = 0;     // mdhd's
    int64_t duration_us = -1;   // mdhd's duration in µs, truncated; -1 for none (0 or all ones)
    char language[4] = {};      // mdhd's ISO 639-2/T code, "und" for none
    int32_t width = 0, height = 0;       // the visual sample entry's
    int32_t sample_rate = 0, channels = 0;  // the audio sample entry's, the AudioSpecificConfig's when it has them
    Bytes csd0, csd1;                    // AVC: SPS, PPS; HEVC: VPS, SPS and PPS in csd0; each with a start code;
                                         // AAC: the AudioSpecificConfig
    uint32_t nal_length_size = 0;        // 1, 2 or 4 for AVC and HEVC, whose samples become Annex B; 0 for AAC
    int64_t edit_media_time = 0;         // the first edit's media time (the movie's first shown tick), 0 for none
    uint64_t edit_empty_movie = 0;       // an empty first edit's duration as elst gives it, in the movie's timescale
    int64_t edit_empty = 0;              // an empty first edit's duration, in the track's timescale
    bool edit_overflow = false;          // the empty edit does not convert: getTrack fails, so selecting it does
    uint32_t largest_sample = 0;         // in the file
    SampleTable samples;                 // in decode order
    // For the extractor's formats, as MPEG4Extractor keeps them.
    int32_t rotation = 0;                // tkhd's matrix: 0, 90, 180 or 270
    int32_t display_width = 0, display_height = 0;  // tkhd's
    uint32_t avg_bitrate = 0, max_bitrate = 0;      // an mp4a esds's above 0, the last of each, as MPEG4Extractor's
    uint32_t esds_avg_bitrate = 0, esds_max_bitrate = 0;  // the track's final esds's, of any parent
    uint32_t audio_object = 0;           // an mp4a esds's AudioSpecificConfig's audio object type (aac-profile)
    bool has_audio_object = false;       // one was read: an esds in a QuickTime wave alone gives none
    // The first colr of type nclx (11 bytes or more) or nclc (10 or more) in a sample entry, as MPEG4Extractor's
    // parseColorInfo keeps it: ISO/IEC 23091-2's colour primaries, transfer and matrix, and nclx's full range flag.
    bool has_colr = false, colr_full_range = false;
    uint16_t colr_primaries = 0, colr_transfer = 0, colr_matrix = 0;
    // A sample entry's pasp, the last of both spacings above 0, as parsePaspBox sets sar-width and sar-height.
    bool has_sar = false;
    uint32_t sar_width = 0, sar_height = 0;
    // The final hvcC's SPSs' VUIs as HevcParameterSets reads them for convertMetaDataToMessage: each value read
    // replaces the one before; `described` and `hdr` (a transfer of ST 2084 or HLG) stay once an SPS gives them.
    struct Vui {
        bool described = false, hdr = false;
        bool has_full_range = false, has_primaries = false, has_transfer = false, has_matrix = false;
        uint32_t full_range = 0, primaries = 0, transfer = 0, matrix = 0;
    } vui;
    // MPEG4Extractor's plain csd-0, csd-1 and csd-2, each replaced by the next: an av1C's, a vpcC's, a dfLa's or an
    // mhaC's bytes among an entry's boxes, an Opus entry's OpusHead, codec delay and seek pre-roll, an alac entry's
    // cookie. The extractor service's convertMessageToMetaData takes csd-0 before the avcC, hvcC and esds.
    Bytes opaque[3];
    bool has_opaque[3] = {};
    bool csd_opaque = false;             // the format's csd-0 (csd0) is the plain csd-0, with no profile or csd-1
    int32_t bits_per_sample = -1;        // an alac entry's cookie's, -1 for none
    int32_t mpegh_profile = -1, mpegh_layout = -1;  // an mhaC's profile and level and channel layout, -1 for none
    bool format_refused = false;         // the format's conversion refuses its avcC, hvcC or esds
    bool select_refused = false;         // MPEG4Extractor's getTrack refuses its avcC or hvcC: no selecting it
    uint8_t config[13] = {};             // the avcC's or hvcC's first bytes, for the profile and level
    uint8_t config_size = 0;
    uint32_t frame_count = 0;            // stsz's count
    uint32_t max_size = 0;               // the largest size of stsz's count of samples
    uint32_t movie_timescale = 0;        // mvhd's
    bool edit_listed = false;            // an elst of at most 2 entries, whose first media edit gives the gapless
    int64_t edit_first_media_time = 0;   // values: its media time and segment duration when it is the first entry
    uint64_t edit_first_segment = 0;     // (the segment in the movie's timescale)
    SeekIndex seek_index;
};

// The tracks of one movie and the moov's bytes their tables read: parse fills them, and they are freed when the
// Tracks goes or is parsed again. Not copied, as the tracks point into it.
struct Tracks {
    Tracks() = default;
    Tracks(const Tracks&) = delete;
    Tracks& operator=(const Tracks&) = delete;
    ~Tracks() { clear(); }

    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    Track& operator[](size_t index) { return list[index]; }
    const Track& operator[](size_t index) const { return list[index]; }
    const Track* begin() const { return list; }
    const Track* end() const { return list + count; }
    void clear();

    // What the parse fills in.
    Track* list = nullptr;  // kMaxTracks of them
    size_t count = 0;
    uint8_t* movie = nullptr;
};

// Parses the source's movie: 0 with the tracks it knows, else kMalformed, kUnsupported, kIo or kNoMemory.
int parse(const Source& source, Tracks* tracks);

// Android's seek modes (MediaSource::ReadOptions), as AMediaExtractor_seekTo maps its own.
enum SeekMode { kSeekPreviousSync = 0, kSeekNextSync = 1, kSeekClosestSync = 2 };

// Where a track's reading goes on after a seek to `time_us` in `mode`, as MPEG4Source::read with a seek: 0 with the
// sample, kEndOfStream past the end, kUnknown for any other failure. The track's seek index is made on the first.
int seek(Track& track, int64_t time_us, SeekMode mode, size_t* sample);

// A sample's time as the extractor gives it, in µs after the track's edits (below 0 for an encoder's priming), and
// a duration in the track's timescale in µs, both truncated as MPEG4Source's long double arithmetic.
int64_t sample_time_us(const Track& track, const Sample& sample);
int64_t duration_us(const Track& track, uint64_t ticks);

// The size of sample `index` as the extractor gives it (Annex B for AVC and HEVC: each NAL unit after a 4-byte start
// code), or below 0 for an error.
long output_size(const Track& track, size_t index);

// Sample `index` read into `out` as the extractor gives it: its size (a NAL unit of length 0 skipped, one whose length
// passes the sample dropped with the rest of it, as MPEG4Source::read), kMalformed for units that grow past
// max-input-size, kIo, or kNoMemory when `capacity` is below output_size.
long read_sample(const Source& source, const Track& track, size_t index, uint8_t* out, size_t capacity);

// A video track's colour keys as Android 15's format has them, -1 for a key it lacks: the colr's, through the
// extractor service's conversions (ColorUtils' ISO, codec and platform aspects), then for HEVC its VUI's
// android._is-hdr and, when the VUI gave all four values, the three keys from them, unspecified aspects taken from
// the colr (convertMetaDataToMessage).
struct ColorKeys {
    int32_t hdr = -1, standard = -1, transfer = -1, range = -1;
};
ColorKeys color_keys(const Track& track);

// Utils.cpp's parseAacProfileFromCsd: the MediaCodecInfo profile of an AAC csd-0's AudioSpecificConfig (its first
// 16 bits' object type, 5 bits or 32 + 6 more after the escape 31), 0 for none it knows or a csd of under 2 bytes.
int32_t aac_profile(const uint8_t* csd, size_t size);

}  // namespace minato::mp4

#endif
