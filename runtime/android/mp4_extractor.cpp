// ADR-0060 e2d: libmediandk's ISO BMFF parser (mp4_extractor.h). The top level is walked on the source, box by box;
// the moov box is read whole (at most kMaxMovieBytes) and parsed in memory through Span, whose every read is checked
// against its end, so a box never reaches past the one that holds it. Sample tables are expanded into one Sample per
// sample, at most kMaxSamples all tracks together; tables that disagree on the count end the track at the shortest
// one, as Android's SampleTable refuses samples past what all its tables describe.
#include "mp4_extractor.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace minato::mp4 {
namespace {

// A checked view of bytes: every read past its end sets `bad` and answers 0.
struct Span {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t at = 0;
    bool bad = false;

    size_t left() const { return bad ? 0 : size - at; }
    bool take(size_t count) {
        if (bad || count > size - at) {
            bad = true;
            return false;
        }
        at += count;
        return true;
    }
    uint64_t number(size_t bytes) {
        if (!take(bytes)) return 0;
        uint64_t value = 0;
        for (size_t index = at - bytes; index < at; index++) value = value << 8 | data[index];
        return value;
    }
    uint8_t u8() { return static_cast<uint8_t>(number(1)); }
    uint16_t u16() { return static_cast<uint16_t>(number(2)); }
    uint32_t u32() { return static_cast<uint32_t>(number(4)); }
    uint64_t u64() { return number(8); }
    // The next `count` bytes as a span of their own, or a bad one.
    Span sub(size_t count) {
        Span part;
        if (take(count)) part = Span{data + at - count, count, 0, false};
        else part.bad = true;
        return part;
    }
};

constexpr uint32_t type_of(const char (&name)[5]) {
    return static_cast<uint32_t>(name[0]) << 24 | static_cast<uint32_t>(name[1]) << 16 |
           static_cast<uint32_t>(name[2]) << 8 | static_cast<uint32_t>(name[3]);
}

// A box inside a span: its type and body, or false at the span's end or for a box that does not fit (`bad` set).
bool next_box(Span& in, uint32_t* type, Span* body) {
    if (in.left() == 0) return false;
    const size_t start = in.at;
    uint64_t size = in.u32();
    *type = in.u32();
    if (size == 1) {
        size = in.u64();
    } else if (size == 0) {
        size = in.size - start;  // to the end of the box that holds it
    }
    const size_t header = in.at - start;
    if (in.bad || size < header || size - header > in.left()) {
        in.bad = true;
        return false;
    }
    *body = in.sub(static_cast<size_t>(size - header));
    return !body->bad;
}

struct Parser {
    std::vector<Track>* tracks;
    uint64_t source_length;  // the source's: a track's samples of one size never number more than it holds
    int error = 0;
    size_t samples_left = kMaxSamples;  // what all tracks together may still make
    bool skip = false;  // the track being read is left out, as Android's verifyTrack leaves it out

    int fail(int answer) {
        if (error == 0) error = answer;
        return error;
    }

    // The sample tables of one track, read before they are combined.
    struct Tables {
        std::vector<uint32_t> sizes;  // stsz/stz2; empty with a constant size
        uint32_t constant_size = 0;
        uint32_t size_count = 0;
        std::vector<uint64_t> chunks;                      // stco/co64
        std::vector<std::array<uint32_t, 2>> chunk_runs;   // stsc: first chunk, samples per chunk
        std::vector<std::array<uint32_t, 2>> time_runs;    // stts: count, delta
        std::vector<std::array<int64_t, 2>> offset_runs;   // ctts: count, offset
        std::vector<uint32_t> syncs;                       // stss, 1-based; `has_syncs` false: all
        bool has_syncs = false;
        bool has_sizes = false, has_chunks = false, has_chunk_runs = false, has_times = false, has_offsets = false;
    };

    // A count of entries of `entry` bytes each that the body must hold, or false (malformed).
    static bool fits(const Span& body, uint64_t count, size_t entry) {
        return count <= body.left() / entry && count <= kMaxSamples;
    }

