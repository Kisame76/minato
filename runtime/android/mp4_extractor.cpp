// ADR-0060 e2d: libmediandk's ISO BMFF parser (mp4_extractor.h). The top level is walked on the source, box by box;
// the moov box is read whole (at most kMaxMovieBytes) and parsed in memory through Span, whose every read is checked
// against its end, so a box never reaches past the one that holds it. Sample tables stay where the moov has them
// (SampleTable); tables that disagree on the count end the track at the shortest one, as Android's SampleTable
// refuses samples past what all its tables describe. Bionic's C library alone (mp4_extractor.h): malloc'd memory,
// which Tracks frees.
#include "mp4_extractor.h"

#include <stdlib.h>
#include <string.h>

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

template <typename T>
T smaller(T a, T b) {
    return b < a ? b : a;
}

template <typename T>
T larger(T a, T b) {
    return a < b ? b : a;
}

uint32_t be32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 |
           static_cast<uint32_t>(bytes[2]) << 8 | static_cast<uint32_t>(bytes[3]);
}

// Frees what a track owns.
void release(Track& track) {
    free(track.csd0.data);
    free(track.csd1.data);
    for (Bytes& plain : track.opaque) {
        free(plain.data);
        plain = Bytes{};
    }
    free(track.samples.sorted_syncs);
    free(track.seek_index.entries);
    track.csd0 = Bytes{};
    track.csd1 = Bytes{};
    track.samples.sorted_syncs = nullptr;
    track.seek_index = SeekIndex{};
}

// The first bytes of a codec configuration (avcC, hvcC), for the profile and level.
void keep_config(Track& track, const Span& body) {
    track.config_size = static_cast<uint8_t>(smaller<size_t>(body.size, sizeof track.config));
    memcpy(track.config, body.data, track.config_size);
}

// NALBitReader's reading of a NAL unit's RBSP, ABitReader's semantics and all: an emulation prevention byte (a 3
// after two zeros) skipped, up to 4 bytes taken into the reservoir at a time, a read past the end failing after it
// took what was left (`over` set, as overRead), a read of more than 32 bits failing at once.
struct NalBits {
    const uint8_t* data;
    size_t size;
    uint32_t reservoir = 0;
    size_t left = 0;  // bits in the reservoir
    int zeros = 0;
    bool over = false;

    bool fill() {
        if (size == 0) {
            over = true;
            return false;
        }
        reservoir = 0;
        size_t count = 0;
        while (size > 0 && count < 4) {
            const bool prevention = zeros >= 2 && *data == 3;
            zeros = *data == 0 ? zeros + 1 : 0;
            if (!prevention) {
                reservoir = reservoir << 8 | *data;
                count++;
            }
            data++;
            size--;
        }
        left = 8 * count;
        reservoir = left == 0 ? 0 : reservoir << (32 - left);
        return true;
    }
    bool get(size_t count, uint32_t* out) {
        if (count > 32) return false;
        uint64_t result = 0;
        while (count > 0) {
            if (left == 0 && !fill()) return false;
            const size_t part = smaller(count, left);
            if (part == 0) continue;  // a fill of an emulation prevention byte alone (ABitReader takes 0 bits)
            result = result << part | reservoir >> (32 - part);
            reservoir = part == 32 ? 0 : reservoir << part;
            left -= part;
            count -= part;
        }
        *out = static_cast<uint32_t>(result);
        return true;
    }
    uint32_t get_or(size_t count, uint32_t fallback) {
        uint32_t value = fallback;
        get(count, &value);
        return value;
    }
    void skip(size_t count) {
        uint32_t ignored = 0;
        for (; count > 32; count -= 32) {
            if (!get(32, &ignored)) return;
        }
        if (count > 0) get(count, &ignored);
    }
    // parseUEWithFallback, whose skipUE and skipSE read the same bits.
    uint32_t ue(uint32_t fallback) {
        uint32_t zero_bits = 0;
        while (get_or(1, 1) == 0) zero_bits++;
        if (zero_bits >= 32) {
            skip(zero_bits);
            return fallback;
        }
        uint32_t value = 0;
        return get(zero_bits, &value) ? value + (1u << zero_bits) - 1 : fallback;
    }
};

// HevcParameterSets::parseSps (Rec. ITU-T H.265's 7.3.2.2) as far as the VUI's colour description, on an SPS after
// its NAL header: the full range flag, the primaries, the transfer and the matrix each replace the one before when
// read; a colour description and an HDR transfer stay once seen. It stops where parseSps returns ERROR_MALFORMED.
void read_vui(const uint8_t* data, size_t size, Track::Vui& vui) {
    NalBits bits{data, size};
    bits.skip(4);  // sps_video_parameter_set_id
    const uint8_t sub_layers = static_cast<uint8_t>(bits.get_or(3, 0));
    bits.skip(1);
    bits.skip(96);  // the general profile, tier and level
    if (sub_layers > 0) {
        bool profile_present[8], level_present[8];
        for (int layer = 0; layer < sub_layers; ++layer) {
            profile_present[layer] = bits.get_or(1, 0) != 0;
            level_present[layer] = bits.get_or(1, 0) != 0;
        }
        bits.skip(2 * (8 - static_cast<size_t>(sub_layers)));
        for (int layer = 0; layer < sub_layers; ++layer) {
            if (profile_present[layer]) bits.skip(88);
            if (level_present[layer]) bits.skip(8);
        }
    }
    bits.ue(0);  // sps_seq_parameter_set_id
    if (static_cast<uint8_t>(bits.ue(0)) == 3) bits.skip(1);  // chroma_format_idc as parseSps keeps it, a uint8_t
    bits.ue(0);
    bits.ue(0);  // the picture's width and height
    if (bits.get_or(1, 0) != 0) {
        for (int offset = 0; offset < 4; offset++) bits.ue(0);  // the conformance window
    }
    bits.ue(0);
    bits.ue(0);  // bit depths
    const size_t poc_lsb_bits = bits.ue(0) + size_t{4};
    const bool ordering = bits.get_or(1, 0) != 0;
    for (uint32_t layer = ordering ? 0 : sub_layers; layer <= sub_layers; ++layer) {
        bits.ue(0);
        bits.ue(0);
        bits.ue(0);
    }
    for (int field = 0; field < 6; field++) bits.ue(0);  // block sizes and transform hierarchy depths
    if (bits.get_or(1, 0) != 0 && bits.get_or(1, 0) != 0) {  // scaling lists, with their data
        for (uint32_t size_id = 0; size_id < 4; ++size_id) {
            for (uint32_t matrix = 0; matrix < 6; matrix += size_id == 3 ? 3 : 1) {
                if (bits.get_or(1, 1) == 0) {
                    bits.ue(0);
                    continue;
                }
                const uint32_t coefficients = smaller(64u, 1u << (4 + (size_id << 1)));
                if (size_id > 1) bits.ue(0);
                for (uint32_t index = 0; index < coefficients; ++index) bits.ue(0);
            }
        }
    }
    bits.skip(1);
    bits.skip(1);  // amp_enabled_flag, sample_adaptive_offset_enabled_flag
    if (bits.get_or(1, 0) != 0) {  // PCM
        bits.skip(4);
        bits.skip(4);
        bits.ue(0);
        bits.ue(0);
        bits.skip(1);
    }
    const uint32_t sets = bits.ue(0);
    uint32_t pictures = 0;
    for (uint32_t set = 0; set < sets; ++set) {
        if (set != 0 && bits.get_or(1, 0) != 0) {  // predicted from the set before
            bits.skip(1);
            bits.ue(0);
            uint32_t next = 0;
            for (uint32_t picture = 0; picture <= pictures; ++picture) {
                if (bits.get_or(1, 0) != 0 || bits.get_or(1, 0) != 0) ++next;
            }
            pictures = next;
        } else {
            const uint32_t negative = bits.ue(0), positive = bits.ue(0);
            if (negative > UINT32_MAX - positive) return;
            pictures = negative + positive;
            for (uint32_t picture = 0; picture < pictures; ++picture) {
                bits.ue(0);
                bits.skip(1);
                if (bits.over) return;
            }
        }
        if (bits.over) return;
    }
    if (bits.get_or(1, 0) != 0) {  // long-term reference pictures
        const uint32_t pictures_lt = bits.ue(0);
        for (uint32_t picture = 0; picture < pictures_lt; ++picture) {
            bits.skip(poc_lsb_bits);
            bits.skip(1);
            if (bits.over) return;
        }
    }
    bits.skip(1);
    bits.skip(1);  // sps_temporal_mvp_enabled_flag, strong_intra_smoothing_enabled_flag
    if (bits.get_or(1, 0) == 0) return;  // no VUI
    if (bits.get_or(1, 0) != 0 && bits.get_or(8, 0) == 0xff) {  // an extended sample aspect ratio
        bits.skip(16);
        bits.skip(16);
    }
    if (bits.get_or(1, 0) != 0) bits.skip(1);  // overscan
    if (bits.get_or(1, 0) == 0) return;        // no video signal type
    bits.skip(3);                              // video_format
    uint32_t value = 0;
    if (bits.get(1, &value)) {
        vui.full_range = value;
        vui.has_full_range = true;
    }
    if (bits.get_or(1, 0) == 0) return;  // no colour description
    vui.described = true;
    if (bits.get(8, &value)) {
        vui.primaries = value;
        vui.has_primaries = true;
    }
    if (bits.get(8, &value)) {
        vui.transfer = value;
        vui.has_transfer = true;
        if (value == 16 || value == 18) vui.hdr = true;  // ST 2084, ARIB STD-B67 (HLG)
    }
    if (bits.get(8, &value)) {
        vui.matrix = value;
        vui.has_matrix = true;
    }
}

struct Parser {
    Tracks* tracks;
    uint64_t source_length;  // the source's: a track's samples of one size never number more than it holds
    const uint8_t* movie;    // the moov's body, which the tracks' tables point into
    int error = 0;
    bool skip = false;  // the track being read is left out, as Android's verifyTrack leaves it out

