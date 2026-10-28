// ADR-0060 e2d: an ISO BMFF (mp4, m4a, 3gp) parser of Minato's for libmediandk's AMediaExtractor, in place of
// Android's MPEG4Extractor (decision 6). Its input is the app's file and untrusted: every size, count and offset is
// checked against the box that holds it and the file's length, and what it keeps is capped (the k* limits below), so
// that a hostile file ends with an error, never with a read past a buffer or an allocation the file decides. It reads
// the movie box (moov) into memory, at most kMaxMovieBytes, keeps it, and finds a sample in its tables when the
// sample is asked for (SampleTable), as Android's SampleTable and SampleIterator do; the samples are read from the
// file.
// Not parsed in this step: fragmented files (moof, e2d2), which answer kUnsupported, as encrypted tracks (encv,
// enca) do; tracks of other codecs than AVC, HEVC and AAC are left out.
#ifndef MINATO_MP4_EXTRACTOR_H
#define MINATO_MP4_EXTRACTOR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <memory>
#include <string>
#include <vector>

namespace minato::mp4 {

// Android's status_t values of MediaErrors.h, which libmediandk turns into AMEDIA_ERROR_*.
constexpr int kMalformed = -1007;    // ERROR_MALFORMED
constexpr int kUnsupported = -1010;  // ERROR_UNSUPPORTED
constexpr int kIo = -1004;           // ERROR_IO
constexpr int kNoMemory = -12;       // NO_MEMORY

// The caps (each a reason to refuse a file, never a size the file asks for).
constexpr uint64_t kMaxMovieBytes = 64u << 20;  // the moov box, read whole; any box but the mdat
constexpr size_t kMaxTracks = 64;
constexpr size_t kMaxCodecData = 1u << 16;  // a track's csd bytes
constexpr int kMaxDepth = 12;  // boxes inside boxes
constexpr uint64_t kMaxInputBytes = 64u << 20;  // a track's largest sample plus 20 (MPEG4Source::start's buffer)

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

    int64_t composition() const { return decode + composition_offset; }
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
    // Whether sample `index` is a sync sample: one stss lists, or every sample of a track without an stss.
    bool is_sync(size_t index) const;
    // Sample `index`'s size in the file; `index` below count().
    uint32_t size(size_t index) const;

    // What the parse fills in.
    std::shared_ptr<const std::vector<uint8_t>> movie;
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
    std::vector<uint32_t> sorted_syncs;  // stss sorted, when its entries do not rise

private:
    uint32_t word(const Entries& entries, size_t index, size_t bytes, size_t field) const;

    struct Cursor {
        size_t run = 0;        // the run's entry
        uint64_t first = 0;    // the run's first sample
        uint64_t start = 0;    // the run's first time (time runs), or its first chunk's first sample (chunk runs)
    };
    mutable Cursor chunk_cursor_, time_cursor_, offset_cursor_;
    // The last sample found in a chunk and where it is, so that the next sample of the chunk follows from it.
    mutable uint64_t cached_chunk_ = 0, cached_sample_ = 0, cached_offset_ = 0;
};

struct Track {
    uint32_t id = 0;            // tkhd's track ID
    std::string mime;           // "video/avc", "video/hevc" or "audio/mp4a-latm"
    uint32_t timescale = 0;     // mdhd's
    uint64_t duration = 0;      // mdhd's, in the timescale
    std::string language;       // mdhd's ISO 639-2/T code, "und" for none
    int32_t width = 0, height = 0;       // the visual sample entry's
    int32_t sample_rate = 0, channels = 0;  // the audio sample entry's, the AudioSpecificConfig's when it has them
    std::vector<uint8_t> csd0, csd1;     // AVC: SPS, PPS; HEVC: VPS, SPS and PPS in csd0; each with a start code;
                                         // AAC: the AudioSpecificConfig
    uint32_t nal_length_size = 0;        // 1, 2 or 4 for AVC and HEVC, whose samples become Annex B; 0 for AAC
    int64_t edit_media_time = 0;         // the first edit's media time (the movie's first shown tick), 0 for none
    int64_t edit_empty = 0;              // an empty first edit's duration, in the track's timescale
    uint32_t largest_sample = 0;         // in the file
    SampleTable samples;                 // in decode order
};

// Parses the source's movie: 0 with the tracks it knows, else kMalformed, kUnsupported, kIo or kNoMemory.
int parse(const Source& source, std::vector<Track>* tracks);

// The size of sample `index` as the extractor gives it (Annex B for AVC and HEVC: each NAL unit after a 4-byte start
// code), or below 0 for an error.
long output_size(const Track& track, size_t index);

// Sample `index` read into `out` as the extractor gives it: its size, or kMalformed for NAL units that do not fill
// it exactly, kIo, or kNoMemory when `capacity` is below output_size.
long read_sample(const Source& source, const Track& track, size_t index, uint8_t* out, size_t capacity);

}  // namespace minato::mp4

#endif