    void read_avcc(Span body, Track& track) {
        body.take(4);  // version, profile, compatibility, level
        track.nal_length_size = (body.u8() & 3u) + 1;
        const uint8_t sps_count = body.u8() & 0x1f;
        for (uint8_t index = 0; index < sps_count; index++) {
            const uint16_t length = body.u16();
            Span set = body.sub(length);
            if (index == 0 && !set.bad) append_set(track.csd0, set);
        }
        const uint8_t pps_count = body.u8();
        for (uint8_t index = 0; index < pps_count; index++) {
            const uint16_t length = body.u16();
            Span set = body.sub(length);
            if (index == 0 && !set.bad) append_set(track.csd1, set);
        }
        if (body.bad || track.csd0.empty()) fail(kMalformed);
    }

    void read_hvcc(Span body, Track& track) {
        body.take(21);
        track.nal_length_size = (body.u8() & 3u) + 1;
        const uint8_t arrays = body.u8();
        for (uint8_t array = 0; array < arrays && !body.bad; array++) {
            body.u8();  // completeness and the NAL unit type
            const uint16_t count = body.u16();
            for (uint16_t index = 0; index < count && !body.bad; index++) {
                const uint16_t length = body.u16();
                Span set = body.sub(length);
                if (!set.bad) append_set(track.csd0, set);  // VPS, SPS and PPS, each after a start code
            }
        }
        if (body.bad || track.csd0.empty()) fail(kMalformed);
    }

    void append_set(std::vector<uint8_t>& csd, const Span& set) {
        if (csd.size() + 4 + set.size > kMaxCodecData) {
            fail(kMalformed);
            return;
        }
        static const uint8_t start[] = {0, 0, 0, 1};
        csd.insert(csd.end(), start, start + 4);
        csd.insert(csd.end(), set.data, set.data + set.size);
    }

    // A descriptor's length in ISO 14496-1's variable form, at most four bytes.
    static uint32_t descriptor_length(Span& body) {
        uint32_t length = 0;
        for (int byte = 0; byte < 4; byte++) {
            const uint8_t part = body.u8();
            length = length << 7 | (part & 0x7fu);
            if ((part & 0x80) == 0) break;
        }
        return length;
    }

    // esds: ES_Descriptor (3) > DecoderConfigDescriptor (4) > DecoderSpecificInfo (5), the AudioSpecificConfig.
    void read_esds(Span body, Track& track) {
        body.take(4);  // version and flags
        if (body.u8() != 3) return fail_void(kMalformed);
        Span es = body.sub(descriptor_length(body));
        es.take(2);  // ES_ID
        const uint8_t es_flags = es.u8();
        if (es_flags & 0x80) es.take(2);              // dependsOn_ES_ID
        if (es_flags & 0x40) es.take(es.u8());        // URL
        if (es_flags & 0x20) es.take(2);              // OCR_ES_Id
        if (es.u8() != 4) return fail_void(kMalformed);
        Span config = es.sub(descriptor_length(es));
        const uint8_t object = config.u8();
        config.take(12);  // stream type, buffer size, bit rates
        if (object != 0x40 && object != 0x66 && object != 0x67 && object != 0x68) {
            track.mime.clear();  // not AAC: a track this parser leaves out
            return;
        }
        if (config.left() > 0 && config.u8() == 5) {
            Span info = config.sub(descriptor_length(config));
            if (info.bad || info.size > kMaxCodecData) return fail_void(kMalformed);
            track.csd0.assign(info.data, info.data + info.size);
            read_audio_config(track);
        }
        if (body.bad || es.bad || config.bad) fail(kMalformed);
    }

    void fail_void(int answer) { fail(answer); }

    // The AudioSpecificConfig's sampling frequency and channel configuration, when it names them.
    static void read_audio_config(Track& track) {
        static const int32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000,  7350};
        if (track.csd0.size() < 2) return;
        const uint32_t bits = static_cast<uint32_t>(track.csd0[0]) << 8 | track.csd0[1];
        const uint32_t object = bits >> 11, frequency = (bits >> 7) & 0xf, channels = (bits >> 3) & 0xf;
        if (object == 0 || object == 31 || frequency == 15) return;  // escapes: the sample entry's values stay
        if (frequency < 13) track.sample_rate = rates[frequency];
        if (channels != 0) track.channels = static_cast<int32_t>(channels);
    }