    int fail(int answer) {
        if (error == 0) error = answer;
        return error;
    }

    // An esds as ESDS reads it: its DecoderConfigDescriptor's object type and bit rates, and the DecoderSpecificInfo.
    struct Esds {
        uint8_t object = 0;
        uint32_t max_bitrate = 0, avg_bitrate = 0;
        Span info;  // empty without one
        bool too_large = false;  // an info past kMaxCodecData, Minato's bound: the file fails
    };

    // Which sample tables one track has, read before they are combined; `size_count` is stsz's count.
    struct Tables {
        uint32_t size_count = 0;
        bool has_sizes = false, has_chunks = false, has_chunk_runs = false, has_times = false, has_offsets = false,
             has_syncs = false;
        bool described = false;  // an stsd named the track's codec
        // The last avcC, hvcC and esds in the track's sample entries, as MPEG4Extractor keeps them in the track's
        // csd-avc, csd-hevc and esds, whichever entry they are in.
        Span avcc, hvcc, esds;
        bool has_avcc = false, has_hvcc = false, has_esds = false;
    };

    // A count of entries of `entry` bytes each that the body must hold, or false (malformed).
    static bool fits(const Span& body, uint64_t count, size_t entry) { return count <= body.left() / entry; }

    // The body's next `count` entries in the movie.
    SampleTable::Entries entries(const Span& body, uint32_t count) const {
        return SampleTable::Entries{static_cast<size_t>(body.data + body.at - movie), count};
    }

    // A csd of parameter sets, each after a start code, made in one allocation: a configuration's walk runs twice,
    // once to sum the sets' sizes (at most kMaxCodecData, else the file is malformed) and once to copy them.
    struct Sets {
        Bytes& csd;
        size_t size = 0;
        bool copying = false;
        bool too_large = false;  // past kMaxCodecData, which fails the file (a walk that ends early refuses the format)

        bool put(const Span& set) {
            static const uint8_t start[] = {0, 0, 0, 1};
            if (!copying) {
                if (set.size + 4 > kMaxCodecData - size) {
                    too_large = true;
                    return false;
                }
                size += 4 + set.size;
                return true;
            }
            memcpy(csd.data + csd.size, start, 4);
            memcpy(csd.data + csd.size + 4, set.data, set.size);
            csd.size += 4 + set.size;
            return true;
        }
        bool allocate() {
            copying = true;
            if (size == 0) return true;
            csd.data = static_cast<uint8_t*>(malloc(size));
            return csd.data != nullptr;
        }
    };

    // An avcC's SPS into `sps` and PPS into `pps`; false for one that ends inside a set or has too many bytes.
    static bool walk_avcc(Span body, Sets& sps, Sets& pps) {
        body.take(5);  // version, profile, compatibility, level, NAL length size
        const uint8_t sps_count = body.u8() & 0x1f;
        for (uint8_t index = 0; index < sps_count && !body.bad; index++) {
            const uint16_t length = body.u16();
            Span set = body.sub(length);
            if (!set.bad && !sps.put(set)) return false;
        }
        const uint8_t pps_count = body.u8();
        for (uint8_t index = 0; index < pps_count && !body.bad; index++) {
            const uint16_t length = body.u16();
            Span set = body.sub(length);
            if (!set.bad && !pps.put(set)) return false;
        }
        return !body.bad;
    }

    // An hvcC's VPS, SPS and PPS into `sets`; false as walk_avcc.
    static bool walk_hvcc(Span body, Sets& sets) {
        body.take(22);  // to the NAL length size, which MPEG4Source reads
        const uint8_t arrays = body.u8();
        for (uint8_t array = 0; array < arrays && !body.bad; array++) {
            body.u8();  // completeness and the NAL unit type
            const uint16_t count = body.u16();
            for (uint16_t index = 0; index < count && !body.bad; index++) {
                const uint16_t length = body.u16();
                Span set = body.sub(length);
                if (!set.bad && !sets.put(set)) return false;
            }
        }
        return !body.bad;
    }

    // Every SPS of an avcC into csd0 and every PPS into csd1, as convertMetaDataToMessage copies them (none for
    // none); false for sets that pass the avcC's end, which the conversion refuses.
    bool split_avcc(const Span& body, Track& track) {
        Sets sps{track.csd0}, pps{track.csd1};
        if (!walk_avcc(body, sps, pps)) {
            if (sps.too_large || pps.too_large) fail(kMalformed);
            return false;
        }
        if (!sps.allocate() || !pps.allocate()) fail(kNoMemory);
        if (error == 0) walk_avcc(body, sps, pps);
        return true;
    }

    // An hvcC's VPS, SPS and PPS into csd0 (none for no arrays, as hev1's sets may be in band); false as
    // split_avcc.
    bool split_hvcc(const Span& body, Track& track) {
        Sets sets{track.csd0};
        if (!walk_hvcc(body, sets)) {
            if (sets.too_large) fail(kMalformed);
            return false;
        }
        if (!sets.allocate()) fail(kNoMemory);
        if (error == 0) walk_hvcc(body, sets);
        return true;
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

    // Utils' loop over an hvcC's units for convertMetaDataToMessage's HevcParameterSets: each SPS (type 33) of 2
    // bytes or more read after its NAL header (the other units' results never reach the format).
    static void read_vuis(Span body, Track::Vui& vui) {
        body.take(22);
        const uint8_t arrays = body.u8();
        for (uint8_t array = 0; array < arrays && !body.bad; array++) {
            body.u8();
            const uint16_t count = body.u16();
            for (uint16_t index = 0; index < count && !body.bad; index++) {
                const uint16_t length = body.u16();
                const Span unit = body.sub(length);
                if (!unit.bad && unit.size >= 2 && (unit.data[0] >> 1 & 0x3f) == 33) {
                    read_vui(unit.data + 2, unit.size - 2, vui);
                }
            }
        }
    }

    // parseColorInfo: a colr under 4 bytes fails the file; the first nclx of 11 bytes or more or nclc of 10 or more
    // stays the track's (its colour range, which every one sets, is there after it).
    void read_colr(Span body, Track& track) {
        if (body.size < 4) return fail_void(kMalformed);
        const uint32_t kind = body.u32();
        const bool nclx = kind == type_of("nclx") && body.size >= 11;
        if ((!nclx && !(kind == type_of("nclc") && body.size >= 10)) || track.has_colr) return;
        track.has_colr = true;
        track.colr_primaries = body.u16();
        track.colr_transfer = body.u16();
        track.colr_matrix = body.u16();
        track.colr_full_range = nclx && (body.u8() & 0x80) != 0;
    }

    // parsePaspBox: a pasp under 8 bytes fails the file; one of both spacings above 0 gives sar-width and height.
    void read_pasp(Span body, Track& track) {
        if (body.size < 8) return fail_void(kMalformed);
        const uint32_t horizontal = body.u32(), vertical = body.u32();
        if (horizontal == 0 || vertical == 0) return;
        track.has_sar = true;
        track.sar_width = horizontal;
        track.sar_height = vertical;
    }

    // esds: version and flags 0, as MPEG4Extractor's esds asks, then ES_Descriptor (3) > DecoderConfigDescriptor (4) >
    // DecoderSpecificInfo (5), the only descriptor after the config's 13 bytes; false for one ESDS's InitCheck
    // refuses and for an info past Minato's bound (too_large).
    static bool parse_esds(Span body, Esds& esds) {
        if (body.u32() != 0 || body.u8() != 3) return false;
        Span es = body.sub(descriptor_length(body));
        es.take(2);  // ES_ID
        const uint8_t es_flags = es.u8();
        if (es_flags & 0x80) es.take(2);              // dependsOn_ES_ID
        if (es_flags & 0x40) es.take(es.u8());        // URL
        if (es_flags & 0x20) {
            es.take(2);  // OCR_ES_Id
            // As ESDS: one "in the wild" has the flag but not the id, the DecoderConfigDescriptor right after.
            if (!es.bad && (es.left() == 0 || es.data[es.at] != 4) && es.data[es.at - 2] == 4) es.at -= 2;
        }
        if (es.u8() != 4) return false;
        Span config = es.sub(descriptor_length(es));
        esds.object = config.u8();
        config.take(4);  // stream type, buffer size
        esds.max_bitrate = config.u32();
        esds.avg_bitrate = config.u32();
        if (config.left() > 0) {
            if (config.u8() != 5) return false;
            esds.info = config.sub(descriptor_length(config));
            esds.too_large = !esds.info.bad && esds.info.size > kMaxCodecData;
            if (esds.info.bad || esds.too_large) return false;
        }
        return !body.bad && !es.bad && !config.bad;
    }

    // An esds of version and flags 0 in any sample entry stays as the track's, as MPEG4Extractor's esds keeps it; in
    // an mp4a, its config is the track's, as updateAudioTrackInfoFromESDS_MPEG4Audio reads it (a track of another
    // object than AAC's is left out).
    void read_esds(Span body, Track& track, Tables& tables, bool in_mp4a) {
        Span head = body;
        if (head.u32() != 0 || head.bad) return fail_void(kMalformed);
        tables.esds = body;
        tables.has_esds = true;
        if (!in_mp4a) return;
        Esds esds;
        if (!parse_esds(body, esds)) return fail_void(kMalformed);
        if (esds.object == 0xe1 || esds.object == 0x6b || esds.object == 0x69) {
            track.mime = nullptr;  // QCELP or MP3, before any bit rate: a track this parser leaves out
            return;
        }
        // updateAudioTrackInfoFromESDS_MPEG4Audio's: each only above 0, so a later esds of 0 keeps them.
        if (esds.max_bitrate > 0 && esds.max_bitrate < INT32_MAX) track.max_bitrate = esds.max_bitrate;
        if (esds.avg_bitrate > 0 && esds.avg_bitrate < INT32_MAX) track.avg_bitrate = esds.avg_bitrate;
        if (esds.object != 0x40 && esds.object != 0x66 && esds.object != 0x67 && esds.object != 0x68) {
            track.mime = nullptr;  // not AAC: a track this parser leaves out
            return;
        }
        const int answer = read_audio_config(track, esds.info);
        if (answer != 0) fail(answer);
    }

    void fail_void(int answer) { fail(answer); }

    // Bits of the AudioSpecificConfig, as ABitReader reads them (the callers check what is left first).
    struct Bits {
        const uint8_t* data;
        size_t size, at;  // in bits
        size_t left() const { return size - at; }
        uint32_t get(size_t count) {
            uint32_t value = 0;
            for (; count > 0 && at < size; count--, at++) value = value << 1 | ((data[at / 8] >> (7 - at % 8)) & 1u);
            return value;
        }
    };

    // MPEG4Extractor's updateAudioTrackInfoFromESDS_MPEG4Audio on the AudioSpecificConfig: its audio object type, and
    // the sampling rate and channel count in place of the sample entry's; 0, kMalformed or kUnsupported, which fail the
    // file.
    static int read_audio_config(Track& track, const Span& config) {
        static const int32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000,  7350};
        if (config.size == 0) return 0;  // the sample entry's values stay
        if (config.size < 2) return kMalformed;
        Bits bits{config.data, config.size * 8, 0};
        uint32_t object = bits.get(5);
        if (object == 31) object = 32 + bits.get(6);  // the escape
        track.audio_object = object;
        track.has_audio_object = true;
        const uint32_t frequency = bits.get(4);
        int32_t rate = 0, channels = 0;
        if (frequency == 15) {
            if (bits.left() < 28) return kMalformed;
            rate = static_cast<int32_t>(bits.get(24));
            channels = static_cast<int32_t>(bits.get(4));
        } else {
            if (bits.left() < 4) return kMalformed;
            channels = static_cast<int32_t>(bits.get(4));
            if (frequency == 13 || frequency == 14) return kMalformed;
            rate = rates[frequency];
        }
        const bool sbr = object == 5 || object == 29;  // SBR, PS
        if (sbr) {
            if (bits.left() < 4) return kMalformed;
            const uint32_t extension = bits.get(4);
            if (extension == 15) {
                if (config.size < 8 || bits.left() < 24) return kMalformed;
                bits.get(24);
            } else if (extension == 13 || extension == 14) {
                return kMalformed;
            }
        }
        // 14496-3's channel configurations 7, 11, 12 and 14 by their channels; others above 6 are not supported.
        if (channels == 11) {
            channels = 7;
        } else if (channels == 7 || channels == 12 || channels == 14) {
            channels = 8;
        } else if (channels > 6) {
            return kUnsupported;
        }
        if (sbr) {
            if (bits.left() < 5) return kMalformed;
            object = bits.get(5);
            if (object == 31) {
                if (bits.left() < 6) return kMalformed;
                object = 32 + bits.get(6);
            }
        }
        if (object == 2 || object == 17 || object == 23 || object == 20 || object == 22) {  // the AAC LC family
            if (bits.left() < 2) return kMalformed;
            bits.get(1);  // frameLengthFlag
            if (bits.get(1) != 0) {  // dependsOnCoreCoder
                if (bits.left() < 14) return kMalformed;
                bits.get(14);
            }
            if (bits.left() > 0) bits.get(1);  // extensionFlag, else taken by the object type
            if (channels == 0) {
                // The program config element's channels.
                if (bits.left() < 32) return kMalformed;
                bits.get(10);  // element instance tag, profile, sampling frequency index
                const uint32_t front = bits.get(4), side = bits.get(4), back = bits.get(4), lfe = bits.get(2);
                bits.get(7);  // associated data and valid coupling channel elements
                if (bits.get(1) != 0) {  // mono mixdown
                    if (bits.left() < 4) return kMalformed;
                    bits.get(4);
                }
                if (bits.left() < 1) return kMalformed;
                if (bits.get(1) != 0) {  // stereo mixdown
                    if (bits.left() < 4) return kMalformed;
                    bits.get(4);
                }
                if (bits.left() < 1) return kMalformed;
                if (bits.get(1) != 0) {  // matrix mixdown
                    if (bits.left() < 3) return kMalformed;
                    bits.get(3);
                }
                for (uint32_t element = 0; element < front + side + back; element++) {
                    if (bits.left() < 5) return kMalformed;
                    channels += bits.get(1) != 0 ? 2 : 1;  // a channel pair, else one channel
                    bits.get(4);
                }
                for (uint32_t element = 0; element < lfe; element++) {
                    if (bits.left() < 4) return kMalformed;
                    bits.get(4);
                    channels++;
                }
            }
        }
        if (channels == 0) return kUnsupported;
        track.sample_rate = rate;
        track.channels = channels;
        return 0;
    }