    void read_stsd(Span body, Track& track) {
        body.take(4);
        const uint32_t entries = body.u32();
        uint32_t type = 0;
        Span entry;
        if (entries == 0 || !next_box(body, &type, &entry)) return fail_void(kMalformed);
        // Only the first entry: a track whose description changes is described by its first, as Android's.
        if (type == type_of("encv") || type == type_of("enca")) return fail_void(kUnsupported);
        const bool avc = type == type_of("avc1") || type == type_of("avc3");
        const bool hevc = type == type_of("hvc1") || type == type_of("hev1");
        if (avc || hevc) {
            entry.take(24);
            track.width = entry.u16();
            track.height = entry.u16();
            entry.take(50);
            track.mime = avc ? "video/avc" : "video/hevc";
            Span part;
            for (uint32_t inner = 0; next_box(entry, &inner, &part);) {
                if (inner == type_of("sinf")) return fail_void(kUnsupported);
                if (avc && inner == type_of("avcC")) read_avcc(part, track);
                if (hevc && inner == type_of("hvcC")) read_hvcc(part, track);
            }
            if (track.nal_length_size == 0) skip = true;  // no avcC or hvcC: no codec configuration
        } else if (type == type_of("mp4a")) {
            entry.take(8);
            const uint16_t version = entry.u16();
            entry.take(6);
            track.channels = entry.u16();
            entry.take(6);
            track.sample_rate = static_cast<int32_t>(entry.u32() >> 16);
            if (version == 1) entry.take(16);
            if (version == 2) entry.take(36);
            track.mime = "audio/mp4a-latm";
            Span part;
            for (uint32_t inner = 0; next_box(entry, &inner, &part);) {
                if (inner == type_of("sinf")) return fail_void(kUnsupported);
                if (inner == type_of("esds")) read_esds(part, track);
            }
        }
        if (entry.bad) fail(kMalformed);
    }

    void read_stbl(Span stbl, Track& track, Tables& tables) {
        uint32_t type = 0;
        Span body;
        // As Android's SampleTable, whose setters refuse a second table of a kind: such a file is malformed.
        const auto twice = [this](bool* seen) {
            if (*seen) fail(kMalformed);
            *seen = true;
            return error != 0;
        };
        while (error == 0 && next_box(stbl, &type, &body)) {
            if (type == type_of("stsd")) {
                read_stsd(body, track);
            } else if (type == type_of("stsz")) {
                if (twice(&tables.has_sizes)) return;
                body.take(4);
                tables.constant_size = body.u32();
                const uint32_t count = body.u32();
                tables.size_count = count;
                if (tables.constant_size == 0) {
                    if (!fits(body, count, 4)) return fail_void(kMalformed);
                    tables.sizes.resize(count);
                    for (uint32_t& size : tables.sizes) size = body.u32();
                } else if (count > kMaxSamples) {
                    return fail_void(kMalformed);
                }
            } else if (type == type_of("stz2")) {
                if (twice(&tables.has_sizes)) return;
                body.take(7);
                const uint8_t field = body.u8();
                const uint32_t count = body.u32();
                if ((field != 4 && field != 8 && field != 16) || count > kMaxSamples ||
                    static_cast<uint64_t>(count) * field > static_cast<uint64_t>(body.left()) * 8) {
                    return fail_void(kMalformed);
                }
                tables.size_count = count;
                tables.sizes.resize(count);
                for (uint32_t index = 0; index < count; index++) {
                    if (field == 4) {
                        const uint8_t pair = body.data[body.at + index / 2];
                        tables.sizes[index] = index % 2 == 0 ? pair >> 4 : pair & 0xfu;
                    } else {
                        tables.sizes[index] = field == 8 ? body.u8() : body.u16();
                    }
                }
            } else if (type == type_of("stco") || type == type_of("co64")) {
                if (twice(&tables.has_chunks)) return;
                body.take(4);
                const uint32_t count = body.u32();
                const size_t entry = type == type_of("stco") ? 4 : 8;
                if (!fits(body, count, entry)) return fail_void(kMalformed);
                tables.chunks.resize(count);
                for (uint64_t& offset : tables.chunks) offset = body.number(entry);
            } else if (type == type_of("stsc")) {
                if (twice(&tables.has_chunk_runs)) return;
                body.take(4);
                const uint32_t count = body.u32();
                if (!fits(body, count, 12)) return fail_void(kMalformed);
                tables.chunk_runs.resize(count);
                for (auto& run : tables.chunk_runs) {
                    run[0] = body.u32();
                    run[1] = body.u32();
                    body.take(4);  // the description index: the first description serves all
                }
            } else if (type == type_of("stts")) {
                if (twice(&tables.has_times)) return;
                body.take(4);
                const uint32_t count = body.u32();
                if (!fits(body, count, 8)) return fail_void(kMalformed);
                tables.time_runs.resize(count);
                for (auto& run : tables.time_runs) {
                    run[0] = body.u32();
                    run[1] = body.u32();
                }
            } else if (type == type_of("ctts")) {
                if (twice(&tables.has_offsets)) return;
                body.take(4);  // version and flags: either version's offsets as signed (below)
                const uint32_t count = body.u32();
                if (!fits(body, count, 8)) return fail_void(kMalformed);
                tables.offset_runs.resize(count);
                for (auto& run : tables.offset_runs) {
                    run[0] = body.u32();
                    const uint32_t offset = body.u32();
                    // Version 0's offsets are unsigned by the standard and taken as signed by Android and writers.
                    run[1] = static_cast<int32_t>(offset);
                }
            } else if (type == type_of("stss")) {
                if (twice(&tables.has_syncs)) return;
                body.take(4);
                const uint32_t count = body.u32();
                if (!fits(body, count, 4)) return fail_void(kMalformed);
                tables.syncs.resize(count);
                for (uint32_t& sync : tables.syncs) sync = body.u32();
            }
            if (body.bad) fail(kMalformed);
        }
        if (stbl.bad) fail(kMalformed);
    }

    // The tables made into samples, as far as all of them describe samples.
    void combine(Track& track, const Tables& tables) {
        if (!tables.has_sizes || !tables.has_chunks || !tables.has_chunk_runs || !tables.has_times) {
            skip = true;  // as Android's verifyTrack: a track without its sample tables is left out
            return;
        }
        size_t count = tables.size_count;
        if (tables.sizes.empty() && tables.constant_size != 0) {
            count = static_cast<size_t>(std::min<uint64_t>(count, source_length / tables.constant_size));
        }
        uint64_t timed = 0;
        for (const auto& run : tables.time_runs) timed += run[0];
        count = static_cast<size_t>(std::min<uint64_t>(count, timed));
        // The tracks share one budget, counted before the samples are made.
        if (count > samples_left) return fail_void(kMalformed);
        samples_left -= count;
        track.samples.reserve(count);
        // Chunks: run r covers chunks chunk_runs[r][0] to the next run's first, each of chunk_runs[r][1] samples.
        size_t sample = 0;
        for (size_t run = 0; run < tables.chunk_runs.size() && sample < count; run++) {
            const uint64_t first = tables.chunk_runs[run][0];
            const uint64_t end = run + 1 < tables.chunk_runs.size() ? tables.chunk_runs[run + 1][0]
                                                                    : tables.chunks.size() + 1;
            const uint32_t per_chunk = tables.chunk_runs[run][1];
            if (first == 0 || end < first || end > tables.chunks.size() + 1) break;
            for (uint64_t chunk = first; chunk < end && sample < count; chunk++) {
                uint64_t offset = tables.chunks[chunk - 1];
                for (uint32_t in = 0; in < per_chunk && sample < count; in++, sample++) {
                    const uint32_t size = tables.sizes.empty() ? tables.constant_size : tables.sizes[sample];
                    track.samples.push_back(Sample{offset, 0, size, 0});
                    if (offset > std::numeric_limits<uint64_t>::max() - size) return fail_void(kMalformed);
                    offset += size;
                    track.largest_sample = std::max(track.largest_sample, size);
                }
            }
        }
        // Times: stts's deltas, then ctts's offsets for the samples it covers.
        int64_t decode = 0;
        size_t index = 0;
        for (const auto& run : tables.time_runs) {
            for (uint32_t in = 0; in < run[0] && index < track.samples.size(); in++, index++) {
                track.samples[index].decode = decode;
                decode += run[1];
            }
        }
        index = 0;
        for (const auto& run : tables.offset_runs) {
            for (uint32_t in = 0; in < run[0] && index < track.samples.size(); in++, index++) {
                track.samples[index].composition_offset = static_cast<int32_t>(run[1]);
            }
        }
        track.sync.assign(track.samples.size(), !tables.has_syncs);
        for (const uint32_t sync : tables.syncs) {
            if (sync >= 1 && sync <= track.samples.size()) track.sync[sync - 1] = true;
        }
    }