    // As MPEG4Extractor's stsd: version and flags 0; more than one entry in a track an earlier stsd described leaves it
    // out ("only a single type of media per track"); else each entry in turn, the codec of each in place of the one
    // before, and the entries fill the box.
    void read_stsd(Span body, Track& track, Tables& tables) {
        if (body.u32() != 0) return fail_void(kMalformed);
        const uint32_t entries = body.u32();
        if (body.bad) return fail_void(kMalformed);
        if (entries > 1 && tables.described) {
            skip = true;
            return;
        }
        for (uint32_t index = 0; index < entries && error == 0; index++) {
            uint32_t type = 0;
            Span entry;
            if (!next_box(body, &type, &entry)) return fail_void(kMalformed);
            read_sample_entry(type, entry, track, tables);
        }
        if (error == 0 && body.left() != 0) fail(kMalformed);
    }

    // MPEG4Extractor's plain csd `index` (csd-0, csd-1 or csd-2) set to these bytes in place of any before; one past
    // kMaxCodecData fails the file, Minato's bound.
    void set_opaque(Track& track, int index, const uint8_t* data, size_t size) {
        if (size > kMaxCodecData) return fail_void(kMalformed);
        uint8_t* copy = static_cast<uint8_t*>(malloc(size > 0 ? size : 1));
        if (copy == nullptr) return fail_void(kNoMemory);
        if (size > 0) memcpy(copy, data, size);
        free(track.opaque[index].data);
        track.opaque[index] = Bytes{copy, size};
        track.has_opaque[index] = true;
    }

    // An av1C's or vpcC's body, a dfLa's or an mhaC's config as the plain csd-0, as MPEG4Extractor's parseChunk keeps
    // them: a dfLa's of 42 bytes, "fLaC" and its body after the version and flags (another size fails the file); an
    // mhaC's after its 5 bytes, which give its length (an mhaC of another size fails the file), and its profile and
    // level and its channel layout. False for another box.
    bool read_plain_csd(uint32_t type, Span body, Track& track) {
        if (type == type_of("av1C") || type == type_of("vpcC")) {
            set_opaque(track, 0, body.data, body.size);
        } else if (type == type_of("dfLa")) {
            if (body.size != 42) {
                fail(kMalformed);
                return true;
            }
            uint8_t flac[42] = {'f', 'L', 'a', 'C'};
            memcpy(flac + 4, body.data + 4, 38);
            set_opaque(track, 0, flac, sizeof flac);
        } else if (type == type_of("mhaC")) {
            if (body.size < 5 || body.size != 5u + (static_cast<size_t>(body.data[3]) << 8 | body.data[4])) {
                fail(kMalformed);
                return true;
            }
            set_opaque(track, 0, body.data + 5, body.size - 5);
            track.mpegh_profile = body.data[1];
            track.mpegh_layout = body.data[2];
        } else {
            return false;
        }
        return true;
    }

    // An Opus entry's dOps as MPEG4Extractor reads it, the entry's bytes after its first 28: 19 to 276 of them
    // (OpusHeader.h's AOPUS_OPUSHEAD_MINSIZE and MAXSIZE) of version 0, else the file fails. csd-0 is them as an
    // OpusHead (its magic, version 1, the pre-skip, rate and gain little-endian), csd-1 the codec delay and csd-2 a
    // seek pre-roll of 80 ms, each in ns as a little-endian int64_t.
    void read_opus(Span rest, Track& track) {
        const size_t size = rest.left();
        if (size < 19 || size > 276) return fail_void(kMalformed);
        uint8_t head[276];
        memcpy(head, rest.data + rest.at, size);
        if (head[8] != 0) return fail_void(kMalformed);
        memcpy(head, "OpusHead", 8);
        head[8] = 1;
        const uint32_t pre_skip = static_cast<uint32_t>(head[10] << 8 | head[11]), rate = be32(head + 12);
        const uint8_t little[8] = {static_cast<uint8_t>(pre_skip), static_cast<uint8_t>(pre_skip >> 8),
                                   static_cast<uint8_t>(rate),     static_cast<uint8_t>(rate >> 8),
                                   static_cast<uint8_t>(rate >> 16), static_cast<uint8_t>(rate >> 24),
                                   head[17],                       head[16]};  // and the gain
        memcpy(head + 10, little, sizeof little);
        set_opaque(track, 0, head, size);
        const uint64_t times[2] = {pre_skip * 1000000000ull / 48000, 80000000};
        for (int index = 0; index < 2; index++) {
            uint8_t bytes[8];
            for (int at = 0; at < 8; at++) bytes[at] = static_cast<uint8_t>(times[index] >> (8 * at));
            set_opaque(track, index + 1, bytes, sizeof bytes);
        }
    }

    // An alac entry's cookie as parseALACSampleEntry reads it after the entry's first 28 bytes: an alac box of 36
    // bytes, version and flags 0, else the file fails; csd-0 is its 24 bytes, bits-per-sample the 6th. Bytes after it
    // fail the file too (Android's CHECK_EQ on the entry's end aborts its extractor). Read in QuickTime files as well,
    // whose cookie Android reads from the entry's wave.
    void read_alac(Span rest, Track& track) {
        if (rest.left() != 36 || be32(rest.data + rest.at) != 36 || be32(rest.data + rest.at + 4) != type_of("alac") ||
            be32(rest.data + rest.at + 8) != 0) {
            return fail_void(kMalformed);
        }
        const uint8_t* cookie = rest.data + rest.at + 12;
        set_opaque(track, 0, cookie, 24);
        track.bits_per_sample = cookie[5];
    }