    // As Android's elst and getTrack: an empty first edit, then the media time of the first edit that shows media.
    void read_elst(Span body, Track& track, uint32_t movie_timescale) {
        const uint8_t version = body.u8();
        body.take(3);
        const uint32_t count = body.u32();
        if (count > 2) return;  // Android ignores an edit list of more entries
        uint64_t empty = 0;
        bool has_empty = false;
        for (uint32_t index = 0; index < count && !body.bad; index++) {
            const uint64_t duration = version == 1 ? body.u64() : body.u32();
            const int64_t media_time = version == 1 ? static_cast<int64_t>(body.u64())
                                                    : static_cast<int32_t>(body.u32());
            body.take(4);  // rate
            if (media_time == -1 && index == 0) {
                empty = duration;  // in the movie's timescale
                has_empty = true;
            } else if (media_time >= 0 && (index == 0 || has_empty)) {
                track.edit_media_time = media_time;
            }
        }
        if (body.bad) return fail_void(kMalformed);
        if (!has_empty || movie_timescale == 0) return;
        // Into the track's timescale, rounded; a conversion that overflows leaves the track out (getTrack fails).
        uint64_t scaled = 0;
        if (__builtin_mul_overflow(empty, static_cast<uint64_t>(track.timescale), &scaled) ||
            __builtin_add_overflow(scaled, static_cast<uint64_t>(movie_timescale / 2), &scaled) ||
            scaled / movie_timescale > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            skip = true;
            return;
        }
        track.edit_empty = static_cast<int64_t>(scaled / movie_timescale);
    }

    void read_trak(Span trak, uint32_t movie_timescale) {
        Track track;
        Tables tables;
        skip = false;
        Span edits;
        bool has_edits = false;
        uint32_t type = 0;
        Span body;
        while (error == 0 && next_box(trak, &type, &body)) {
            if (type == type_of("tkhd")) {
                const uint8_t version = body.u8();
                body.take(3 + (version == 1 ? 16 : 8));
                track.id = body.u32();
            } else if (type == type_of("edts")) {
                uint32_t inner = 0;
                Span list;
                while (next_box(body, &inner, &list)) {
                    if (inner == type_of("elst")) {
                        edits = list;
                        has_edits = true;
                    }
                }
            } else if (type == type_of("mdia")) {
                read_mdia(body, track, tables);
            }
            if (body.bad) fail(kMalformed);
        }
        if (trak.bad) fail(kMalformed);
        if (error != 0 || track.mime.empty()) return;
        if (track.timescale == 0) return;  // none, or no mdhd: as Android's verifyTrack, the track is left out
        if (tracks->size() >= kMaxTracks) return fail_void(kMalformed);
        if (has_edits) read_elst(edits, track, movie_timescale);
        if (!skip && error == 0) combine(track, tables);
        if (skip || error != 0) return;
        tracks->push_back(std::move(track));
    }

    void read_mdia(Span mdia, Track& track, Tables& tables) {
        uint32_t type = 0;
        Span body;
        while (error == 0 && next_box(mdia, &type, &body)) {
            if (type == type_of("mdhd")) {
                const uint8_t version = body.u8();
                body.take(3 + (version == 1 ? 16 : 8));
                track.timescale = body.u32();
                track.duration = version == 1 ? body.u64() : body.u32();
                const uint16_t code = body.u16();
                if (code == 0 || code == 0x7fff) {
                    track.language = "und";
                } else {
                    track.language = {static_cast<char>(((code >> 10) & 0x1f) + 0x60),
                                      static_cast<char>(((code >> 5) & 0x1f) + 0x60),
                                      static_cast<char>((code & 0x1f) + 0x60)};
                }
            } else if (type == type_of("minf")) {
                uint32_t inner = 0;
                Span part;
                while (error == 0 && next_box(body, &inner, &part)) {
                    if (inner == type_of("stbl")) read_stbl(part, track, tables);
                }
            }
            if (body.bad) fail(kMalformed);
        }
        if (mdia.bad) fail(kMalformed);
    }

    // Nested boxes are counted so that a file of boxes in boxes ends at kMaxDepth.
    bool too_deep(Span body, int depth) {
        if (depth > kMaxDepth) return true;
        uint32_t type = 0;
        Span inner;
        while (next_box(body, &type, &inner)) {
            if (inner.size >= 8 && (type == type_of("udta") || type == type_of("meta") || type == type_of("trak") ||
                                    type == type_of("mdia") || type == type_of("minf") || type == type_of("stbl") ||
                                    type == type_of("edts") || type == type_of("dinf"))) {
                if (too_deep(inner, depth + 1)) return true;
            }
        }
        return false;
    }