    // As MPEG4Extractor's sample entries: one Android knows names the track's codec in place of the one before
    // (Minato reads AVC, HEVC and AAC and leaves a track of another out), and the avcC, hvcC and esds among its boxes
    // and in a QuickTime wave stay as the track's (Tables); one that names no codec (tx3g, whose text track's type
    // comes from its hdlr) or that Android does not know (avc3 among them) changes nothing. The boxes of ac-3, ec-3,
    // ac-4 and mett are not read, nor those of Opus and alac, whose entries Android reads to their end itself.
    void read_sample_entry(uint32_t type, Span entry, Track& track, Tables& tables) {
        if (type == type_of("encv") || type == type_of("enca")) return fail_void(kUnsupported);
        static constexpr uint32_t visual[] = {
            type_of("mp4v"), type_of("s263"), type_of("H263"), type_of("h263"), type_of("avc1"),
            type_of("hvc1"), type_of("hev1"), type_of("dvav"), type_of("dva1"), type_of("dvhe"),
            type_of("dvh1"), type_of("dav1"), type_of("av01"), type_of("vp09")};
        static constexpr uint32_t sound[] = {
            type_of("mp4a"), type_of("samr"), type_of("sawb"), type_of("Opus"), type_of("twos"), type_of("sowt"),
            type_of("alac"), type_of("fLaC"), type_of(".mp3"), type_of("ms\0U"), type_of("mha1"), type_of("mhm1"),
            type_of("dtsc"), type_of("dtse"), type_of("dtsh"), type_of("dtsl"), type_of("dtsx")};
        static constexpr uint32_t other[] = {type_of("ac-3"), type_of("ec-3"), type_of("ac-4"), type_of("mett")};
        const auto listed = [type](const uint32_t* names, size_t count) {
            for (size_t index = 0; index < count; index++) {
                if (names[index] == type) return true;
            }
            return false;
        };
        const bool is_visual = listed(visual, sizeof visual / sizeof *visual);
        const bool is_sound = listed(sound, sizeof sound / sizeof *sound);
        if (!is_visual && !is_sound && !listed(other, sizeof other / sizeof *other)) return;
        tables.described = true;
        track.mime = type == type_of("avc1")                               ? "video/avc"
                     : type == type_of("hvc1") || type == type_of("hev1") ? "video/hevc"
                     : type == type_of("mp4a")                             ? "audio/mp4a-latm"
                                                                           : nullptr;
        if (is_visual) {
            entry.take(24);
            track.width = entry.u16();
            track.height = entry.u16();
            entry.take(50);
        } else if (is_sound) {
            entry.take(8);
            const uint16_t version = entry.u16();
            entry.take(6);
            track.channels = entry.u16();
            entry.take(6);
            track.sample_rate = static_cast<int32_t>(entry.u32() >> 16);
            // Opus's dOps and alac's cookie after the 28 bytes, whatever the version.
            if (entry.bad) return fail_void(kMalformed);
            if (type == type_of("Opus")) return read_opus(entry, track);
            if (type == type_of("alac")) return read_alac(entry, track);
            if (version == 1) entry.take(16);
            if (version == 2) entry.take(36);
        } else {
            return;
        }
        if (entry.bad) return fail_void(kMalformed);
        Span part;
        for (uint32_t inner = 0; error == 0 && next_box(entry, &inner, &part);) {
            if (inner == type_of("sinf")) return fail_void(kUnsupported);
            if (inner == type_of("avcC")) {
                tables.avcc = part;
                tables.has_avcc = true;
            } else if (inner == type_of("hvcC")) {
                tables.hvcc = part;
                tables.has_hvcc = true;
            } else if (inner == type_of("esds")) {
                read_esds(part, track, tables, type == type_of("mp4a"));
            } else if (inner == type_of("colr")) {
                read_colr(part, track);
            } else if (inner == type_of("pasp")) {
                read_pasp(part, track);
            } else if (read_plain_csd(inner, part, track)) {
                // an av1C, vpcC, dfLa or mhaC: the plain csd-0
            } else if (is_sound && inner == type_of("wave")) {
                // Its esds, whose parent is not the mp4a, leaves the entry's rate and channels as they are.
                Span wave;
                for (uint32_t kind = 0; error == 0 && next_box(part, &kind, &wave);) {
                    if (kind == type_of("esds")) read_esds(wave, track, tables, false);
                    else read_plain_csd(kind, wave, track);
                }
                if (part.bad) fail(kMalformed);
            }
        }
        if (entry.bad) fail(kMalformed);
    }

    // As verifyTrack at the trak's end: an AVC track needs an avcC, an HEVC track an hvcC and an AAC track an esds, or
    // it is left out; its NAL length (MPEG4Source), csd and profile's bytes (convertMessageToMetaData, which takes
    // csd-avc and csd-hevc by the MIME type) come from that box, whatever boxes its other entries had. A box whose
    // bytes those refuse keeps the track, as MPEG4Extractor keeps the bytes: getTrack refuses an avcC under 7 bytes
    // or of another version than 1, and an hvcC under 22 bytes or of another version than 1 (no selecting the track);
    // the conversion refuses those avcCs and one whose sets pass its end, an hvcC under 23 bytes, of a version but 0
    // and 1 or whose units pass its end (its count of arrays a char, unsigned on arm64), and an esds ESDS refuses (no
    // format).
    void configure(Track& track, const Tables& tables) {
        const bool avc = strcmp(track.mime, "video/avc") == 0, hevc = strcmp(track.mime, "video/hevc") == 0;
        if (avc ? !tables.has_avcc : hevc ? !tables.has_hvcc : !tables.has_esds) {
            skip = true;
            return;
        }
        if (avc) {
            const Span& box = tables.avcc;
            track.select_refused = box.size < 7 || box.data[0] != 1;
            if (box.size >= 5) track.nal_length_size = (box.data[4] & 3u) + 1;
            keep_config(track, box);
            track.format_refused = track.select_refused || !split_avcc(box, track);
        } else if (hevc) {
            const Span& box = tables.hvcc;
            track.select_refused = box.size < 22 || box.data[0] != 1;
            if (box.size >= 22) track.nal_length_size = (box.data[21] & 3u) + 1;
            keep_config(track, box);
            track.format_refused = box.size < 23 || box.data[0] > 1 || !split_hvcc(box, track);
            if (!track.format_refused) read_vuis(box, track.vui);
        } else {
            Esds esds;  // ESDS's InitCheck, which the format's conversion takes
            if (!parse_esds(tables.esds, esds)) {
                if (esds.too_large) return fail_void(kMalformed);
                track.format_refused = true;
                return;
            }
            track.esds_max_bitrate = esds.max_bitrate;  // the conversion's, when the mp4a gave none
            track.esds_avg_bitrate = esds.avg_bitrate;
            const Span& info = esds.info;
            if (info.size == 0) return;
            track.csd0.data = static_cast<uint8_t*>(malloc(info.size));
            if (track.csd0.data == nullptr) return fail_void(kNoMemory);
            memcpy(track.csd0.data, info.data, info.size);
            track.csd0.size = info.size;
        }
    }

    // Utils' reassembleAVCC: version 1, the first SPS's profile, constraints and level (High, none and 1.3 without
    // one), each unit after a start code in csd-0 an SPS and in csd-1 a PPS, their counts written as Android does
    // (Opus's csd-1, a codec delay under 2^31 ns, holds no start code: its pass finds no unit).
    static size_t reassemble_avcc(const Bytes& csd0, const Bytes& csd1, uint8_t* avcc) {
        // findNextNalStartCode: the next 00 00 00 01 before the last 4 bytes, else the end.
        const auto next = [](const uint8_t* data, size_t length) -> size_t {
            for (size_t at = 0; length > 4 && at + 4 < length; at++) {
                if (data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 0 && data[at + 3] == 1) return at;
            }
            return length;
        };
        memcpy(avcc, "\x01\x64\x00\x0d\xff", 5);
        size_t at = 6;
        for (int pass = 0; pass < 2; pass++) {
            const Bytes& csd = pass == 0 ? csd0 : csd1;
            const size_t count_at = pass == 0 ? 5 : at++;
            size_t index = 0, last = 0;
            int units = 0;
            do {
                index += next(csd.data + index, csd.size - index);
                if (last > 0) {
                    const size_t size = index - last;
                    if (pass == 0 && units == 0 && size > 3) memcpy(avcc + 1, csd.data + last + 1, 3);
                    avcc[at++] = static_cast<uint8_t>(size >> 8);
                    avcc[at++] = static_cast<uint8_t>(size);
                    memcpy(avcc + at, csd.data + last, size);
                    at += size;
                    units++;
                }
                index += 4;
                last = index;
            } while (index < csd.size);
            avcc[count_at] = static_cast<uint8_t>(pass == 0 ? 0xe0 | units : units);
        }
        return at;
    }

    // The format's codec data when MPEG4Extractor's meta has a plain csd-0, which the extractor service's
    // convertMessageToMetaData takes in place of the avcC, hvcC or esds: an AVC track's csd-0 as is without a plain
    // csd-1 (no profile, level or csd-1), else the avcC reassembleAVCC makes of both; an HEVC track's format refused
    // (reassembleHVCC makes nothing of a csd-0 without a leading start code, and Minato does not port makeHvcc for one
    // with it); an AAC track's esds reassembleESDS's, its config the csd-0 and its bit rates 64 kbit/s.
    void plain_config(Track& track) {
        const bool avc = strcmp(track.mime, "video/avc") == 0, hevc = strcmp(track.mime, "video/hevc") == 0;
        track.format_refused = hevc;
        if (hevc) return;
        free(track.csd0.data);
        free(track.csd1.data);
        track.csd0 = Bytes{};
        track.csd1 = Bytes{};
        const Bytes& csd0 = track.opaque[0];
        if (avc && track.has_opaque[1]) {
            uint8_t* avcc = static_cast<uint8_t*>(malloc(csd0.size + track.opaque[1].size + 1024));
            if (avcc == nullptr) return fail_void(kNoMemory);
            const Span box{avcc, reassemble_avcc(csd0, track.opaque[1], avcc), 0, false};
            keep_config(track, box);
            track.format_refused = !split_avcc(box, track);
            free(avcc);
            return;
        }
        track.csd_opaque = true;
        track.csd0.data = static_cast<uint8_t*>(malloc(csd0.size > 0 ? csd0.size : 1));
        if (track.csd0.data == nullptr) return fail_void(kNoMemory);
        if (csd0.size > 0) memcpy(track.csd0.data, csd0.data, csd0.size);
        track.csd0.size = csd0.size;
        if (!avc) track.esds_avg_bitrate = track.esds_max_bitrate = 64000;
    }