    void read_moov(Span moov) {
        if (too_deep(moov, 1)) return fail_void(kMalformed);
        uint32_t movie_timescale = 0;
        uint32_t type = 0;
        Span body;
        while (error == 0 && next_box(moov, &type, &body)) {
            if (type == type_of("mvhd")) {
                const uint8_t version = body.u8();
                body.take(3 + (version == 1 ? 16 : 8));
                movie_timescale = body.u32();
            } else if (type == type_of("mvex")) {
                return fail_void(kUnsupported);  // fragmented (e2d2)
            } else if (type == type_of("trak")) {
                read_trak(body, movie_timescale);
            }
            if (body.bad) fail(kMalformed);
        }
        if (moov.bad) fail(kMalformed);
    }
};

uint32_t be32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 |
           static_cast<uint32_t>(bytes[2]) << 8 | static_cast<uint32_t>(bytes[3]);
}

// Reads exactly `size` bytes at `offset`, or false.
bool read_exactly(const Source& source, uint64_t offset, void* data, size_t size) {
    if (offset > source.length || size > source.length - offset) return false;
    auto* bytes = static_cast<uint8_t*>(data);
    while (size > 0) {
        const ssize_t got = source.read(source.context, offset, bytes, size);
        if (got <= 0) return false;
        bytes += got;
        offset += static_cast<uint64_t>(got);
        size -= static_cast<size_t>(got);
    }
    return true;
}

// Whether an ftyp's brands say that the file has a movie, as MPEG4Extractor's ftyp case decides it: the major and the
// compatible brands (not the minor version), none of them "qt  ", and one left after HEIF's pair or AVIF's brands are
// taken out. Each brand is read, as Android reads them, in pieces of at most 1 KiB.
int read_brands(const Source& source, uint64_t body, uint64_t size, bool* branded) {
    if (size < 8) return kMalformed;
    bool qt = false, mif1 = false, heic = false, avif = false, avis = false, other = false;
    const uint64_t count = (size - 8) / 4 + 2;
    uint8_t piece[1024];
    for (uint64_t index = 0; index < count;) {
        const uint64_t many = std::min<uint64_t>(count - index, sizeof piece / 4);
        if (!read_exactly(source, body + index * 4, piece, static_cast<size_t>(many * 4))) return kMalformed;
        for (uint64_t at = 0; at < many; at++, index++) {
            if (index == 1) continue;  // the minor version
            const uint32_t brand = be32(piece + at * 4);
            qt |= brand == type_of("qt  ");
            mif1 |= brand == type_of("mif1");
            heic |= brand == type_of("heic");
            avif |= brand == type_of("avif");
            avis |= brand == type_of("avis");
            other |= brand != type_of("mif1") && brand != type_of("heic") && brand != type_of("avif") &&
                     brand != type_of("avis");
        }
    }
    if (qt) return 0;
    // HEIF's mif1 and heic go when both are there, else AVIF's avif and avis.
    *branded |= mif1 && heic ? other || avif || avis : other || mif1 || heic;
    return 0;
}

}  // namespace