    void read_stbl(Span stbl, Track& track, Tables& tables) {
        SampleTable& table = track.samples;
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
                read_stsd(body, track, tables);
            } else if (type == type_of("stsz")) {
                if (twice(&tables.has_sizes)) return;
                body.take(4);
                table.constant_size = body.u32();
                const uint32_t count = body.u32();
                tables.size_count = count;
                if (table.constant_size == 0) {
                    if (!fits(body, count, 4)) return fail_void(kMalformed);
                    table.sizes = entries(body, count);
                }
            } else if (type == type_of("stz2")) {
                if (twice(&tables.has_sizes)) return;
                body.take(7);
                const uint8_t field = body.u8();
                const uint32_t count = body.u32();
                if ((field != 4 && field != 8 && field != 16) ||
                    static_cast<uint64_t>(count) * field > static_cast<uint64_t>(body.left()) * 8) {
                    return fail_void(kMalformed);
                }
                tables.size_count = count;
                table.constant_size = 0;
                table.sizes = entries(body, count);
                table.size_bits = field;
            } else if (type == type_of("stco") || type == type_of("co64")) {
                if (twice(&tables.has_chunks)) return;
                body.take(4);
                const uint32_t count = body.u32();
                const size_t entry = type == type_of("stco") ? 4 : 8;
                if (!fits(body, count, entry)) return fail_void(kMalformed);
                table.chunks = entries(body, count);
                table.chunk_bytes = static_cast<uint32_t>(entry);
            } else if (type == type_of("stsc")) {
                if (twice(&tables.has_chunk_runs)) return;
                body.take(4);
                const uint32_t count = body.u32();
                // The description index of each run is not read: the first description serves all.
                if (!fits(body, count, 12)) return fail_void(kMalformed);
                table.chunk_runs = entries(body, count);
            } else if (type == type_of("stts")) {
                if (twice(&tables.has_times)) return;
                body.take(4);
                const uint32_t count = body.u32();
                if (!fits(body, count, 8)) return fail_void(kMalformed);
                table.time_runs = entries(body, count);
            } else if (type == type_of("ctts")) {
                if (twice(&tables.has_offsets)) return;
                body.take(4);  // version and flags: either version's offsets as signed (SampleTable::at)
                const uint32_t count = body.u32();
                if (!fits(body, count, 8)) return fail_void(kMalformed);
                table.offset_runs = entries(body, count);
            } else if (type == type_of("stss")) {
                if (twice(&tables.has_syncs)) return;
                body.take(4);
                const uint32_t count = body.u32();
                if (!fits(body, count, 4)) return fail_void(kMalformed);
                table.has_syncs = true;
                table.syncs = entries(body, count);
                // Entries that do not rise are sorted once, into memory the stss box bounds, so that every lookup
                // is a binary search and a read through the track never scans the table per sample.
                const uint8_t* first = movie + table.syncs.at;
                bool rising = true;
                for (uint32_t index = 1; index < count && rising; index++) {
                    rising = be32(first + (index - 1) * 4) <= be32(first + index * 4);
                }
                if (!rising) {
                    free(table.sorted_syncs);
                    table.sorted_syncs = static_cast<uint32_t*>(malloc(larger<size_t>(count, 1) * sizeof(uint32_t)));
                    if (table.sorted_syncs == nullptr) return fail_void(kNoMemory);
                    for (uint32_t index = 0; index < count; index++) {
                        table.sorted_syncs[index] = be32(first + index * 4);
                    }
                    qsort(table.sorted_syncs, count, sizeof(uint32_t), [](const void* a, const void* b) {
                        const uint32_t left = *static_cast<const uint32_t*>(a);
                        const uint32_t right = *static_cast<const uint32_t*>(b);
                        return left < right ? -1 : left > right ? 1 : 0;
                    });
                }
            }
            if (body.bad) fail(kMalformed);
        }
        if (stbl.bad) fail(kMalformed);
    }

    // The count of samples all tables describe, and the largest sample's size; the tables stay in the movie.
    void combine(Track& track, const Tables& tables) {
        if (!tables.has_sizes || !tables.has_chunks || !tables.has_chunk_runs || !tables.has_times) {
            skip = true;  // as Android's verifyTrack: a track without its sample tables is left out
            return;
        }
        SampleTable& table = track.samples;
        table.movie = movie;
        const uint8_t* base = movie;
        uint64_t count = tables.size_count;
        if (table.constant_size != 0) count = smaller<uint64_t>(count, source_length / table.constant_size);
        uint64_t timed = 0;
        for (uint32_t run = 0; run < table.time_runs.count; run++) timed += be32(base + table.time_runs.at + run * 8);
        count = smaller(count, timed);
        // Chunks: run r covers chunks first(r) to the next run's first, each of its samples per chunk; the runs
        // describe samples as far as they are in order and within the chunk table.
        uint64_t chunked = 0;
        for (uint32_t run = 0; run < table.chunk_runs.count && chunked < count; run++) {
            const uint8_t* entry = base + table.chunk_runs.at + run * 12;
            const uint64_t first = be32(entry);
            const uint64_t end = run + 1 < table.chunk_runs.count ? be32(entry + 12) : table.chunks.count + 1ull;
            if (first == 0 || end < first || end > table.chunks.count + 1ull) break;
            chunked += (end - first) * be32(entry + 4);  // below 2^64: at most 2^32 chunks of 2^32 samples
        }
        table.samples = static_cast<size_t>(smaller(count, chunked));
        // As SampleTable's countSamples and getMaxSampleSize: stsz's count, and the largest of its sizes.
        track.frame_count = tables.size_count;
        track.max_size = table.constant_size;
        for (size_t index = 0; table.constant_size == 0 && index < tables.size_count; index++) {
            track.max_size = larger(track.max_size, table.size(index));
        }
        if (table.constant_size != 0) {
            track.largest_sample = table.samples == 0 ? 0 : table.constant_size;
            return;
        }
        // Read once here, in one pass without memory per sample.
        for (size_t index = 0; index < table.samples; index++) {
            track.largest_sample = larger(track.largest_sample, table.size(index));
        }
    }

    // As MPEG4Extractor's parseTrackHeader: a tkhd of another size than its version's is malformed, one of another
    // version unsupported; the matrix gives the rotation, and the last two fields the display size.
    void read_tkhd(Span body, Track& track) {
        const uint8_t version = body.size > 0 ? body.data[0] : 0;
        const size_t dynamic = version == 1 ? 36 : 24;
        if (body.size < 4 || body.size != dynamic + 60) return fail_void(kMalformed);
        if (version > 1) return fail_void(kUnsupported);
        const uint8_t* bytes = body.data;
        track.id = be32(bytes + (version == 1 ? 20 : 12));
        const size_t matrix = dynamic + 16;
        const int32_t a00 = static_cast<int32_t>(be32(bytes + matrix));
        const int32_t a01 = static_cast<int32_t>(be32(bytes + matrix + 4));
        const int32_t a10 = static_cast<int32_t>(be32(bytes + matrix + 12));
        const int32_t a11 = static_cast<int32_t>(be32(bytes + matrix + 16));
        const int32_t one = 0x10000;
        track.rotation = a00 == 0 && a01 == one && a10 == -one && a11 == 0     ? 90
                         : a00 == 0 && a01 == -one && a10 == one && a11 == 0   ? 270
                         : a00 == -one && a01 == 0 && a10 == 0 && a11 == -one ? 180
                                                                               : 0;
        track.display_width = static_cast<int32_t>(be32(bytes + dynamic + 52) >> 16);
        track.display_height = static_cast<int32_t>(be32(bytes + dynamic + 56) >> 16);
    }

    // As Android's elst and getTrack: an empty first edit, then the media time of the first edit that shows media.
    void read_elst(Span body, Track& track) {
        const uint8_t version = body.u8();
        body.take(3);
        const uint32_t count = body.u32();
        if (count > 2) return;  // Android ignores an edit list of more entries
        track.edit_listed = true;
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
                if (index == 0) {
                    track.edit_first_media_time = media_time;
                    track.edit_first_segment = duration;
                }
            }
        }
        if (body.bad) return fail_void(kMalformed);
        if (has_empty) track.edit_empty_movie = empty;
    }

    // As getTrack, once the moov is read: the empty edit into the track's timescale by the movie's (the moov's last
    // mvhd's), rounded. A conversion that overflows marks the track, for which getTrack fails; ticks past int64_t's
    // range are taken as INT64_MAX.
    static void convert_edit(Track& track, uint32_t movie_timescale) {
        track.movie_timescale = movie_timescale;
        if (movie_timescale == 0) return;
        uint64_t scaled = 0;
        if (__builtin_mul_overflow(track.edit_empty_movie, static_cast<uint64_t>(track.timescale), &scaled) ||
            __builtin_add_overflow(scaled, static_cast<uint64_t>(movie_timescale / 2), &scaled)) {
            track.edit_overflow = true;
            return;
        }
        track.edit_empty = static_cast<int64_t>(smaller<uint64_t>(scaled / movie_timescale, INT64_MAX));
    }

    void read_trak(Span trak) {
        Track track;
        Tables tables;
        skip = false;
        Span edits;
        bool has_edits = false;
        uint32_t type = 0;
        Span body;
        while (error == 0 && next_box(trak, &type, &body)) {
            if (type == type_of("tkhd")) {
                read_tkhd(body, track);
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
        if (!keep(track, tables, has_edits, edits)) {
            release(track);
            return;
        }
        tracks->list[tracks->count++] = track;
    }

    // Whether the track read is one to keep, its edits and its tables taken: as Android's verifyTrack, a track without
    // a timescale (none, or no mdhd) is left out.
    bool keep(Track& track, const Tables& tables, bool has_edits, Span edits) {
        if (error != 0 || track.mime == nullptr || track.timescale == 0) return false;
        if (tracks->count >= kMaxTracks) {
            fail(kMalformed);
            return false;
        }
        if (has_edits) read_elst(edits, track);
        if (!skip && error == 0) configure(track, tables);
        if (!skip && error == 0 && track.has_opaque[0]) plain_config(track);
        if (!skip && error == 0) combine(track, tables);
        return !skip && error == 0;
    }

    // As MPEG4Extractor's mdhd: a version above 1 is an I/O error and a timescale of 0 malformed; a duration of 0 (or
    // a version 0 duration of all ones) gives none, and one whose µs pass int64_t's or a version 1 duration below 0
    // (all ones among them) is malformed. The language is the three packed letters, each plus 0x60.
    void read_mdhd(Span& body, Track& track) {
        const uint8_t version = body.u8();
        if (version > 1) return fail_void(kIo);
        body.take(3 + (version == 1 ? 16 : 8));
        track.timescale = body.u32();
        if (!body.bad && track.timescale == 0) return fail_void(kMalformed);
        const uint64_t duration = version == 1 ? body.u64() : body.u32();
        const bool none = duration == 0 || (version == 0 && duration == UINT32_MAX);
        if (!none && track.timescale != 0) {
            const unsigned __int128 scaled = static_cast<unsigned __int128>(duration) * 1000000 / track.timescale;
            if (static_cast<int64_t>(duration) < 0 || scaled > INT64_MAX) return fail_void(kMalformed);
            track.duration_us = static_cast<int64_t>(scaled);
        }
        const uint16_t code = body.u16();
        track.language[0] = static_cast<char>(((code >> 10) & 0x1f) + 0x60);
        track.language[1] = static_cast<char>(((code >> 5) & 0x1f) + 0x60);
        track.language[2] = static_cast<char>((code & 0x1f) + 0x60);
        track.language[3] = 0;
    }

    void read_mdia(Span mdia, Track& track, Tables& tables) {
        uint32_t type = 0;
        Span body;
        while (error == 0 && next_box(mdia, &type, &body)) {
            if (type == type_of("mdhd")) {
                read_mdhd(body, track);
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
                read_trak(body);
            }
            if (body.bad) fail(kMalformed);
        }
        if (moov.bad) fail(kMalformed);
        for (size_t index = 0; index < tracks->count; index++) convert_edit(tracks->list[index], movie_timescale);
    }
};

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
        const uint64_t many = smaller<uint64_t>(count - index, sizeof piece / 4);
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

uint32_t SampleTable::word(const Entries& entries, size_t index, size_t bytes, size_t field) const {
    return be32(movie + entries.at + index * bytes + field * 4);
}

uint32_t SampleTable::size(size_t index) const {
    if (constant_size != 0) return constant_size;
    const uint8_t* table = movie + sizes.at;
    switch (size_bits) {
        case 4: return index % 2 == 0 ? table[index / 2] >> 4 : table[index / 2] & 0xfu;
        case 8: return table[index];
        case 16: return static_cast<uint32_t>(table[index * 2]) << 8 | table[index * 2 + 1];
        default: return be32(table + index * 4);
    }
}

int SampleTable::at(size_t index, Sample* out) const {
    if (index >= samples) return kMalformed;
    // The chunk: the runs before the sample's are passed, from the cursor's run or, for an earlier sample, the first.
    if (index < chunk_cursor_.first) chunk_cursor_ = Cursor{};
    uint64_t first_chunk = 0, per_chunk = 0;
    for (;; chunk_cursor_.run++) {
        // The parse counted samples only as far as the runs are in order, so the sample's run is reached first.
        const size_t run = chunk_cursor_.run;
        first_chunk = word(chunk_runs, run, 12, 0);
        per_chunk = word(chunk_runs, run, 12, 1);
        const uint64_t end = run + 1 < chunk_runs.count ? word(chunk_runs, run + 1, 12, 0) : chunks.count + 1ull;
        const uint64_t in_run = (end - first_chunk) * per_chunk;
        if (index - chunk_cursor_.first < in_run) break;
        chunk_cursor_.first += in_run;
    }
    const uint64_t chunk = first_chunk + (index - chunk_cursor_.first) / per_chunk;
    const uint64_t chunk_first = chunk_cursor_.first + (chunk - first_chunk) * per_chunk;
    // Its offset: the chunk's, plus the sizes of the chunk's samples before it, from the last sample found in the
    // chunk when that one is not after it.
    uint64_t sample = chunk_first, offset = 0;
    if (cached_chunk_ == chunk && cached_sample_ >= chunk_first && cached_sample_ <= index) {
        sample = cached_sample_;
        offset = cached_offset_;
    } else {
        const uint8_t* entry = movie + chunks.at + (chunk - 1) * chunk_bytes;
        offset = chunk_bytes == 8 ? static_cast<uint64_t>(be32(entry)) << 32 | be32(entry + 4) : be32(entry);
    }
    if (constant_size != 0) {
        uint64_t before = 0;
        if (__builtin_mul_overflow(index - sample, uint64_t{constant_size}, &before) ||
            __builtin_add_overflow(offset, before, &offset)) {
            return kMalformed;
        }
    } else {
        for (; sample < index; sample++) {
            uint64_t next = 0;
            if (__builtin_add_overflow(offset, size(sample), &next)) {
                // The last sample whose offset holds is kept, so that the samples after it fail at once.
                cached_chunk_ = chunk;
                cached_sample_ = sample;
                cached_offset_ = offset;
                return kMalformed;
            }
            offset = next;
        }
    }
    cached_chunk_ = chunk;
    cached_sample_ = index;
    cached_offset_ = offset;
    const uint32_t bytes = size(index);
    if (offset > UINT64_MAX - bytes) return kMalformed;
    Sample found{offset, 0, bytes, 0, 0, chunk_first + per_chunk - 1};
    const int answer = timing(index, &found);
    if (answer == 0 && out != nullptr) *out = found;
    return answer;
}

int SampleTable::timing(size_t index, Sample* out) const {
    if (index >= samples) return kMalformed;
    // Its decode time: the time runs before its own, then its place in its run.
    if (index < time_cursor_.first) time_cursor_ = Cursor{};
    // The sums stay below 2^64: fewer than 2^32 samples (stsz's count), each of a delta below 2^32.
    for (;; time_cursor_.run++) {
        const uint64_t many = word(time_runs, time_cursor_.run, 8, 0);
        if (index - time_cursor_.first < many) break;
        time_cursor_.start += many * word(time_runs, time_cursor_.run, 8, 1);
        time_cursor_.first += many;
    }
    const uint32_t delta = word(time_runs, time_cursor_.run, 8, 1);
    const uint64_t decode = time_cursor_.start + (index - time_cursor_.first) * delta;
    if (decode > static_cast<uint64_t>(INT64_MAX)) return kMalformed;
    // Its composition offset: ctts's for the samples it covers, 0 past them. Version 0's offsets are unsigned by the
    // standard and taken as signed by Android and writers.
    if (index < offset_cursor_.first) offset_cursor_ = Cursor{};
    int32_t composition_offset = 0;
    for (; offset_cursor_.run < offset_runs.count; offset_cursor_.run++) {
        const uint64_t many = word(offset_runs, offset_cursor_.run, 8, 0);
        if (index - offset_cursor_.first < many) {
            composition_offset = static_cast<int32_t>(word(offset_runs, offset_cursor_.run, 8, 1));
            break;
        }
        offset_cursor_.first += many;
    }
    out->decode = static_cast<int64_t>(decode);
    out->composition_offset = composition_offset;
    out->duration = delta;
    return 0;
}

bool SampleTable::is_sync(size_t index) const {
    if (!has_syncs) return true;
    if (index >= UINT32_MAX) return false;
    if (index == 0 && listed(0)) return true;  // SampleTable keeps an entry 0 as 0, the first sample
    return listed(static_cast<uint32_t>(index + 1));
}

bool SampleTable::listed(uint32_t wanted) const {
    size_t low = 0, high = syncs.count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const uint32_t sync = sorted_syncs != nullptr ? sorted_syncs[middle] : word(syncs, middle, 4, 0);
        if (sync == wanted) return true;
        if (sync < wanted) low = middle + 1;
        else high = middle;
    }
    return false;
}

uint32_t SampleTable::sync_entry(size_t index) const {
    const uint32_t entry = sorted_syncs != nullptr ? sorted_syncs[index] : word(syncs, index, 4, 0);
    return entry == 0 ? 0 : entry - 1;
}

namespace {

constexpr int kOutOfRange = -1008;  // ERROR_OUT_OF_RANGE
enum Find { kBefore, kAfter, kClosest };  // SampleTable's kFlagBefore, kFlagAfter and kFlagClosest

uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

// A long double in int64_t's range, saturated there.
int64_t saturated(long double value) {
    if (value >= 9223372036854775808.0L) return INT64_MAX;
    if (value < -9223372036854775808.0L) return INT64_MIN;
    return static_cast<int64_t>(value);
}

int64_t saturated_add(int64_t a, int64_t b) {
    int64_t sum = 0;
    if (!__builtin_add_overflow(a, b, &sum)) return sum;
    return b > 0 ? INT64_MAX : INT64_MIN;
}

// Ticks in µs, truncated, as MPEG4Source's long double arithmetic gives them (saturated past int64_t's range, where
// Android's overflows).
int64_t ticks_us(int64_t ticks, uint32_t timescale) {
    return saturated(static_cast<long double>(ticks) * 1000000 / timescale);
}

// SampleTable's buildSampleEntriesTable: each sample's composition time, by time; none over kMaxTableBytes with the
// tables, as its mTotalSize counts them.
void make_index(Track& track) {
    SeekIndex& index = track.seek_index;
    index.made = true;
    const SampleTable& table = track.samples;
    const size_t count = table.count();
    const uint64_t tables = uint64_t{table.chunk_runs.count} * 12 + uint64_t{table.time_runs.count} * 8 +
                            uint64_t{table.offset_runs.count} * 8 + uint64_t{table.syncs.count} * 4;
    if (count == 0 || tables + uint64_t{count} * sizeof(SeekIndex::Entry) > kMaxTableBytes) return;
    index.entries = static_cast<SeekIndex::Entry*>(malloc(count * sizeof(SeekIndex::Entry)));
    if (index.entries == nullptr) return;
    // SampleTable's running time: a negative offset larger than it resets it to 0 (and the entry's time), so the
    // samples after it are earlier by the time it had. -INT32_MIN stays INT32_MIN in its int arithmetic.
    uint64_t reset = 0;
    for (size_t at = 0; at < count; at++) {
        Sample sample{};
        uint64_t time = UINT64_MAX;  // a time past int64_t's, as SampleTable clamps an overflow
        if (table.timing(at, &sample) == 0) {
            const uint64_t running = static_cast<uint64_t>(sample.decode) - reset;
            const int32_t offset = sample.composition_offset;
            const uint64_t back = offset == INT32_MIN ? static_cast<uint64_t>(int64_t{INT32_MIN})
                                                      : static_cast<uint64_t>(-static_cast<int64_t>(offset));
            const uint64_t least = offset == INT32_MIN ? INT32_MAX : back;
            if (offset < 0 && running < least) {
                reset = static_cast<uint64_t>(sample.decode);
                time = 0;
            } else {
                time = offset < 0 ? running - back : running + static_cast<uint64_t>(offset);
            }
        }
        index.entries[at] = SeekIndex::Entry{time, static_cast<uint32_t>(at)};
    }
    index.count = count;
    qsort(index.entries, count, sizeof(SeekIndex::Entry), [](const void* a, const void* b) {
        const uint64_t left = static_cast<const SeekIndex::Entry*>(a)->time;
        const uint64_t right = static_cast<const SeekIndex::Entry*>(b)->time;
        return left < right ? -1 : left > right ? 1 : 0;
    });
}

// SampleTable's getSampleTime: entry `at`'s time in µs, rounded.
uint64_t entry_us(const SeekIndex& index, size_t at, uint32_t timescale) {
    return (index.entries[at].time * 1000000 + timescale / 2) / timescale;
}

// SampleTable's findSampleAtTime.
int sample_at_time(const Track& track, uint64_t time_us, Find find, uint32_t* sample) {
    const SeekIndex& index = track.seek_index;
    if (index.entries == nullptr) return kOutOfRange;
    size_t left = 0, right = index.count;
    while (left < right) {
        const size_t center = left + (right - left) / 2;
        const uint64_t center_us = entry_us(index, center, track.timescale);
        if (time_us < center_us) {
            right = center;
        } else if (time_us > center_us) {
            left = center + 1;
        } else {
            *sample = index.entries[center].sample;
            return 0;
        }
    }
    size_t closest = left;
    if (closest == index.count) {
        if (find == kAfter) return kOutOfRange;
        find = kBefore;
    } else if (closest == 0) {
        find = kAfter;  // the first sample, where the end of the stream would be the answer before it
    }
    if (find == kBefore) {
        closest--;
    } else if (find == kClosest && distance(entry_us(index, closest, track.timescale), time_us) >
                                       distance(time_us, entry_us(index, closest - 1, track.timescale))) {
        closest--;
    }
    *sample = index.entries[closest].sample;
    return 0;
}

// A sample's composition time as SampleIterator's getSampleTime gives it, or false.
bool composition(const Track& track, size_t at, uint64_t* time) {
    Sample sample{};
    if (track.samples.timing(at, &sample) != 0) return false;
    *time = static_cast<uint64_t>(sample.composition());
    return true;
}

// SampleTable's findSyncSampleNear.
int sync_near(const Track& track, uint32_t start, Find find, size_t* sync) {
    const SampleTable& table = track.samples;
    *sync = 0;
    if (!table.has_syncs) {
        *sync = start;
        return 0;
    }
    if (table.syncs.count == 0) return 0;
    size_t left = 0, right = table.syncs.count;
    while (left < right) {
        const size_t center = left + (right - left) / 2;
        const uint32_t entry = table.sync_entry(center);
        if (start < entry) {
            right = center;
        } else if (start > entry) {
            left = center + 1;
        } else {
            *sync = entry;
            return 0;
        }
    }
    if (left == table.syncs.count) {
        if (find == kAfter) return kOutOfRange;  // "tried to find a sync frame after the last one"
        find = kBefore;
    } else if (left == 0) {
        find = kAfter;
    }
    if (find == kBefore) {
        left--;
    } else if (find == kClosest) {
        uint64_t time = 0, upper = 0, lower = 0;
        if (!composition(track, start, &time) || !composition(track, table.sync_entry(left), &upper) ||
            !composition(track, table.sync_entry(left - 1), &lower)) {
            return kMalformed;
        }
        if (distance(upper, time) > distance(time, lower)) left--;
    }
    *sync = table.sync_entry(left);
    return 0;
}

}  // namespace

int seek(Track& track, int64_t time_us, SeekMode mode, size_t* sample) {
    const Find find = mode == kSeekPreviousSync ? kBefore : mode == kSeekNextSync ? kAfter : kClosest;
    // MPEG4Source's edits: the empty edit's time comes off (not below 0), the shift's goes on.
    if (track.edit_empty > 0) time_us = larger<int64_t>(time_us - ticks_us(track.edit_empty, track.timescale), 0);
    if (track.edit_media_time > 0) time_us = saturated_add(time_us, ticks_us(track.edit_media_time, track.timescale));
    if (!track.seek_index.made) make_index(track);
    uint32_t found = 0;
    int answer = sample_at_time(track, static_cast<uint64_t>(time_us), find, &found);
    size_t sync = found;
    // Every audio sample counts as a sync sample, as MPEG4Source has it for AAC but USAC (audio object 42).
    const bool all_sync = strncmp(track.mime, "audio/", 6) == 0 && track.audio_object != 42;
    if (answer == 0 && !all_sync) answer = sync_near(track, found, find, &sync);
    Sample at{};
    if (answer == 0 && track.samples.at(found, &at) != 0) answer = kMalformed;
    if (answer != 0) return answer == kOutOfRange ? kEndOfStream : kUnknown;
    *sample = sync;
    return 0;
}

int64_t sample_time_us(const Track& track, const Sample& sample) {
    int64_t ticks = sample.composition();
    if (track.edit_empty > 0) ticks = saturated_add(ticks, track.edit_empty);
    if (track.edit_media_time > 0) ticks = saturated_add(ticks, -track.edit_media_time);
    return ticks_us(ticks, track.timescale);
}

int64_t duration_us(const Track& track, uint64_t ticks) {
    return saturated(static_cast<long double>(ticks) * 1000000 / track.timescale);
}

void Tracks::clear() {
    for (size_t index = 0; index < count; index++) release(list[index]);
    free(list);
    free(movie);
    list = nullptr;
    count = 0;
    movie = nullptr;
}

int parse(const Source& source, Tracks* tracks) {
    tracks->clear();
    uint8_t* movie = nullptr;
    size_t movie_size = 0;
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
            movie_size = static_cast<size_t>(size - length);
            movie = static_cast<uint8_t*>(malloc(larger<size_t>(movie_size, 1)));
            if (movie == nullptr) return kNoMemory;
            found = true;
            if (!read_exactly(source, offset + length, movie, movie_size)) return kIo;
        }
        offset += size;
        return 0;
    };
    // As MPEG4Extractor::readMetaData: the walk ends once the brands said there is a movie and both the moov and an
    // mdat were seen; an error ends the file before the moov and only the walk after it (mInitCheck is OK by then),
    // but a fragment, which this step does not read.
    int answer = 0;
    bool read = false;  // the moov's bytes are all there
    while (offset < source.length && !(branded && read && data)) {
        answer = next();
        read = found && answer != kIo && answer != kNoMemory;
        if (answer == kUnsupported || (answer != 0 && !read)) {
            free(movie);
            return answer;
        }
        if (answer != 0) break;
    }
    if (!found) return kMalformed;
    tracks->movie = movie;
    tracks->list = static_cast<Track*>(calloc(kMaxTracks, sizeof(Track)));
    if (tracks->list == nullptr) {
        tracks->clear();
        return kNoMemory;
    }
    Parser parser{tracks, source.length, movie};
    parser.read_moov(Span{movie, movie_size, 0, false});
    if (parser.error != 0) {
        tracks->clear();
        return parser.error;
    }
    return 0;
}

long output_size(const Track& track, size_t index) {
    if (index >= track.samples.count()) return kMalformed;
    // As MPEG4Source::start: a track whose largest sample plus 20 bytes is above the buffer it allocates is refused.
    if (track.largest_sample > kMaxInputBytes - 20) return kMalformed;
    const uint32_t size = track.samples.size(index);
    // A NAL length of fewer than 4 bytes becomes a 4-byte start code: at most 3 more bytes a unit of one byte or more
    // (a unit of length 0 is skipped).
    if (track.nal_length_size == 0 || track.nal_length_size == 4) return static_cast<long>(size);
    const uint64_t units = size / (track.nal_length_size + 1) + 1;
    return static_cast<long>(size + units * (4 - track.nal_length_size));
}