int parse(const Source& source, std::vector<Track>* tracks) {
    tracks->clear();
    std::vector<uint8_t> movie;
    uint64_t offset = 0;
    bool found = false, branded = false, data = false;
    // One box of the top level, on the source: ftyp, mdat, moov, free and others, each skipped by its size but the
    // moov and the ftyp's brands. Any box but the mdat holds at most kMaxMovieBytes, as Android's kMaxAtomSize.
    auto next = [&]() -> int {
        uint8_t header[16];
        if (source.length - offset < 8 || !read_exactly(source, offset, header, 8)) return kMalformed;
        uint64_t size = be32(header);
        const uint32_t type = be32(header + 4);
        uint64_t length = 8;
        if (size == 1) {
            if (source.length - offset < 16 || !read_exactly(source, offset + 8, header + 8, 8)) return kMalformed;
            size = 0;
            for (int index = 8; index < 16; index++) size = size << 8 | header[index];
            length = 16;
        } else if (size == 0) {
            size = source.length - offset;
        }
        if (size < length || size > source.length - offset) return kMalformed;
        if (type == type_of("moof") || type == type_of("mfra")) return kUnsupported;  // fragmented (e2d2)
        if (type != type_of("mdat") && size - length > kMaxMovieBytes) return kMalformed;
        if (type == type_of("ftyp")) {
            const int answer = read_brands(source, offset + length, size - length, &branded);
            if (answer != 0) return answer;
        }
        data |= type == type_of("mdat");
        if (type == type_of("moov") && !found) {
            try {
                movie.resize(static_cast<size_t>(size - length));
            } catch (const std::bad_alloc&) {
                return kNoMemory;
            }
            if (!read_exactly(source, offset + length, movie.data(), movie.size())) return kIo;
            found = true;
        }
        offset += size;
        return 0;
    };
    // As MPEG4Extractor::readMetaData: the walk ends once the brands said there is a movie and both the moov and an
    // mdat were seen; an error ends the file before the moov and only the walk after it (mInitCheck is OK by then),
    // but a fragment, which this step does not read.
    while (offset < source.length && !(branded && found && data)) {
        const int answer = next();
        if (answer == kUnsupported || (answer != 0 && !found)) return answer;
        if (answer != 0) break;
    }
    if (!found) return kMalformed;
    try {
        Parser parser{tracks, source.length};
        parser.read_moov(Span{movie.data(), movie.size(), 0, false});
        if (parser.error != 0) {
            tracks->clear();
            return parser.error;
        }
    } catch (const std::bad_alloc&) {
        tracks->clear();
        return kNoMemory;
    }
    return 0;
}

long output_size(const Track& track, size_t index) {
    if (index >= track.samples.size()) return kMalformed;
    // As MPEG4Source::start: a track whose largest sample plus 20 bytes is above the buffer it allocates is refused.
    if (track.largest_sample > kMaxInputBytes - 20) return kMalformed;
    const uint32_t size = track.samples[index].size;
    // A NAL length of fewer than 4 bytes becomes a 4-byte start code: at most 3 more bytes a unit of one byte or more.
    if (track.nal_length_size == 0 || track.nal_length_size == 4) return static_cast<long>(size);
    const uint64_t units = size / (track.nal_length_size + 1) + 1;
    return static_cast<long>(size + units * (4 - track.nal_length_size));
}

long read_sample(const Source& source, const Track& track, size_t index, uint8_t* out, size_t capacity) {
    const long longest = output_size(track, index);
    if (longest < 0) return longest;
    if (capacity < static_cast<size_t>(longest)) return kNoMemory;
    const Sample& sample = track.samples[index];
    if (sample.offset > source.length || sample.size > source.length - sample.offset) return kMalformed;
    if (track.nal_length_size == 0 || track.nal_length_size == 4) {
        if (!read_exactly(source, sample.offset, out, sample.size)) return kIo;
        if (track.nal_length_size == 0) return static_cast<long>(sample.size);
        // In place: each 4-byte length becomes a start code, and the lengths must fill the sample exactly.
        size_t at = 0;
        while (at < sample.size) {
            if (sample.size - at < 4) return kMalformed;
            const uint32_t length = be32(out + at);
            if (length > sample.size - at - 4) return kMalformed;
            out[at] = out[at + 1] = out[at + 2] = 0;
            out[at + 3] = 1;
            at += 4 + length;
        }
        return static_cast<long>(sample.size);
    }
    // Lengths of 1 or 2 bytes: read into the end of the buffer, then copy each unit forward after a start code.
    const size_t from = capacity - sample.size;
    if (!read_exactly(source, sample.offset, out + from, sample.size)) return kIo;
    size_t at = from, written = 0;
    const size_t end = capacity;
    while (at < end) {
        if (end - at < track.nal_length_size) return kMalformed;
        uint32_t length = 0;
        for (uint32_t byte = 0; byte < track.nal_length_size; byte++) length = length << 8 | out[at + byte];
        at += track.nal_length_size;
        if (length > end - at || written + 4 > at) return kMalformed;  // the unit would overwrite unread bytes
        out[written] = out[written + 1] = out[written + 2] = 0;
        out[written + 3] = 1;
        std::memmove(out + written + 4, out + at, length);
        written += 4 + length;
        at += length;
    }
    return static_cast<long>(written);
}

}  // namespace minato::mp4