long read_sample(const Source& source, const Track& track, size_t index, uint8_t* out, size_t capacity) {
    const long longest = output_size(track, index);
    if (longest < 0) return longest;
    if (capacity < static_cast<size_t>(longest)) return kNoMemory;
    Sample sample;
    if (track.samples.at(index, &sample) != 0) return kMalformed;
    if (sample.offset > source.length || sample.size > source.length - sample.offset) return kMalformed;
    if (track.nal_length_size == 0) {
        if (!read_exactly(source, sample.offset, out, sample.size)) return kIo;
        return static_cast<long>(sample.size);
    }
    // As MPEG4Source::read: read into the end of the buffer, then copy each unit forward after a start code. A unit of
    // length 0 is skipped, a length past the sample drops the rest of it ("abnormal nallength, ignore this NAL"), and
    // the units may not grow past MPEG4Source's buffer, max-input-size (the largest size plus 20).
    const size_t from = capacity - sample.size;
    if (!read_exactly(source, sample.offset, out + from, sample.size)) return kIo;
    const uint64_t buffer = uint64_t{track.max_size} + 20;
    size_t at = from, written = 0;
    const size_t end = capacity;
    while (at < end) {
        if (end - at < track.nal_length_size) break;
        uint32_t length = 0;
        for (uint32_t byte = 0; byte < track.nal_length_size; byte++) length = length << 8 | out[at + byte];
        at += track.nal_length_size;
        if (length > end - at) break;
        if (length == 0) continue;
        if (written + 4 + uint64_t{length} > buffer) return kMalformed;
        if (written + 4 > at) return kMalformed;  // the unit would overwrite unread bytes (output_size rules it out)
        out[written] = out[written + 1] = out[written + 2] = 0;
        out[written + 3] = 1;
        memmove(out + written + 4, out + at, length);
        written += 4 + length;
        at += length;
    }
    return static_cast<long>(written);
}

namespace {

// ColorUtils at android-15.0.0_r1 over VideoAPI.h's ColorAspects (range, primaries, transfer and matrix values) and
// the platform's colour range, standard and transfer; ALookup takes a table's first pair of a value.
struct Aspects {
    uint32_t range = 0, primaries = 0, transfer = 0, matrix = 0;
};

constexpr uint32_t kOther = 0xff;  // RangeOther, PrimariesOther, TransferOther, MatrixOther

// sIsoPrimaries, sIsoTransfers and sIsoMatrixCoeffs: an ISO value and its aspect.
constexpr uint32_t kIsoPrimaries[][2] = {{1, 1}, {2, 0}, {4, 2}, {5, 3}, {6, 4}, {7, 4}, {8, 5}, {9, 6}, {10, kOther},
                                         {11, 7}, {12, 8}};
constexpr uint32_t kIsoTransfers[][2] = {{1, 3},       {2, 0},       {4, 4},     {5, 5},     {6, 3},    {7, 0x40},
                                         {8, 1},       {9, kOther},  {10, kOther}, {11, 0x41}, {12, 0x42}, {13, 2},
                                         {14, 3},      {15, 3},      {16, 6},    {17, 0x43}, {18, 7}};
constexpr uint32_t kIsoMatrices[][2] = {{0, kOther}, {1, 1}, {2, 0}, {4, 2}, {6, 3}, {5, 3}, {7, 4}, {8, kOther},
                                        {9, 5},      {10, 6}};
// sStandards: a platform standard, its primaries and its matrix.
constexpr uint32_t kStandards[][3] = {{0, 0, 0}, {1, 1, 1}, {2, 3, 3}, {3, 3, 1}, {4, 4, 3}, {5, 4, 4},
                                      {6, 6, 5}, {7, 6, 6}, {8, 2, 2}, {9, 5, 5}, {10, 8, 3}};

template <size_t count>
uint32_t iso_aspect(const uint32_t (&table)[count][2], uint32_t value) {
    for (const auto& pair : table) {
        if (pair[0] == value) return pair[1];
    }
    return 0;  // unspecified
}

// convertIsoColorAspectsToCodecAspects.
Aspects iso_aspects(uint32_t primaries, uint32_t transfer, uint32_t matrix, bool full_range) {
    return Aspects{full_range ? 1u : 2u, iso_aspect(kIsoPrimaries, primaries), iso_aspect(kIsoTransfers, transfer),
                   iso_aspect(kIsoMatrices, matrix)};
}

// convertCodecColorAspectsToPlatformAspects: the wrapped values, and false for an aspect past kOther.
bool platform_aspects(const Aspects& aspects, int32_t* range, int32_t* standard, int32_t* transfer) {
    const uint32_t r = aspects.range, p = aspects.primaries, t = aspects.transfer, m = aspects.matrix;
    *range = static_cast<int32_t>(r <= 2 ? r : r <= kOther ? 0x10000 + r : 0);
    *standard = -1;
    for (const auto& entry : kStandards) {
        if (entry[1] == p && entry[2] == m) {
            *standard = static_cast<int32_t>(entry[0]);
            break;
        }
    }
    if (*standard == -1) {
        *standard = static_cast<int32_t>(p > kOther || m > kOther ? 0
                                         : p <= 6 && m <= 6      ? 64 + p + m * 7
                                                                 : 0x10000 + p + m * 0x100);
    }
    const bool defined = t <= 7 || (t >= 0x40 && t <= 0x43);
    *transfer = static_cast<int32_t>(t <= 7 ? t : t > kOther ? 0 : defined ? 32 + t : 0x10000 + t);
    return r <= kOther && p <= kOther && m <= kOther && t <= kOther;
}

// convertPlatformColorAspectsToCodecAspects: false when any value does not unwrap.
bool codec_aspects(int32_t range, int32_t standard, int32_t transfer, Aspects* aspects) {
    bool ok = true;
    if (range >= 0 && range <= 2) {
        aspects->range = static_cast<uint32_t>(range);
    } else if (range >= 0x10000 && range < 0x10000 + 0x100) {
        aspects->range = static_cast<uint32_t>(range - 0x10000);
    } else {
        aspects->range = kOther;
        ok = false;
    }
    bool listed = false;
    for (const auto& entry : kStandards) {
        if (static_cast<int32_t>(entry[0]) == standard) {
            aspects->primaries = entry[1];
            aspects->matrix = entry[2];
            listed = true;
            break;
        }
    }
    if (!listed) {
        const bool vendor = standard >= 0x10000;
        const int32_t start = vendor ? 0x10000 : 64, primaries = vendor ? 0x100 : 7, matrices = vendor ? 0x100 : 7;
        if (standard >= start && standard < start + primaries * matrices) {
            aspects->primaries = static_cast<uint32_t>((standard - start) % primaries);
            aspects->matrix = static_cast<uint32_t>((standard - start) / primaries);
        } else {
            aspects->primaries = kOther;
            aspects->matrix = kOther;
            ok = false;
        }
    }
    if (transfer >= 0 && transfer <= 7) {
        aspects->transfer = static_cast<uint32_t>(transfer);
    } else {
        const bool vendor = transfer >= 0x10000;
        const int32_t start = vendor ? 0x10000 : 32, transfers = vendor ? 0x100 : 0x44;
        if (transfer >= start && transfer < start + transfers) {
            aspects->transfer = static_cast<uint32_t>(transfer - start);
        } else {
            aspects->transfer = kOther;
            ok = false;
        }
    }
    return ok;
}

}  // namespace

ColorKeys color_keys(const Track& track) {
    ColorKeys keys;
    if (track.mime == nullptr || strncmp(track.mime, "video/", 6) != 0) return keys;
    // The colr's keys in the track's format (those not 0), through the extractor service's convertMessageToMetaData
    // (the codec aspects not 0 kept) and back in convertMetaDataToMessageColorAspects.
    Aspects kept;
    if (track.has_colr) {
        int32_t range = 0, standard = 0, transfer = 0;
        platform_aspects(iso_aspects(track.colr_primaries, track.colr_transfer, track.colr_matrix,
                                     track.colr_full_range),
                         &range, &standard, &transfer);
        if (!codec_aspects(range, standard, transfer, &kept)) kept = Aspects{};
        if (platform_aspects(kept, &range, &standard, &transfer)) {
            if (range != 0) keys.range = range;
            if (standard != 0) keys.standard = standard;
            if (transfer != 0) keys.transfer = transfer;
        }
    }
    if (strcmp(track.mime, "video/hevc") != 0) return keys;
    const Track::Vui& vui = track.vui;
    if (vui.described) keys.hdr = vui.hdr ? 1 : 0;
    if (!vui.has_primaries || !vui.has_transfer || !vui.has_matrix || !vui.has_full_range) return keys;
    Aspects aspects = iso_aspects(vui.primaries, vui.transfer, vui.matrix, vui.full_range != 0);
    if (aspects.primaries == 0) aspects.primaries = kept.primaries;  // unspecified: the colr's, when it gave one
    if (aspects.transfer == 0) aspects.transfer = kept.transfer;
    if (aspects.matrix == 0) aspects.matrix = kept.matrix;
    if (aspects.range == 0) aspects.range = kept.range;
    int32_t range = 0, standard = 0, transfer = 0;
    if (platform_aspects(aspects, &range, &standard, &transfer)) {
        keys.standard = standard;
        keys.transfer = transfer;
        keys.range = range;
    }
    return keys;
}

int32_t aac_profile(const uint8_t* csd, size_t size) {
    if (size < 2) return 0;
    uint32_t object = static_cast<uint32_t>(csd[0]) << 8 | csd[1];
    object = (object & 0xf800u) == 0xf800u ? 32 + ((object >> 5) & 0x3fu) : object >> 11;
    static const uint32_t objects[] = {1, 2, 3, 4, 5, 6, 17, 23, 29, 39, 42};
    for (uint32_t known : objects) {
        if (object == known) return static_cast<int32_t>(object);
    }
    return 0;
}

}  // namespace minato::mp4
