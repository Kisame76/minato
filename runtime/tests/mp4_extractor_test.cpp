// ADR-0060 e2d: the ISO BMFF parser of libmediandk's AMediaExtractor (runtime/android/mp4_extractor.cpp), on the
// host. Minato's own test files (tools/make-mp4-sample.sh) against the H.264 streams they were made from: each sample
// is the stream's access unit without its parameter sets, in Annex B, at the index's time. Then files of boxes this
// test builds, hostile ones among them (the ADR's edge cases: truncated boxes, sizes past the end, 64-bit sizes,
// sample tables that disagree, counts the box cannot hold, encrypted and fragmented files), and with --sweep, the
// test files with each byte changed and cut at each length, within a time budget: never a crash, a read past a buffer
// (ASan) or an allocation the file decides.
#include "mp4_extractor.h"

#include "check.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <malloc/malloc.h>
#include <sanitizer/allocator_interface.h>
#include <random>
#include <string>
#include <vector>

// The bytes the process holds from malloc: the sanitizer's count in the ASan build, the default zone's otherwise.
size_t held_bytes() {
#if defined(__has_feature) && __has_feature(address_sanitizer)
    return __sanitizer_get_current_allocated_bytes();
#else
    malloc_statistics_t statistics;
    malloc_zone_statistics(nullptr, &statistics);
    return statistics.size_in_use;
#endif
}

namespace {

using minato::mp4::Source;
using minato::mp4::Track;
using Bytes = std::vector<uint8_t>;

Bytes file_bytes(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const std::string kAssets = std::string(APKRT_SOURCE_DIR) + "/fixtures/apps/";

ssize_t read_memory(void *context, uint64_t offset, void *data, size_t size) {
    const auto *bytes = static_cast<const Bytes *>(context);
    if (offset > bytes->size()) return -1;
    const size_t got = std::min<uint64_t>(size, bytes->size() - offset);
    std::memcpy(data, bytes->data() + offset, got);
    return static_cast<ssize_t>(got);
}

// A track's text and bytes as the test compares them.
std::string text(const char *value) { return value == nullptr ? std::string() : std::string(value); }
Bytes bytes(const minato::mp4::Bytes &value) { return Bytes(value.data, value.data + value.size); }

Source source_of(const Bytes &bytes) {
    return Source{const_cast<Bytes *>(&bytes), read_memory, bytes.size()};
}

// The stream's access units by its index: bytes, the frame's time in frames, key.
struct Unit {
    Bytes bytes;
    int64_t frame;
    bool key;
};

std::vector<Unit> units_of(const std::string &name) {
    const Bytes stream = file_bytes(kAssets + "media-codec-test/assets/" + name + ".h264");
    std::ifstream index(kAssets + "media-codec-test/assets/" + name + ".idx");
    std::vector<Unit> units;
    long long offset, size, pts;
    int key;
    while (index >> offset >> size >> pts >> key) {
        units.push_back({Bytes(stream.begin() + offset, stream.begin() + offset + size), (pts * 30 + 500000) / 1000000,
                         key != 0});
    }
    return units;
}

// The NAL units of an Annex B access unit, without start codes.
std::vector<Bytes> nal_units(const Bytes &unit) {
    std::vector<Bytes> units;
    size_t start = SIZE_MAX;
    for (size_t at = 0; at + 3 <= unit.size();) {
        if (unit[at] == 0 && unit[at + 1] == 0 && unit[at + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = at;
                while (end > start && unit[end - 1] == 0) end--;
                units.emplace_back(unit.begin() + static_cast<long>(start), unit.begin() + static_cast<long>(end));
            }
            at += 3;
            start = at;
        } else {
            at++;
        }
    }
    if (start != SIZE_MAX && start < unit.size()) {
        units.emplace_back(unit.begin() + static_cast<long>(start), unit.end());
    }
    return units;
}

Bytes with_start_code(const Bytes &nal) {
    Bytes out{0, 0, 0, 1};
    out.insert(out.end(), nal.begin(), nal.end());
    return out;
}

// The access unit as the extractor gives it: its NAL units but the parameter sets and delimiters, each after 00000001.
Bytes expected_sample(const Unit &unit) {
    Bytes out;
    for (const Bytes &nal : nal_units(unit.bytes)) {
        const int type = nal[0] & 0x1f;
        if (type == 7 || type == 8 || type == 9) continue;
        const Bytes coded = with_start_code(nal);
        out.insert(out.end(), coded.begin(), coded.end());
    }
    return out;
}

Bytes read_all(const Source &source, const Track &track, size_t index, long *answer) {
    const long size = minato::mp4::output_size(track, index);
    Bytes out(size > 0 ? static_cast<size_t>(size) : 0);
    *answer = minato::mp4::read_sample(source, track, index, out.data(), out.size());
    return out;
}

// Sample `index` of `track`, which must have it.
minato::mp4::Sample sample_of(const Track &track, size_t index) {
    minato::mp4::Sample sample{};
    CHECK(track.samples.at(index, &sample) == 0);
    return sample;
}

void check_video_track(const Track &track, const std::vector<Unit> &units, int64_t shift) {
    CHECK(text(track.mime) == "video/avc");
    CHECK(track.width == 320 && track.height == 180);
    CHECK(track.timescale == 30000 && track.nal_length_size == 4);
    CHECK(track.edit_media_time == shift * 1000 && track.edit_empty == 0);
    CHECK(text(track.language) == "und");
    std::vector<Bytes> sets = nal_units(units[0].bytes);
    Bytes sps, pps;
    for (const Bytes &nal : sets) {
        if ((nal[0] & 0x1f) == 7 && sps.empty()) sps = with_start_code(nal);
        if ((nal[0] & 0x1f) == 8 && pps.empty()) pps = with_start_code(nal);
    }
    CHECK(bytes(track.csd0) == sps && bytes(track.csd1) == pps);
    CHECK(track.samples.count() == units.size());
    if (track.samples.count() != units.size()) return;
    uint32_t largest = 0;
    for (size_t index = 0; index < units.size(); index++) {
        const auto &sample = sample_of(track, index);
        CHECK(sample.composition() == units[index].frame * 1000 + shift * 1000);
        CHECK(track.samples.is_sync(index) == units[index].key);
        largest = std::max(largest, sample.size);
    }
    CHECK(track.largest_sample == largest);
}

TEST(baseline_mp4_has_the_streams_frames_and_an_aac_track) {
    const Bytes file = file_bytes(kAssets + "media-extractor-test/assets/baseline.mp4");
    const Source source = source_of(file);
    minato::mp4::Tracks tracks;
    CHECK(!file.empty() && minato::mp4::parse(source, &tracks) == 0);
    CHECK(tracks.size() == 2);
    if (tracks.size() != 2) return;
    const std::vector<Unit> units = units_of("baseline");
    check_video_track(tracks[0], units, 0);
    for (size_t index = 0; index < tracks[0].samples.count() && index < units.size(); index++) {
        long answer = 0;
        const Bytes got = read_all(source, tracks[0], index, &answer);
        CHECK(answer == static_cast<long>(got.size()) && got == expected_sample(units[index]));
    }
    const Track &audio = tracks[1];
    CHECK(text(audio.mime) == "audio/mp4a-latm");
    CHECK(audio.sample_rate == 44100 && audio.channels == 1 && audio.timescale == 44100);
    CHECK((bytes(audio.csd0) == Bytes{0x12, 0x08}));  // AAC LC, 44.1 kHz, mono
    CHECK(audio.nal_length_size == 0 && audio.edit_media_time == 2112);  // the encoder's priming
    CHECK(audio.samples.count() == 46);
    for (size_t index = 0; index < audio.samples.count(); index++) {
        const minato::mp4::Sample sample = sample_of(audio, index);
        CHECK(audio.samples.is_sync(index) && sample.decode == static_cast<int64_t>(index) * 1024);
        long answer = 0;
        const Bytes got = read_all(source, audio, index, &answer);
        CHECK(answer == static_cast<long>(sample.size) && got.size() == sample.size);
    }
}

TEST(main_mp4_shows_frames_in_another_order_than_it_decodes_them) {
    const Bytes file = file_bytes(kAssets + "media-extractor-test/assets/main.mp4");
    const Source source = source_of(file);
    minato::mp4::Tracks tracks;
    CHECK(minato::mp4::parse(source, &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() != 1) return;
    const std::vector<Unit> units = units_of("main");
    check_video_track(tracks[0], units, 2);  // decoded two frames ahead, its first edit starts two frames in
    for (size_t index = 0; index < tracks[0].samples.count() && index < units.size(); index++) {
        CHECK(sample_of(tracks[0], index).decode == static_cast<int64_t>(index) * 1000);
        long answer = 0;
        CHECK(read_all(source, tracks[0], index, &answer) == expected_sample(units[index]));
    }
}

// The cursors find a sample the same way from anywhere: every sample of both test files, asked for backwards and in
// a shuffled order after a read through, is the sample of the read through.
TEST(samples_asked_for_in_any_order_are_the_same) {
    for (const char *name : {"baseline", "main"}) {
        const Bytes file = file_bytes(kAssets + "media-extractor-test/assets/" + name + ".mp4");
        minato::mp4::Tracks tracks;
        CHECK(minato::mp4::parse(source_of(file), &tracks) == 0 && !tracks.empty());
        for (const Track &track : tracks) {
            std::vector<minato::mp4::Sample> forward;
            for (size_t index = 0; index < track.samples.count(); index++) forward.push_back(sample_of(track, index));
            std::vector<size_t> order;
            for (size_t index = forward.size(); index > 0; index--) order.push_back(index - 1);
            std::vector<size_t> shuffled = order;
            std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937(7));
            order.insert(order.end(), shuffled.begin(), shuffled.end());
            size_t wrong = 0;
            for (const size_t index : order) {
                const minato::mp4::Sample got = sample_of(track, index);
                const minato::mp4::Sample &want = forward[index];
                wrong += got.offset != want.offset || got.size != want.size || got.decode != want.decode ||
                         got.composition_offset != want.composition_offset;
            }
            CHECK(wrong == 0);
        }
    }
}

// Android 15's answers for the test files, from media-extractor-test's reference (tools/android-reference.sh): each
// track's samples in order (time, sync flag and size, from the extractor's lines of that track) and the seeks of a
// track alone (the first sample after each, or the end of the stream).
struct Reference {
    std::vector<std::vector<std::array<int64_t, 3>>> samples;           // per track: time, flags, size
    std::vector<std::array<int64_t, 4>> seeks;                          // mode, time, status, the next sample's time
};

Reference reference_of(const std::string &name, bool alone_track) {
    std::ifstream in(std::string(APKRT_SOURCE_DIR) + "/fixtures/expected/media-extractor-test.logcat");
    Reference reference;
    const std::string walk = "fd " + name + " s";
    const std::string seeks = "fd " + name + (alone_track ? " track 0 alone" : "") + " seek mode ";
    for (std::string line; std::getline(in, line);) {
        const size_t colon = line.find(": ");
        if (colon == std::string::npos) continue;
        const std::string text = line.substr(colon + 2);
        long long track = 0, time = 0, flags = 0, size = 0, mode = 0, to = 0, status = 0;
        int read = 0;
        if (text.compare(0, walk.size(), walk) == 0 &&
            std::sscanf(text.c_str() + walk.size(), "%*d: track %lld time %lld flags %lld size %lld%n", &track, &time,
                        &flags, &size, &read) == 4) {
            const size_t index = static_cast<size_t>(track);
            if (reference.samples.size() <= index) reference.samples.resize(index + 1);
            reference.samples[index].push_back({time, flags, size});
        } else if (text.compare(0, seeks.size(), seeks) == 0 &&
                   std::sscanf(text.c_str() + seeks.size(), "%lld to %lld: status %lld; track %*d time %lld", &mode,
                               &to, &status, &time) == 4 &&
                   to >= 0) {  // a time below 0 takes no seek
            reference.seeks.push_back({mode, to, status, time});
        }
    }
    return reference;
}

// Each track's samples in order, as Android 15 gives them: the time after the edits (below 0 for AAC's priming), the
// sync flag and the size; and the seeks of the first track alone in each mode, the sample reading goes on from.
TEST(the_test_files_read_and_seek_as_android_15s) {
    for (const char *name : {"baseline", "main"}) {
        const Bytes file = file_bytes(kAssets + "media-extractor-test/assets/" + name + ".mp4");
        minato::mp4::Tracks tracks;
        CHECK(minato::mp4::parse(source_of(file), &tracks) == 0);
        const Reference reference = reference_of(name, tracks.size() > 1);
        CHECK(reference.samples.size() == tracks.size() && reference.seeks.size() == 18);
        size_t wrong = 0;
        for (size_t track = 0; track < tracks.size() && track < reference.samples.size(); track++) {
            const auto &want = reference.samples[track];
            CHECK(want.size() == tracks[track].samples.count());
            for (size_t index = 0; index < want.size() && index < tracks[track].samples.count(); index++) {
                const minato::mp4::Sample sample = sample_of(tracks[track], index);
                const int64_t time = minato::mp4::sample_time_us(tracks[track], sample);
                const int64_t flags = tracks[track].samples.is_sync(index) ? 1 : 0;
                wrong += time != want[index][0] || flags != want[index][1] ||
                         minato::mp4::output_size(tracks[track], index) != want[index][2];
            }
        }
        for (const auto &[mode, to, status, time] : reference.seeks) {
            size_t sample = 0;
            const int answer = minato::mp4::seek(tracks[0], to, static_cast<minato::mp4::SeekMode>(mode), &sample);
            const bool ended = status == -10006;  // AMEDIA_ERROR_END_OF_STREAM
            if (ended != (answer == minato::mp4::kEndOfStream) || (!ended && answer != 0)) {
                wrong++;
                std::fprintf(stderr, "  %s seek mode %lld to %lld: %d\n", name, static_cast<long long>(mode),
                             static_cast<long long>(to), answer);
                continue;
            }
            if (ended) continue;
            const int64_t got = minato::mp4::sample_time_us(tracks[0], sample_of(tracks[0], sample));
            if (got != time) {
                wrong++;
                std::fprintf(stderr, "  %s seek mode %lld to %lld: %lld, not %lld\n", name,
                             static_cast<long long>(mode), static_cast<long long>(to), static_cast<long long>(got),
                             static_cast<long long>(time));
            }
        }
        CHECK(wrong == 0);
    }
}

// A file of boxes this test builds: one AVC track of `samples` samples of 16 bytes each (one NAL unit of 12), all in
// one chunk, which the knobs change.
struct Knobs {
    uint32_t samples = 4;
    uint32_t stts_count = 0;      // 0: as many as the samples
    uint32_t stsz_count = 0;      // 0: as many as the samples
    uint32_t stsc_per_chunk = 0;  // 0: as many as the samples
    uint32_t stsz_box_count = 0;  // the count stsz claims, 0: its entries'
    bool largesize = false;       // mdat with a 64-bit size
    bool encrypted = false;       // encv instead of avc1
    bool fragmented = false;      // a moof before the mdat, as a fragmented file has it
    bool chunk_past_end = false;  // the chunk's offset past the file
    uint32_t nal_length = 12;     // the length the first sample's NAL unit claims
    uint32_t first_size = 16;     // the size stsz gives the first sample
    int depth = 0;                // udta boxes nested inside the moov
    std::string repeat;           // a sample table given twice: stsz (as another table), stco, stsc, stts, ctts, stss
    uint32_t timescale = 30000;   // mdhd's
    bool mdhd = true, avcc = true, stsz = true;
    uint32_t constant_count = 0;  // not 0: that many samples of 1 byte in one chunk, stsz's constant size
    int tracks = 1;               // the same trak, this many times
    Bytes elst;                   // not empty: an edts with this elst body
    uint32_t movie_timescale = 0;  // mvhd's
    uint64_t chunk_offset64 = 0;  // not 0: a co64 in place of the stco, with this offset
    std::vector<uint32_t> syncs;  // not empty: stss's entries, in this order
    std::vector<std::array<uint32_t, 2>> time_runs;  // not empty: stts's runs, count and delta
    uint32_t ctts_count = 0;      // not 0: ctts covers this many samples, each 2000 ticks later than decoded
    uint8_t stz2_field = 0;       // not 0: an stz2 of this field size in place of the stsz
    uint16_t language = 0x55c4;   // mdhd's packed ISO 639-2/T code ("und")
    int rotation = -1;            // not -1: tkhd's matrix for this rotation, with a display of 640 x 360
    int tkhd_extra = 0;           // bytes after tkhd's fields
    uint8_t tkhd_version = 0;
    uint32_t last_size = 16;      // the size stsz gives the last sample
    Bytes first_sample;           // not empty: the first sample's bytes (and its size), in place of a 12-byte unit
    uint8_t nal_size = 4;         // avcC's NAL length size: 1, 2 or 4
    int64_t mdhd_duration = -1;   // not -1: mdhd's duration (all its bits in a version 1 mdhd)
    uint8_t mdhd_version = 0;
    bool mvhd_last = false;       // the mvhd after the traks
    Bytes audio_config;           // not empty: an AAC track of this AudioSpecificConfig in place of the AVC track
    std::vector<int32_t> ctts_offsets;  // not empty: ctts's offsets, one run a sample
    std::vector<std::array<uint32_t, 2>> ctts_runs;  // not empty: ctts's runs, count and offset
    std::vector<std::vector<std::string>> stsds;  // not empty: these stsd boxes, of these entries ("avc1", "mp4a")
    uint32_t stsd_claim = 0;      // not 0: the entry count each stsd claims
    uint32_t stsd_flags = 0;      // each stsd's version and flags
    bool stsd_tail = false;       // 4 bytes in each stsd after its entries
    bool avcc_twice = false;      // a second avcC in the avc1 entry, of another SPS
    Bytes avcc_body;              // not empty: the avcC's body
    uint32_t hvcc_boxes = 1;      // hvcC boxes in an hvc1 entry
    uint16_t hvcc_empty_units = 0;  // not 0: each hvcC's one array is this many units of no bytes
    std::vector<Bytes> hvcc_units;  // not empty: the hvcC's one array's units (NAL header first) in place of its VPS
    std::vector<Bytes> entry_boxes;  // boxes after the avcC, hvcC or esds of the avc1, hvc1 and mp4a entries
    Bytes hvcc_whole;               // not empty: the hvcC's body in place of the one built
    Bytes raw_entry;                // the sample entry named "raw", whole
};

void put32(Bytes &out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(value >> shift));
}
void put16(Bytes &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}
Bytes box(const char *type, const Bytes &body) {
    Bytes out;
    put32(out, static_cast<uint32_t>(body.size() + 8));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
Bytes full(uint32_t version_flags) {
    Bytes out;
    put32(out, version_flags);
    return out;
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes &part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

Bytes built_file(const Knobs &knobs, uint64_t *mdat_data_offset = nullptr) {
    const uint32_t count = knobs.samples;
    Bytes mdat_body;
    for (uint32_t index = 0; index < count; index++) {
        if (index == 0 && !knobs.first_sample.empty()) {
            mdat_body.insert(mdat_body.end(), knobs.first_sample.begin(), knobs.first_sample.end());
            continue;
        }
        put32(mdat_body, index == 0 ? knobs.nal_length : 12);
        mdat_body.push_back(0x65);  // an IDR slice's header
        for (uint32_t byte = 0; byte < 11; byte++) mdat_body.push_back(static_cast<uint8_t>(index * 16 + byte));
    }
    const Bytes ftyp = box("ftyp", cat({{'i', 's', 'o', 'm'}, {0, 0, 2, 0}, {'i', 's', 'o', 'm'}}));
    // avcC: version 1, profile 66, compat 0, level 30, length size 4, one SPS and one PPS.
    const uint8_t length_size = static_cast<uint8_t>(0xfc | (knobs.nal_size - 1));
    const Bytes avcc = box("avcC", !knobs.avcc_body.empty() ? knobs.avcc_body
                                                             : Bytes{1, 66, 0, 30, length_size, 0xe1, 0, 4, 0x67, 66, 0,
                                                                     30, 1, 0, 2, 0x68, 0xce});
    Bytes entry_body(6, 0);
    put16(entry_body, 1);                       // data reference index
    entry_body.insert(entry_body.end(), 16, 0);  // pre_defined, reserved
    put16(entry_body, 320);
    put16(entry_body, 180);
    put32(entry_body, 0x00480000);
    put32(entry_body, 0x00480000);
    put32(entry_body, 0);
    put16(entry_body, 1);
    entry_body.insert(entry_body.end(), 32, 0);
    put16(entry_body, 0x18);
    put16(entry_body, 0xffff);
    Bytes visual = knobs.avcc ? cat({entry_body, avcc}) : entry_body;
    if (knobs.avcc_twice) {
        visual = cat({visual, box("avcC", {1, 77, 0, 30, length_size, 0xe1, 0, 4, 0x67, 77, 0, 30, 1, 0, 2, 0x68,
                                           0xee})});
    }
    if (knobs.encrypted) visual = cat({visual, box("sinf", box("frma", {'a', 'v', 'c', '1'}))});
    Bytes extra_boxes;
    for (const Bytes &extra : knobs.entry_boxes) extra_boxes = cat({extra_boxes, extra});
    visual = cat({visual, extra_boxes});
    // mp4a: version 0, 2 channels of 16 bits at 44,100 a second, and an esds whose DecoderSpecificInfo is the config.
    Bytes sound(6, 0);
    put16(sound, 1);
    sound.insert(sound.end(), 8, 0);
    put16(sound, 2);
    put16(sound, 16);
    put32(sound, 0);
    put32(sound, 44100u << 16);
    const Bytes &config = knobs.audio_config;
    const Bytes &esds_config = config.empty() ? Bytes{0x12, 0x10} : config;
    Bytes decoder = {0x40, 0x15, 0, 0, 0};
    put32(decoder, 128000);
    put32(decoder, 96000);
    decoder.insert(decoder.end(), {5, static_cast<uint8_t>(esds_config.size())});
    decoder.insert(decoder.end(), esds_config.begin(), esds_config.end());
    Bytes descriptor = {0, 1, 0, 4, static_cast<uint8_t>(decoder.size())};
    descriptor.insert(descriptor.end(), decoder.begin(), decoder.end());
    Bytes esds = full(0);
    esds.insert(esds.end(), {3, static_cast<uint8_t>(descriptor.size())});
    esds.insert(esds.end(), descriptor.begin(), descriptor.end());
    // The same esds with bit rates of 0 (its DecoderConfigDescriptor's max and average after the 5 bytes before them).
    Bytes esds_without_rates = esds;
    std::fill(esds_without_rates.begin() + 16, esds_without_rates.begin() + 24, 0);
    Bytes esds_of_int32_max = esds;  // and of INT32_MAX, which MPEG4Extractor leaves out
    Bytes esds_of_mp3 = esds;  // and of MP3's object type
    esds_of_mp3[11] = 0x6b;
    Bytes esds_of_tag_5 = esds;  // and with an ES_Descriptor tag of 5, which ESDS refuses
    esds_of_tag_5[4] = 5;
    for (int i : {16, 20}) std::copy_n(Bytes{0x7f, 0xff, 0xff, 0xff}.begin(), 4, esds_of_int32_max.begin() + i);
    // And one whose DecoderSpecificInfo is a byte more than kMaxCodecData, each length in 4 bytes.
    const auto put_length = [](Bytes &bytes, size_t length) {
        for (int shift : {21, 14, 7}) bytes.push_back(static_cast<uint8_t>(0x80 | (length >> shift & 0x7f)));
        bytes.push_back(static_cast<uint8_t>(length & 0x7f));
    };
    Bytes long_decoder(decoder.begin(), decoder.begin() + 13);
    long_decoder.push_back(5);
    put_length(long_decoder, minato::mp4::kMaxCodecData + 1);
    long_decoder.insert(long_decoder.end(), minato::mp4::kMaxCodecData + 1, 0x12);
    Bytes long_descriptor = {0, 1, 0, 4};
    put_length(long_descriptor, long_decoder.size());
    long_descriptor.insert(long_descriptor.end(), long_decoder.begin(), long_decoder.end());
    Bytes long_esds = full(0);
    long_esds.push_back(3);
    put_length(long_esds, long_descriptor.size());
    long_esds.insert(long_esds.end(), long_descriptor.begin(), long_descriptor.end());
    const Bytes video_entry = box(knobs.encrypted ? "encv" : "avc1", visual);
    const Bytes audio_entry = box("mp4a", cat({sound, box("esds", esds), extra_boxes}));
    // hvcC: version 1, Main profile, level 93, NAL length size 4, one array of a VPS (or of units of no bytes).
    Bytes hvcc_body = {1, 1, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 93, 0xf0, 0, 0xfc, 0xfd, 0xf8, 0xf8, 0, 0, 0x0f, 1, 0x20};
    const uint16_t units = knobs.hvcc_empty_units != 0 ? knobs.hvcc_empty_units
                           : knobs.hvcc_units.empty()  ? 1
                                                       : static_cast<uint16_t>(knobs.hvcc_units.size());
    put16(hvcc_body, units);
    for (uint16_t unit = 0; unit < units; unit++) {
        if (knobs.hvcc_empty_units != 0) {
            put16(hvcc_body, 0);
        } else if (!knobs.hvcc_units.empty()) {
            put16(hvcc_body, static_cast<uint16_t>(knobs.hvcc_units[unit].size()));
            hvcc_body.insert(hvcc_body.end(), knobs.hvcc_units[unit].begin(), knobs.hvcc_units[unit].end());
        } else {
            hvcc_body.insert(hvcc_body.end(), {0, 2, 0x40, 1});
        }
    }
    Bytes hevc_visual = entry_body;
    const Bytes hvcc = box("hvcC", knobs.hvcc_whole.empty() ? hvcc_body : knobs.hvcc_whole);
    for (uint32_t index = 0; index < knobs.hvcc_boxes; index++) {
        hevc_visual.insert(hevc_visual.end(), hvcc.begin(), hvcc.end());
    }
    hevc_visual = cat({hevc_visual, extra_boxes});
    // An esds whose DecoderConfigDescriptor is followed by a descriptor of tag 6, not a DecoderSpecificInfo.
    Bytes odd_decoder = {0x40, 0x15, 0, 0, 0};
    put32(odd_decoder, 128000);
    put32(odd_decoder, 96000);
    odd_decoder.insert(odd_decoder.end(), {6, 1, 2});
    Bytes odd_esds = full(0);
    odd_esds.insert(odd_esds.end(), {3, static_cast<uint8_t>(odd_decoder.size() + 5), 0, 1, 0, 4,
                                     static_cast<uint8_t>(odd_decoder.size())});
    odd_esds.insert(odd_esds.end(), odd_decoder.begin(), odd_decoder.end());
    // An stsd entry by name: avc1, hvc1 and mp4a with their avcC, hvcC and esds, without them after a "-"; mp4v and
    // Opus with the avcC after a "*"; avc1 with an esds of version and flags alone after it ("avc1+e") and mp4v with
    // the AAC esds ("mp4v+e"); mp4a with its esds in a QuickTime wave ("mp4a~", "mp4a~+" with the entry's boxes in
    // the wave, "mp4a~5" of an ES_Descriptor tag of 5), with one of a tag 6 after its config ("mp4a!") or of bit
    // rates 0 ("mp4a0"), INT32_MAX ("mp4aM") or MP3 ("mp4a3"), or of a DecoderSpecificInfo past kMaxCodecData, in it
    // ("mp4aL") or a wave ("mp4a~L"); avc1 and hvc1 with a configuration of no parameter sets ("avc1/0", "hvc1/0");
    // Opus with a dOps ("Opus+", of another size or version after it) and alac with its cookie ("alac+", "alac+35",
    // "alac+x"); tx3g and others with none.
    const auto sample_entry = [&](const std::string &name) {
        if (name == "raw") return knobs.raw_entry;
        if (name == "avc1") return video_entry;
        if (name == "mp4a") return audio_entry;
        if (name == "hvc1") return box("hvc1", hevc_visual);
        if (name == "mp4v*") return box("mp4v", cat({entry_body, avcc}));
        if (name == "Opus*") return box("Opus", cat({sound, avcc}));
        if (name.rfind("Opus+", 0) == 0) {
            // A dOps: version 0 (1 for "Opus+v1"), 2 channels, a pre-skip of 312, 48000 Hz, gain 0, mapping family 0;
            // of 18, 276 or 277 bytes for "Opus+18" and the others; in a sound entry of version 1 ("Opus+q"); of a
            // gain of 0x0102 and two SPSs after it, the second ending in a start code ("Opus+s"); of version 2
            // ("Opus+w"), whose 36 more bytes the dOps does not have.
            Bytes body = {static_cast<uint8_t>(name == "Opus+v1"), 2, 0x01, 0x38, 0, 0, 0xbb, 0x80, 0, 0, 0};
            const bool sized = name.size() > 5 && isdigit(static_cast<unsigned char>(name[5]));
            body.resize((sized ? std::stoul(name.substr(5)) : 19) - 8);
            if (name == "Opus+s") {
                body[8] = 1;  // the gain, after the version, channels, pre-skip and rate
                body[9] = 2;
                body.insert(body.end(), {0, 0, 0, 1, 0x67, 0x64, 0, 0x1f, 0xaa, 0, 0, 0, 1, 0x67, 0x42, 0, 0x1e, 0xbb,
                                         0, 0, 0, 1});
            }
            Bytes head = sound;
            if (name == "Opus+q") head[9] = 1;
            if (name == "Opus+w") head[9] = 2;
            return box("Opus", cat({head, box("dOps", body)}));
        }
        if (name.rfind("alac+", 0) == 0) {
            // An alac box of version and flags 0 and a cookie of 4096 frames, 16 bits, 2 channels at 44100 Hz; its size
            // 35 ("alac+35", one byte short), claiming 40 ("alac+40"), of type alaC ("alac+t") or flags 1 ("alac+f"),
            // or an extra byte after it ("alac+x").
            Bytes cookie = {0, 0, 0, 0, 0, 0, 0x10, 0, 0, 16, 40, 10, 14, 2, 0, 255, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0xac, 0x44};
            if (name == "alac+35") cookie.pop_back();
            if (name == "alac+f") cookie[3] = 1;
            Bytes inner = box(name == "alac+t" ? "alaC" : "alac", cookie);
            if (name == "alac+40") inner[3] = 40;
            Bytes entry = cat({sound, inner});
            if (name == "alac+x") entry.push_back(0);
            return box("alac", entry);
        }
        if (name == "avc1+e") return box("avc1", cat({entry_body, avcc, box("esds", full(0))}));
        if (name == "mp4v+e") return box("mp4v", cat({entry_body, box("esds", esds)}));
        if (name == "mp4a~") {
            return box("mp4a", cat({sound, box("wave", cat({box("frma", {'m', 'p', '4', 'a'}), box("esds", esds)}))}));
        }
        if (name == "mp4a~+") {  // the entry's boxes inside the wave
            return box("mp4a", cat({sound, box("wave", cat({box("frma", {'m', 'p', '4', 'a'}), box("esds", esds),
                                                              extra_boxes}))}));
        }
        if (name == "mp4a~5") {
            return box("mp4a", cat({sound, box("wave", cat({box("frma", {'m', 'p', '4', 'a'}),
                                                              box("esds", esds_of_tag_5)})),
                                    extra_boxes}));
        }
        if (name == "mp4a~L") {
            return box("mp4a",
                       cat({sound, box("wave", cat({box("frma", {'m', 'p', '4', 'a'}), box("esds", long_esds)}))}));
        }
        if (name == "mp4aL") return box("mp4a", cat({sound, box("esds", long_esds)}));
        if (name == "mp4a!") return box("mp4a", cat({sound, box("esds", odd_esds)}));
        if (name == "mp4a0") return box("mp4a", cat({sound, box("esds", esds_without_rates)}));
        if (name == "mp4aM") return box("mp4a", cat({sound, box("esds", esds_of_int32_max)}));
        if (name == "mp4a3") return box("mp4a", cat({sound, box("esds", esds_of_mp3)}));
        if (name == "avc1/0") return box("avc1", cat({entry_body, box("avcC", {1, 66, 0, 30, length_size, 0xe0, 0})}));
        if (name == "hvc1/0") {
            Bytes empty(hvcc_body.begin(), hvcc_body.begin() + 22);
            empty.push_back(0);
            return box("hvc1", cat({entry_body, box("hvcC", empty)}));
        }
        const std::string type = name.substr(0, 4);
        const bool sound_entry = type == "mp4a" || type == "Opus" || type == "samr" || type == "ac-3";
        return box(type.c_str(), sound_entry ? sound : entry_body);
    };
    Bytes stsd = box("stsd", cat({full(0), {0, 0, 0, 1}, config.empty() ? video_entry : audio_entry}));
    if (!knobs.stsds.empty()) {
        stsd.clear();
        for (const std::vector<std::string> &names : knobs.stsds) {
            Bytes body = full(knobs.stsd_flags);
            put32(body, knobs.stsd_claim != 0 ? knobs.stsd_claim : static_cast<uint32_t>(names.size()));
            for (const std::string &name : names) body = cat({body, sample_entry(name)});
            if (knobs.stsd_tail) put32(body, 0);
            stsd = cat({stsd, box("stsd", body)});
        }
    }
    const uint32_t constant = knobs.constant_count;
    const uint32_t first =
        knobs.first_sample.empty() ? knobs.first_size : static_cast<uint32_t>(knobs.first_sample.size());
    Bytes stts = full(0);
    if (knobs.time_runs.empty()) {
        put32(stts, 1);
        put32(stts, constant != 0 ? constant : knobs.stts_count != 0 ? knobs.stts_count : count);
        put32(stts, 1000);
    } else {
        put32(stts, static_cast<uint32_t>(knobs.time_runs.size()));
        for (const auto &run : knobs.time_runs) {
            put32(stts, run[0]);
            put32(stts, run[1]);
        }
    }
    Bytes stsc = full(0);
    put32(stsc, 1);
    put32(stsc, 1);
    put32(stsc, constant != 0 ? constant : knobs.stsc_per_chunk != 0 ? knobs.stsc_per_chunk : count);
    put32(stsc, 1);
    Bytes stsz = full(0);
    if (constant != 0) {
        put32(stsz, 1);
        put32(stsz, constant);
    } else {
        put32(stsz, 0);
        const uint32_t entries = knobs.stsz_count != 0 ? knobs.stsz_count : count;
        put32(stsz, knobs.stsz_box_count != 0 ? knobs.stsz_box_count : entries);
        for (uint32_t index = 0; index < entries; index++) {
            put32(stsz, index == 0 ? first : index + 1 == entries ? knobs.last_size : 16);
        }
    }
    const uint64_t header = knobs.largesize ? 16 : 8;
    // The moov comes after the mdat, so the chunk's offset is known: ftyp, then mdat's header.
    const uint64_t data = ftyp.size() + header;
    if (mdat_data_offset != nullptr) *mdat_data_offset = data;
    Bytes stco = full(0);
    put32(stco, 1);
    if (knobs.chunk_offset64 != 0) {
        put32(stco, static_cast<uint32_t>(knobs.chunk_offset64 >> 32));
        put32(stco, static_cast<uint32_t>(knobs.chunk_offset64));
    } else {
        put32(stco, knobs.chunk_past_end ? 0x7fffff00u : static_cast<uint32_t>(data));
    }
    Bytes ctts = full(0);
    put32(ctts, 1);
    put32(ctts, knobs.ctts_count != 0 ? knobs.ctts_count : count);
    put32(ctts, knobs.ctts_count != 0 ? 2000 : 0);
    // stz2: the same sizes as stsz's, in fields of 4, 8 or 16 bits (4 bits hold at most 15).
    Bytes stz2 = full(0);
    stz2.insert(stz2.end(), {0, 0, 0, knobs.stz2_field});
    put32(stz2, count);
    for (uint32_t index = 0; index < count; index++) {
        const uint32_t size = index == 0 ? knobs.first_size : 16;
        if (knobs.stz2_field == 16) put16(stz2, static_cast<uint16_t>(size));
        if (knobs.stz2_field == 8) stz2.push_back(static_cast<uint8_t>(size));
        if (knobs.stz2_field == 4 && index % 2 == 0) stz2.push_back(static_cast<uint8_t>(size << 4));
        if (knobs.stz2_field == 4 && index % 2 == 1) stz2.back() |= static_cast<uint8_t>(size & 0xf);
    }
    Bytes stss = full(0);
    put32(stss, 1);
    put32(stss, 1);
    Bytes syncs = full(0);
    put32(syncs, static_cast<uint32_t>(knobs.syncs.size()));
    for (const uint32_t sync : knobs.syncs) put32(syncs, sync);
    // A second stsz of another kind than the first: a constant size over many samples, past the first's entries.
    Bytes other_stsz = full(0);
    put32(other_stsz, 1);
    put32(other_stsz, 100000);
    Bytes tables = cat({stsd, box("stts", stts), box("stsc", stsc)});
    if (knobs.stsz) tables = cat({tables, knobs.stz2_field != 0 ? box("stz2", stz2) : box("stsz", stsz)});
    if (knobs.ctts_count != 0) tables = cat({tables, box("ctts", ctts)});
    if (!knobs.ctts_runs.empty()) {
        Bytes runs = full(0);
        put32(runs, static_cast<uint32_t>(knobs.ctts_runs.size()));
        for (const auto &run : knobs.ctts_runs) {
            put32(runs, run[0]);
            put32(runs, run[1]);
        }
        tables = cat({tables, box("ctts", runs)});
    }
    if (!knobs.ctts_offsets.empty()) {
        Bytes runs = full(0);
        put32(runs, static_cast<uint32_t>(knobs.ctts_offsets.size()));
        for (const int32_t offset : knobs.ctts_offsets) {
            put32(runs, 1);
            put32(runs, static_cast<uint32_t>(offset));
        }
        tables = cat({tables, box("ctts", runs)});
    }
    tables = cat({tables, box(knobs.chunk_offset64 != 0 ? "co64" : "stco", stco)});
    if (!knobs.syncs.empty()) tables = cat({tables, box("stss", syncs)});
    if (knobs.repeat == "stsz") tables = cat({tables, box("stsz", other_stsz)});
    if (knobs.repeat == "stco") tables = cat({tables, box("stco", stco)});
    if (knobs.repeat == "stsc") tables = cat({tables, box("stsc", stsc)});
    if (knobs.repeat == "stts") tables = cat({tables, box("stts", stts)});
    if (knobs.repeat == "ctts") tables = cat({tables, box("ctts", ctts), box("ctts", ctts)});
    if (knobs.repeat == "stss") tables = cat({tables, box("stss", stss), box("stss", stss)});
    const Bytes stbl = box("stbl", tables);
    Bytes mdhd = full(static_cast<uint32_t>(knobs.mdhd_version) << 24);
    const uint64_t duration = knobs.mdhd_duration != -1 ? static_cast<uint64_t>(knobs.mdhd_duration) : count * 1000u;
    mdhd.insert(mdhd.end(), knobs.mdhd_version == 1 ? 16 : 8, 0);
    put32(mdhd, knobs.timescale);
    if (knobs.mdhd_version == 1) put32(mdhd, static_cast<uint32_t>(duration >> 32));
    put32(mdhd, static_cast<uint32_t>(duration));
    put16(mdhd, knobs.language);
    put16(mdhd, 0);
    Bytes hdlr = full(0);
    put32(hdlr, 0);
    if (config.empty()) {
        hdlr.insert(hdlr.end(), {'v', 'i', 'd', 'e'});
    } else {
        hdlr.insert(hdlr.end(), {'s', 'o', 'u', 'n'});
    }
    hdlr.insert(hdlr.end(), 13, 0);
    const Bytes minf = box("minf", cat({box("vmhd", cat({full(1), Bytes(8, 0)})), stbl}));
    const Bytes mdia = box("mdia", knobs.mdhd ? cat({box("mdhd", mdhd), box("hdlr", hdlr), minf})
                                              : cat({box("hdlr", hdlr), minf}));
    Bytes tkhd = full(static_cast<uint32_t>(knobs.tkhd_version) << 24 | 3);
    tkhd.insert(tkhd.end(), 8, 0);
    put32(tkhd, 1);  // track ID
    if (knobs.rotation < 0) {
        tkhd.insert(tkhd.end(), 68, 0);
    } else {
        tkhd.insert(tkhd.end(), 24, 0);  // reserved, duration, reserved, layer, group, volume, reserved
        const int32_t one = 0x10000, c = knobs.rotation == 0 ? one : knobs.rotation == 180 ? -one : 0;
        const int32_t s = knobs.rotation == 90 ? one : knobs.rotation == 270 ? -one : 0;
        for (const int32_t value : {c, s, 0, -s, c, 0, 0, 0, 0x40000000}) put32(tkhd, static_cast<uint32_t>(value));
        put32(tkhd, 640u << 16);
        put32(tkhd, 360u << 16);
    }
    tkhd.insert(tkhd.end(), static_cast<size_t>(knobs.tkhd_extra), 0);
    Bytes nested;
    for (int level = 0; level < knobs.depth; level++) nested = box("udta", nested);
    Bytes mvhd = full(0);
    mvhd.insert(mvhd.end(), 8, 0);  // creation and modification times
    put32(mvhd, knobs.movie_timescale);
    mvhd.insert(mvhd.end(), 84, 0);
    const Bytes edts = knobs.elst.empty() ? Bytes{} : box("edts", box("elst", knobs.elst));
    const Bytes trak = box("trak", cat({box("tkhd", tkhd), edts, mdia}));
    Bytes moov_body = knobs.mvhd_last ? Bytes{} : box("mvhd", mvhd);
    for (int track = 0; track < knobs.tracks; track++) moov_body = cat({moov_body, trak});
    if (knobs.mvhd_last) moov_body = cat({moov_body, box("mvhd", mvhd)});
    const Bytes moov = box("moov", cat({moov_body, nested}));
    Bytes mdat;
    if (knobs.largesize) {
        put32(mdat, 1);
        mdat.insert(mdat.end(), {'m', 'd', 'a', 't'});
        put32(mdat, 0);
        put32(mdat, static_cast<uint32_t>(mdat_body.size() + 16));
        mdat.insert(mdat.end(), mdat_body.begin(), mdat_body.end());
    } else {
        mdat = box("mdat", mdat_body);
    }
    const Bytes moof = knobs.fragmented ? box("moof", box("mfhd", cat({full(0), {0, 0, 0, 1}}))) : Bytes{};
    return cat({ftyp, moof, mdat, moov});
}

int parsed(const Bytes &file, minato::mp4::Tracks *tracks) {
    return minato::mp4::parse(source_of(file), tracks);
}

TEST(a_built_file_parses_and_reads) {
    for (const bool largesize : {false, true}) {
        Knobs knobs;
        knobs.largesize = largesize;
        const Bytes file = built_file(knobs);
        minato::mp4::Tracks tracks;
        CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
        if (tracks.size() != 1) continue;
        CHECK(tracks[0].samples.count() == 4 && bytes(tracks[0].csd0) == (Bytes{0, 0, 0, 1, 0x67, 66, 0, 30}));
        long answer = 0;
        const Bytes got = read_all(source_of(file), tracks[0], 1, &answer);
        CHECK(answer == 16 && got[3] == 1 && got[4] == 0x65 && got[5] == 16);
    }
}

TEST(sample_tables_that_disagree_end_the_track_where_one_ends) {
    Knobs short_stts;
    short_stts.stts_count = 2;
    Knobs short_stsz;
    short_stsz.stsz_count = 3;
    Knobs short_chunk;
    short_chunk.stsc_per_chunk = 1;  // one chunk of one sample
    for (const Knobs &knobs : {short_stts, short_stsz, short_chunk}) {
        minato::mp4::Tracks tracks;
        const int answer = parsed(built_file(knobs), &tracks);
        CHECK(answer == 0 && tracks.size() == 1);
        if (tracks.size() == 1) CHECK(tracks[0].samples.count() < 4);
    }
}

TEST(counts_a_box_cannot_hold_and_sizes_past_the_end_are_malformed) {
    Knobs huge;
    huge.stsz_box_count = 0xffffffffu;
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(huge), &tracks) == minato::mp4::kMalformed);
    Bytes file = built_file(Knobs{});
    // The moov's size past the end of the file.
    const Bytes type{'m', 'o', 'o', 'v'};
    const auto moov = std::search(file.begin(), file.end(), type.begin(), type.end()) - 4;
    *moov = 0x7f;
    CHECK(parsed(file, &tracks) == minato::mp4::kMalformed);
    Knobs deep;
    deep.depth = minato::mp4::kMaxDepth + 4;
    CHECK(parsed(built_file(deep), &tracks) == minato::mp4::kMalformed);
}

TEST(truncated_files_end_with_an_error_or_fewer_samples) {
    const Bytes whole = built_file(Knobs{});
    for (size_t length = 0; length < whole.size(); length++) {
        const Bytes cut(whole.begin(), whole.begin() + static_cast<long>(length));
        minato::mp4::Tracks tracks;
        const int answer = parsed(cut, &tracks);
        CHECK(answer == minato::mp4::kMalformed || answer == 0);
    }
}

TEST(reads_past_the_file_or_the_sample_are_refused) {
    Knobs past;
    past.chunk_past_end = true;
    minato::mp4::Tracks tracks;
    const Bytes file = built_file(past);
    CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        long answer = 0;
        read_all(source_of(file), tracks[0], 0, &answer);
        CHECK(answer == minato::mp4::kMalformed || answer == minato::mp4::kIo);
    }
    Knobs long_nal;
    long_nal.nal_length = 13;  // one byte past the sample: MPEG4Source drops the unit
    const Bytes other = built_file(long_nal);
    CHECK(parsed(other, &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        long answer = 1;
        read_all(source_of(other), tracks[0], 0, &answer);
        CHECK(answer == 0);
        Bytes small(4);
        CHECK(minato::mp4::read_sample(source_of(other), tracks[0], 1, small.data(), small.size()) ==
              minato::mp4::kNoMemory);
        CHECK(minato::mp4::output_size(tracks[0], 99) < 0);
    }
}

TEST(a_track_with_a_sample_larger_than_androids_input_buffer_is_refused) {
    // MPEG4Source::start refuses a track whose largest sample plus 20 bytes is above 64 MiB, so a size field
    // never decides how much a reader allocates.
    minato::mp4::Tracks tracks;
    Knobs largest;
    largest.first_size = static_cast<uint32_t>(minato::mp4::kMaxInputBytes - 20);
    CHECK(parsed(built_file(largest), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(minato::mp4::output_size(tracks[0], 0) == static_cast<long>(largest.first_size));
    Knobs larger;
    larger.first_size = largest.first_size + 1;
    const Bytes file = built_file(larger);
    CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        CHECK(minato::mp4::output_size(tracks[0], 0) == minato::mp4::kMalformed);
        CHECK(minato::mp4::output_size(tracks[0], 1) == minato::mp4::kMalformed);
        Bytes out(64);
        CHECK(minato::mp4::read_sample(source_of(file), tracks[0], 1, out.data(), out.size()) ==
              minato::mp4::kMalformed);
    }
    // The sweep's case: baseline.mp4's first stsz entry with its high byte set, a sample of about 4 GB.
    Bytes changed = file_bytes(kAssets + "media-extractor-test/assets/baseline.mp4");
    const char stsz[] = {'s', 't', 's', 'z'};
    const auto at = std::search(changed.begin(), changed.end(), stsz, stsz + 4);
    CHECK(at != changed.end() && at + 16 < changed.end());
    if (at != changed.end() && at + 16 < changed.end()) {
        at[16] = 0xff;
        CHECK(parsed(changed, &tracks) == 0 && tracks.size() == 2);
        if (!tracks.empty()) CHECK(minato::mp4::output_size(tracks[0], 0) == minato::mp4::kMalformed);
    }
}

// As Android's SampleTable, whose setters refuse a second table of the same kind (ERROR_MALFORMED, which ends the
// whole file): a second stsz of another kind would otherwise leave sizes for fewer samples than the count.
TEST(a_sample_table_given_twice_makes_the_file_malformed) {
    for (const char *table : {"stsz", "stco", "stsc", "stts", "ctts", "stss"}) {
        Knobs knobs;
        knobs.repeat = table;
        minato::mp4::Tracks tracks;
        const int answer = parsed(built_file(knobs), &tracks);
        CHECK(answer == minato::mp4::kMalformed);
        if (answer != minato::mp4::kMalformed) std::fprintf(stderr, "  %s twice: %d\n", table, answer);
    }
}

// As Android's verifyTrack: a track without a timescale (none, or no mdhd), without its sample tables or without
// its codec configuration is left out, and the file still parses.
// (A timescale of 0 in an mdhd makes the file malformed: a_media_header_reads_as_androids.)
TEST(a_track_without_a_timescale_tables_or_configuration_is_left_out) {
    Knobs no_mdhd;
    no_mdhd.mdhd = false;
    Knobs no_avcc;
    no_avcc.avcc = false;
    Knobs no_stsz;
    no_stsz.stsz = false;
    Knobs two;
    two.tracks = 2;
    two.mdhd = false;
    for (const Knobs &knobs : {no_mdhd, no_avcc, no_stsz, two}) {
        minato::mp4::Tracks tracks;
        CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.empty());
    }
}

ssize_t read_padded(void *context, uint64_t offset, void *data, size_t size) {
    const auto *bytes = static_cast<const Bytes *>(context);
    std::memset(data, 0, size);
    if (offset < bytes->size()) {
        std::memcpy(data, bytes->data() + offset, std::min<uint64_t>(size, bytes->size() - offset));
    }
    return static_cast<ssize_t>(size);
}

// More samples than the 2,097,152 that part 1 refused (its kMaxSamples), of 1 byte each in one chunk: the track
// parses and its samples are where and when the tables put them. The source is long enough to hold them all.
TEST(a_track_of_more_samples_than_part_1_took_parses) {
    Knobs knobs;
    knobs.constant_count = 1u << 22;
    uint64_t data = 0;
    const Bytes file = built_file(knobs, &data);
    minato::mp4::Tracks tracks;
    CHECK(minato::mp4::parse(Source{const_cast<Bytes *>(&file), read_padded, 1ull << 40}, &tracks) == 0 &&
          tracks.size() == 1);
    if (tracks.size() != 1) return;
    CHECK(tracks[0].samples.count() == knobs.constant_count);
    for (const size_t index : {size_t{0}, size_t{1} << 21, (size_t{1} << 22) - 1}) {
        if (index >= tracks[0].samples.count()) continue;
        const minato::mp4::Sample &sample = sample_of(tracks[0], index);
        CHECK(sample.offset == data + index && sample.size == 1);
        CHECK(sample.decode == static_cast<int64_t>(index) * 1000);
        CHECK(tracks[0].samples.is_sync(index));  // no stss: every sample
    }
}

// The parse keeps no memory per sample, as Android's SampleTable reads stsz and stco from the file when a sample is
// asked for: the tracks of 2,097,152 samples of a file of about 1 KiB hold far less than one byte a sample.
TEST(parsing_allocates_no_memory_per_sample) {
    Knobs knobs;
    knobs.constant_count = 1u << 21;
    const Bytes file = built_file(knobs);
    minato::mp4::Tracks tracks;
    const size_t before = held_bytes();
    const int answer = minato::mp4::parse(Source{const_cast<Bytes *>(&file), read_padded, 1ull << 40}, &tracks);
    const size_t held = held_bytes() - before;
    CHECK(answer == 0 && tracks.size() == 1);
    CHECK(held < (64u << 10));
    if (held >= (64u << 10)) std::fprintf(stderr, "  held %zu bytes\n", held);
}

// A chunk near the end of the 64-bit range: a sample whose bytes would end past it is malformed when it is asked for,
// as SampleIterator answers it, and the file and its other samples stay.
TEST(a_sample_past_the_64_bit_range_fails_alone) {
    Knobs knobs;
    knobs.chunk_offset64 = ~uint64_t{0} - 39;  // samples of 16 bytes: the third ends past the range
    const Bytes file = built_file(knobs);
    minato::mp4::Tracks tracks;
    CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() != 1) return;
    CHECK(tracks[0].samples.count() == 4 && sample_of(tracks[0], 1).offset == ~uint64_t{0} - 23);
    minato::mp4::Sample sample{};
    CHECK(tracks[0].samples.at(2, &sample) == minato::mp4::kMalformed);
    CHECK(tracks[0].samples.at(3, &sample) == minato::mp4::kMalformed);
    CHECK(tracks[0].samples.at(0, &sample) == 0 && sample.offset == ~uint64_t{0} - 39);
    long answer = 0;
    read_all(source_of(file), tracks[0], 0, &answer);
    CHECK(answer == minato::mp4::kMalformed);  // past the file
}

// A ctts that covers fewer samples than the track gives the rest an offset of 0, as Android's
// CompositionDeltaLookup does past its entries.
TEST(samples_past_the_ctts_have_no_composition_offset) {
    Knobs knobs;
    knobs.ctts_count = 2;
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() != 1) return;
    for (size_t index = 0; index < 4; index++) {
        CHECK(sample_of(tracks[0], index).composition_offset == (index < 2 ? 2000 : 0));
    }
}

// stz2's fields of 16, 8 and 4 bits give the sizes stsz gives, high nibble first, and the samples' places follow from
// them. A 4-bit field holds 15 at most, so there the built file's later sizes of 16 are 0.
TEST(an_stz2_gives_the_sizes_an_stsz_gives) {
    uint64_t data = 0;
    for (const uint8_t field : {uint8_t{16}, uint8_t{8}, uint8_t{4}}) {
        Knobs knobs;
        knobs.stz2_field = field;
        knobs.first_size = field == 16 ? 300 : field == 8 ? 200 : 12;  // the NAL length is checked on a read only
        const Bytes file = built_file(knobs, &data);
        minato::mp4::Tracks tracks;
        CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
        if (tracks.size() != 1) continue;
        const uint32_t later = field == 4 ? 0 : 16;
        CHECK(tracks[0].samples.count() == 4 && tracks[0].largest_sample == std::max(knobs.first_size, later));
        for (size_t index = 0; index < 4; index++) {
            const minato::mp4::Sample sample = sample_of(tracks[0], index);
            CHECK(sample.size == (index == 0 ? knobs.first_size : later));
            CHECK(sample.offset == data + (index == 0 ? 0 : knobs.first_size + (index - 1) * later));
        }
    }
}

// A chunk whose second half ends past the 64-bit range: reading through that half stays linear, as the place of the
// first failing sample is kept and not found again from the chunk's start for each sample after it.
TEST(samples_after_one_past_the_64_bit_range_fail_in_bounded_time) {
    Knobs knobs;
    knobs.samples = 1u << 20;
    knobs.chunk_offset64 = ~uint64_t{0} - 16 * (uint64_t{1} << 19) + 1;  // sample 2^19 - 1 is the first to fail
    const Bytes file = built_file(knobs);
    minato::mp4::Tracks tracks;
    CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() != 1) return;
    // From a sample well past the first that fails, with no sample of the chunk found before.
    minato::mp4::Sample sample{};
    const auto start = std::chrono::steady_clock::now();
    size_t failed = 0;
    for (size_t index = size_t{3} << 18; index < knobs.samples; index++) {
        failed += tracks[0].samples.at(index, &sample) == minato::mp4::kMalformed;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(failed == size_t{1} << 18);
    CHECK(seconds < 5);
    CHECK(tracks[0].samples.at((size_t{1} << 19) - 2, &sample) == 0);
}

// Decode times past int64_t's range: two runs of 2^31 samples, each 2^32 - 1 ticks long. A sample whose time would
// pass it is malformed when it is asked for, and the samples before it stay.
TEST(a_decode_time_past_int64_fails_alone) {
    Knobs knobs;
    knobs.constant_count = 0xffffffffu;
    knobs.time_runs = {{1u << 31, 0xffffffffu}, {1u << 31, 0xffffffffu}};
    const Bytes file = built_file(knobs);
    minato::mp4::Tracks tracks;
    CHECK(minato::mp4::parse(Source{const_cast<Bytes *>(&file), read_padded, 1ull << 40}, &tracks) == 0 &&
          tracks.size() == 1);
    if (tracks.size() != 1) return;
    const uint64_t first_run = (uint64_t{1} << 31) * 0xffffffffu;  // 2^63 - 2^31
    minato::mp4::Sample sample{};
    CHECK(tracks[0].samples.at(size_t{1} << 31, &sample) == 0 && static_cast<uint64_t>(sample.decode) == first_run);
    CHECK(tracks[0].samples.at((size_t{1} << 31) + 1, &sample) == minato::mp4::kMalformed);
    CHECK(tracks[0].samples.at((size_t{1} << 31) - 1, &sample) == 0 &&
          static_cast<uint64_t>(sample.decode) == first_run - 0xffffffffu);
}

// A sample at INT64_MAX's decode time whose composition offset takes it past: its composition time saturates.
TEST(a_composition_time_past_int64_saturates) {
    Knobs knobs;
    knobs.constant_count = 0xffffffffu;
    knobs.time_runs = {{1u << 31, 0xffffffffu}, {1, 0x7fffffffu}, {1, 1}};
    knobs.ctts_runs = {{(1u << 31) + 1, 0}, {1, 1}};
    const Bytes file = built_file(knobs);
    minato::mp4::Tracks tracks;
    CHECK(minato::mp4::parse(Source{const_cast<Bytes *>(&file), read_padded, 1ull << 40}, &tracks) == 0 &&
          tracks.size() == 1);
    if (tracks.size() != 1) return;
    minato::mp4::Sample sample{};
    CHECK(tracks[0].samples.at((size_t{1} << 31) + 1, &sample) == 0 && sample.decode == INT64_MAX &&
          sample.composition_offset == 1);
    CHECK(sample.composition() == INT64_MAX);
}

// What a parse holds is freed when its tracks go or are parsed again, on success and on each way it fails: the test
// files, a table given twice after the avcC's csd was made, an encrypted and a fragmented file, a fragment after the
// moov was read, a track left out for want of its avcC, and an unsorted stss, whose sorted copy the track holds.
TEST(a_parse_holds_nothing_once_its_tracks_go) {
    Knobs twice;
    twice.repeat = "stsz";
    Knobs encrypted;
    encrypted.encrypted = true;
    Knobs fragmented;
    fragmented.fragmented = true;
    Knobs no_avcc;
    no_avcc.avcc = false;
    Knobs unsorted;
    unsorted.syncs = {3, 1};
    // ftyp, moov, then a moof: the walk reads the moov before it meets the fragment.
    const Bytes plain = built_file(Knobs{});
    const Bytes moov_type{'m', 'o', 'o', 'v'};
    const auto moov = std::search(plain.begin(), plain.end(), moov_type.begin(), moov_type.end()) - 4;
    const Bytes moof = box("moof", box("mfhd", cat({full(0), {0, 0, 0, 1}})));
    const Bytes moov_then_moof = cat({Bytes(plain.begin(), plain.begin() + 20), Bytes(moov, plain.end()), moof});
    const std::vector<Bytes> files = {file_bytes(kAssets + "media-extractor-test/assets/baseline.mp4"),
                                      file_bytes(kAssets + "media-extractor-test/assets/main.mp4"),
                                      built_file(twice), built_file(encrypted), built_file(fragmented),
                                      moov_then_moof, built_file(no_avcc), built_file(unsorted)};
    // Once before counting: the C library allocates 16 bytes for good on qsort's first call (the unsorted stss).
    for (const Bytes &file : files) {
        minato::mp4::Tracks tracks;
        parsed(file, &tracks);
    }
    const size_t before = held_bytes();
    {
        minato::mp4::Tracks tracks;
        for (const Bytes &file : files) parsed(file, &tracks);
        CHECK(tracks.size() == 1);  // the last file's
    }
    for (const Bytes &file : files) {
        minato::mp4::Tracks tracks;
        parsed(file, &tracks);
    }
    CHECK(held_bytes() == before);
    minato::mp4::Tracks tracks;
    CHECK(parsed(moov_then_moof, &tracks) == minato::mp4::kUnsupported);
}

// mdhd's language: its packed letters, each plus 0x60 whatever they are, as MPEG4Extractor reads them.
TEST(a_track_language_reads_as_androids) {
    const std::pair<uint16_t, const char *> cases[] = {
        {0x55c4, "und"}, {0, "```"}, {0x7fff, "\x7f\x7f\x7f"}, {0x15c7, "eng"}};
    for (const auto &[code, language] : cases) {
        Knobs knobs;
        knobs.language = code;
        minato::mp4::Tracks tracks;
        CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
        if (tracks.size() == 1) CHECK(text(tracks[0].language) == language);
    }
}

// tkhd as MPEG4Extractor's parseTrackHeader reads it: a size other than its version's is malformed, a version above 1
// unsupported; its matrix gives the rotation, its last two fields the display size.
TEST(a_track_header_reads_as_androids) {
    for (const int rotation : {0, 90, 180, 270}) {
        Knobs knobs;
        knobs.rotation = rotation;
        minato::mp4::Tracks tracks;
        CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
        if (tracks.size() != 1) continue;
        CHECK(tracks[0].rotation == rotation && tracks[0].display_width == 640 && tracks[0].display_height == 360);
    }
    Knobs longer;
    longer.tkhd_extra = 4;
    Knobs later;
    later.tkhd_version = 2;
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(longer), &tracks) == minato::mp4::kMalformed);
    CHECK(parsed(built_file(later), &tracks) == minato::mp4::kUnsupported);
}

// An stss entry of 0 names the first sample, as SampleTable keeps it; the largest size counts all of stsz's sizes,
// past where another table ends the track (getMaxSampleSize).
TEST(stss_zero_and_the_largest_size_read_as_androids) {
    Knobs zero;
    zero.syncs = {0, 3};
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(zero), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        CHECK(tracks[0].samples.is_sync(0) && !tracks[0].samples.is_sync(1) && tracks[0].samples.is_sync(2));
    }
    Knobs shorter;
    shorter.stts_count = 2;
    shorter.last_size = 40;
    CHECK(parsed(built_file(shorter), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        CHECK(tracks[0].samples.count() == 2 && tracks[0].max_size == 40 && tracks[0].frame_count == 4);
    }
}

// Seeks on a built file of 4 samples of 1000 ticks at 30,000 a second (0, 33,333, 66,667 and 100,000 µs as
// SampleTable rounds them), all sync samples: an exact time, the closest, and a time after the last.
TEST(seeks_on_a_built_file_follow_androids_rules) {
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(Knobs{}), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() != 1) return;
    size_t sample = 99;
    CHECK(minato::mp4::seek(tracks[0], 66667, minato::mp4::kSeekNextSync, &sample) == 0 && sample == 2);
    CHECK(minato::mp4::seek(tracks[0], 40000, minato::mp4::kSeekClosestSync, &sample) == 0 && sample == 1);
    CHECK(minato::mp4::seek(tracks[0], 60000, minato::mp4::kSeekClosestSync, &sample) == 0 && sample == 2);
    CHECK(minato::mp4::seek(tracks[0], 200000, minato::mp4::kSeekNextSync, &sample) == minato::mp4::kEndOfStream);
    CHECK(minato::mp4::seek(tracks[0], 200000, minato::mp4::kSeekPreviousSync, &sample) == 0 && sample == 3);
}

// An stss out of order is taken as a set, as part 1 took it, and reading every sample's flag stays linear: 1,048,576
// samples, every other one a sync sample, listed backwards with an entry 0 and one past the count.
TEST(an_unsorted_sync_table_reads_in_bounded_time) {
    Knobs knobs;
    knobs.constant_count = 1u << 20;
    knobs.syncs.push_back(knobs.constant_count + 5);
    for (uint32_t sync = knobs.constant_count - 1; sync >= 1 && sync < knobs.constant_count; sync -= 2) {
        knobs.syncs.push_back(sync);
    }
    knobs.syncs.push_back(0);
    const Bytes file = built_file(knobs);
    minato::mp4::Tracks tracks;
    CHECK(minato::mp4::parse(Source{const_cast<Bytes *>(&file), read_padded, 1ull << 40}, &tracks) == 0 &&
          tracks.size() == 1);
    if (tracks.size() != 1) return;
    const auto start = std::chrono::steady_clock::now();
    size_t wrong = 0;
    for (size_t index = 0; index < knobs.constant_count; index++) {
        wrong += tracks[0].samples.is_sync(index) != (index % 2 == 0);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(wrong == 0);
    CHECK(seconds < 5);
}

// A source that remembers the furthest byte it was asked for.
struct Furthest {
    const Bytes *bytes;
    uint64_t end = 0;
};

ssize_t read_furthest(void *context, uint64_t offset, void *data, size_t size) {
    auto *furthest = static_cast<Furthest *>(context);
    const ssize_t got = read_memory(const_cast<Bytes *>(furthest->bytes), offset, data, size);
    if (got > 0) furthest->end = std::max<uint64_t>(furthest->end, offset + static_cast<uint64_t>(got));
    return got;
}

// As MPEG4Extractor::readMetaData: once an ftyp with a brand (not "qt  ", not only HEIF's or AVIF's), the moov and an
// mdat were seen, the top level is not walked further, so neither a long tail of boxes nor a cut one after them
// matters. Without such an ftyp the walk goes on to the end. An ftyp shorter than its two brand fields is malformed.
TEST(the_top_level_walk_stops_after_the_movie_and_its_data_as_androids_does) {
    const Bytes plain = built_file(Knobs{});
    Bytes tail;
    for (int index = 0; index < 4096; index++) tail = cat({tail, box("free", {})});
    const Bytes cut = {0, 0, 1, 0, 'f', 'r', 'e', 'e'};  // claims 256 bytes, 8 are there
    auto ftyp_of = [](std::initializer_list<const char *> brands) {
        Bytes body;
        for (const char *brand : brands) {
            body.insert(body.end(), brand, brand + 4);
            if (body.size() == 4) put32(body, 0x200);  // the minor version after the major brand
        }
        return box("ftyp", body);
    };
    const size_t ftyp_size = ftyp_of({"isom", "isom"}).size();
    auto with_ftyp = [&](const Bytes &ftyp, const Bytes &after) {
        Bytes file = cat({ftyp, Bytes(plain.begin() + static_cast<long>(ftyp_size), plain.end()), after});
        return file;
    };
    struct Case {
        Bytes ftyp;
        bool stops;
    };
    const Case cases[] = {{ftyp_of({"isom", "isom"}), true},  {ftyp_of({"mp42"}), true},
                          {ftyp_of({"heic", "mif1"}), false}, {ftyp_of({"avif", "avis"}), false},
                          {ftyp_of({"avif", "mif1"}), true},  {ftyp_of({"qt  ", "isom"}), false}};
    for (const Case &one : cases) {
        // The chunk offsets assume the 20-byte ftyp: a shorter or longer one only moves where the samples are.
        const Bytes file = with_ftyp(one.ftyp, tail);
        Furthest furthest{&file};
        minato::mp4::Tracks tracks;
        CHECK(minato::mp4::parse(Source{&furthest, read_furthest, file.size()}, &tracks) == 0 && tracks.size() == 1);
        const uint64_t walked_to = file.size() - tail.size();
        CHECK((furthest.end <= walked_to) == one.stops);
        // After the moov, as after Android's mInitCheck = OK, a cut or wrong box ends the walk, not the file.
        for (const Bytes &after : {cut, Bytes{0, 0, 0, 3, 'f', 'r', 'e', 'e'}, Bytes{0, 0, 0, 0}}) {
            const Bytes ended = with_ftyp(one.ftyp, cat({tail, after}));
            CHECK(parsed(ended, &tracks) == 0 && tracks.size() == 1);
        }
    }
    // No ftyp: the walk goes on.
    const Bytes bare = cat({Bytes(plain.begin() + static_cast<long>(ftyp_size), plain.end()), tail});
    Furthest furthest{&bare};
    minato::mp4::Tracks tracks;
    CHECK(minato::mp4::parse(Source{&furthest, read_furthest, bare.size()}, &tracks) == 0 &&
          furthest.end == bare.size());
    CHECK(parsed(cat({bare, cut}), &tracks) == 0 && tracks.size() == 1);
    // Before the moov the same box fails the file.
    CHECK(parsed(cat({cut, bare}), &tracks) == minato::mp4::kMalformed);
    const Bytes short_ftyp = with_ftyp(box("ftyp", {'i', 's', 'o', 'm'}), {});
    CHECK(parsed(short_ftyp, &tracks) == minato::mp4::kMalformed);
}

// A source of zeros but for pieces of bytes at their offsets, to place boxes past 64 MiB without the memory.
struct Sparse {
    std::vector<std::pair<uint64_t, Bytes>> pieces;
};

ssize_t read_sparse(void *context, uint64_t offset, void *data, size_t size) {
    const auto *sparse = static_cast<const Sparse *>(context);
    auto *out = static_cast<uint8_t *>(data);
    std::memset(out, 0, size);
    for (const auto &[at, bytes] : sparse->pieces) {
        const uint64_t begin = std::max(at, offset), end = std::min<uint64_t>(at + bytes.size(), offset + size);
        if (begin < end) std::memcpy(out + (begin - offset), bytes.data() + (begin - at), end - begin);
    }
    return static_cast<ssize_t>(size);
}

// As MPEG4Extractor's kMaxAtomSize: a box other than the mdat with more than 64 MiB of data fails the file before the
// moov (an ftyp's brands are not read beyond it) and ends the walk after it.
TEST(a_box_above_64_mib_but_the_mdat_fails_the_file_before_the_movie) {
    const Bytes plain = built_file(Knobs{});
    const size_t ftyp_size = 20;
    const Bytes rest(plain.begin() + static_cast<long>(ftyp_size), plain.end());
    auto header = [](const char *type, uint64_t data) {
        Bytes out;
        put32(out, 1);
        out.insert(out.end(), type, type + 4);
        put32(out, static_cast<uint32_t>((data + 16) >> 32));
        put32(out, static_cast<uint32_t>(data + 16));
        return out;
    };
    const uint64_t cap = 64u << 20;
    for (const char *type : {"free", "ftyp"}) {
        for (const uint64_t data : {cap, cap + 1}) {
            Bytes start = Bytes(plain.begin(), plain.begin() + static_cast<long>(ftyp_size));
            Bytes box = header(type, data);
            if (std::strcmp(type, "ftyp") == 0) box.insert(box.end(), {'i', 's', 'o', 'm', 0, 0, 2, 0});
            Sparse sparse{{{0, start}, {ftyp_size, box}, {ftyp_size + 16 + data, rest}}};
            const uint64_t length = ftyp_size + 16 + data + rest.size();
            minato::mp4::Tracks tracks;
            const int answer = minato::mp4::parse(Source{&sparse, read_sparse, length}, &tracks);
            CHECK(data > cap ? answer == minato::mp4::kMalformed : answer == 0 && tracks.size() == 1);
        }
    }
    // After the moov (no ftyp, so the walk reaches it), a large box is not read and ends the walk.
    Sparse after{{{0, rest}, {rest.size(), header("free", cap + 1)}}};
    minato::mp4::Tracks tracks;
    CHECK(minato::mp4::parse(Source{&after, read_sparse, rest.size() + 16 + cap + 1}, &tracks) == 0 &&
          tracks.size() == 1);
}

Bytes edit_list(uint8_t version, std::initializer_list<std::pair<uint64_t, int64_t>> entries) {
    Bytes body = full(static_cast<uint32_t>(version) << 24);
    put32(body, static_cast<uint32_t>(entries.size()));
    for (const auto &[duration, media_time] : entries) {
        if (version == 1) {
            put32(body, static_cast<uint32_t>(duration >> 32));
            put32(body, static_cast<uint32_t>(duration));
            put32(body, static_cast<uint32_t>(static_cast<uint64_t>(media_time) >> 32));
        } else {
            put32(body, static_cast<uint32_t>(duration));
        }
        put32(body, static_cast<uint32_t>(media_time));
        put32(body, 0x00010000);  // rate 1
    }
    return body;
}

// As Android's elst and getTrack: an edit list of more than two entries is ignored; an empty first edit is rounded
// from the movie's timescale (the moov's last mvhd's, wherever it is) into the track's; one whose conversion
// overflows keeps the track listed but marks it, as getTrack fails for it; and one past int64_t's range saturates.
TEST(an_edit_list_reads_as_android_reads_it) {
    Knobs knobs;
    knobs.movie_timescale = 7;
    knobs.timescale = 10;
    knobs.elst = edit_list(0, {{2, -1}, {40, 5}});
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(tracks[0].edit_empty == 3 && tracks[0].edit_media_time == 5);  // (2*10+3)/7
    knobs.elst = edit_list(0, {{2, -1}, {40, 5}, {40, 6}});
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(tracks[0].edit_empty == 0 && tracks[0].edit_media_time == 0);
    knobs.elst = edit_list(0, {{2, -1}, {40, 5}});
    knobs.mvhd_last = true;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(tracks[0].edit_empty == 3 && tracks[0].movie_timescale == 7);
    knobs.mvhd_last = false;
    knobs.timescale = 30000;
    knobs.elst = edit_list(1, {{1ull << 63, -1}, {40, 5}});
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(tracks[0].edit_overflow);
    knobs.timescale = 1;
    knobs.movie_timescale = 1;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(!tracks[0].edit_overflow && tracks[0].edit_empty == INT64_MAX);
}

// MPEG4Source::read's conversion of NAL units to Annex B: a unit of length 0 is skipped, a length past the sample
// ends the sample with the units before it, and a sample that grows past the buffer (max-input-size, the largest
// size plus 20) is malformed.
TEST(nal_units_convert_as_mpeg4_source_reads_them) {
    struct Case {
        uint8_t nal_size;
        Bytes sample, output;
        long answer;
    };
    std::vector<Case> cases = {
        {4, {0, 0, 0, 0, 0, 0, 0, 5, 0x65, 1, 2, 3, 4, 0, 0, 0, 3, 9, 9}, {0, 0, 0, 1, 0x65, 1, 2, 3, 4}, 9},
        {2, {0, 0, 0, 3, 0x65, 1, 2, 0, 9, 7, 7}, {0, 0, 0, 1, 0x65, 1, 2}, 7},
        {1, {0, 2, 0x65, 1, 5, 0x65}, {0, 0, 0, 1, 0x65, 1}, 6},
        {1, {1, 0x65, 1, 0x65, 1, 0x65, 1, 0x65}, {}, 20},
    };
    // 8 units of 1 byte: 40 bytes, past 16 + 20.
    cases.push_back({1, {1, 0x65, 1, 0x65, 1, 0x65, 1, 0x65, 1, 0x65, 1, 0x65, 1, 0x65, 1, 0x65}, {}, -1007});
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.nal_size = test.nal_size;
        knobs.first_sample = test.sample;
        const Bytes file = built_file(knobs);
        minato::mp4::Tracks tracks;
        CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
        if (tracks.size() != 1) continue;
        long answer = 0;
        const Bytes out = read_all(source_of(file), tracks[0], 0, &answer);
        CHECK(answer == test.answer);
        if (answer != test.answer) std::fprintf(stderr, "  NAL size %u: %ld\n", test.nal_size, answer);
        if (!test.output.empty() && answer > 0) CHECK(Bytes(out.begin(), out.begin() + answer) == test.output);
    }
}

// mdhd as MPEG4Extractor reads it: a duration of 0, or all ones in a version 0 box, gives none; one past int64_t's µs
// or a negative one of a version 1 box (all ones too) is malformed, as a timescale of 0 is; a version above 1 is an
// I/O error.
TEST(a_media_header_reads_as_androids) {
    struct Case {
        uint8_t version;
        int64_t duration;
        uint32_t timescale;
        int answer;
        int64_t duration_us;
    };
    const Case cases[] = {
        {0, -1, 30000, 0, 133333},  // the 4 samples' 4000 ticks
        {0, 0, 30000, 0, -1},
        {0, 0xffffffff, 30000, 0, -1},
        {1, static_cast<int64_t>(UINT64_MAX), 30000, minato::mp4::kMalformed, 0},
        {1, INT64_MIN, 30000, minato::mp4::kMalformed, 0},
        {1, int64_t{1} << 62, 1, minato::mp4::kMalformed, 0},
        {1, int64_t{1} << 40, 1, 0, (int64_t{1} << 40) * 1000000},
        {0, -1, 0, minato::mp4::kMalformed, 0},
        {2, -1, 30000, minato::mp4::kIo, 0},
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.mdhd_version = test.version;
        knobs.timescale = test.timescale;
        // -1 is the knob's default, so all ones of a version 1 box is put in through UINT64_MAX's bits.
        knobs.mdhd_duration = test.duration;
        if (test.version == 1 && test.duration == static_cast<int64_t>(UINT64_MAX)) knobs.mdhd_duration = -2;
        Bytes file = built_file(knobs);
        if (knobs.mdhd_duration == -2) {
            const char mdhd[] = {'m', 'd', 'h', 'd'};
            const auto at = std::search(file.begin(), file.end(), mdhd, mdhd + 4);
            CHECK(at != file.end());
            if (at != file.end()) std::fill(at + 4 + 4 + 16 + 4, at + 4 + 4 + 16 + 4 + 8, 0xff);
        }
        minato::mp4::Tracks tracks;
        const int answer = parsed(file, &tracks);
        CHECK(answer == test.answer);
        if (answer != test.answer) std::fprintf(stderr, "  mdhd %u %lld: %d\n", test.version,
                                                static_cast<long long>(test.duration), answer);
        if (answer == 0) CHECK(tracks.size() == 1 && tracks[0].duration_us == test.duration_us);
    }
}

// stsd as MPEG4Extractor reads it: every entry in turn, each one's codec in place of the one before (a second avcC's
// configuration in place of the first's); a second stsd likewise, unless it has more than one entry, which leaves the
// track out; a version or flags other than 0, a count of entries the box does not hold, or bytes after them, malformed.
TEST(sample_descriptions_read_as_androids) {
    struct Case {
        std::vector<std::vector<std::string>> stsds;
        uint32_t claim, flags;
        bool tail;
        int answer;
        const char *mime;  // nullptr: no track
    };
    const Case cases[] = {
        {{{"avc1"}, {"mp4a"}}, 0, 0, false, 0, "audio/mp4a-latm"},
        {{{"avc1", "mp4a"}}, 0, 0, false, 0, "audio/mp4a-latm"},
        {{{"mp4a", "avc1"}}, 0, 0, false, 0, "video/avc"},
        {{{"avc1"}, {"mp4a", "avc1"}}, 0, 0, false, 0, nullptr},
        {{{"avc1"}}, 0, 1, false, minato::mp4::kMalformed, nullptr},
        {{{"avc1"}}, 2, 0, false, minato::mp4::kMalformed, nullptr},
        {{{"avc1"}}, 0, 0, true, minato::mp4::kMalformed, nullptr},
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.stsds = test.stsds;
        knobs.stsd_claim = test.claim;
        knobs.stsd_flags = test.flags;
        knobs.stsd_tail = test.tail;
        const Bytes file = built_file(knobs);
        minato::mp4::Tracks tracks;
        const int answer = parsed(file, &tracks);
        CHECK(answer == test.answer);
        if (answer != test.answer) std::fprintf(stderr, "  stsd case: %d\n", answer);
        if (answer != 0) continue;
        CHECK(tracks.size() == (test.mime == nullptr ? 0u : 1u));
        if (tracks.size() != 1 || test.mime == nullptr) continue;
        CHECK(text(tracks[0].mime) == test.mime);
        // An AAC track's samples are read as they are, a video track's as Annex B.
        const bool audio = text(test.mime) == "audio/mp4a-latm";
        CHECK(tracks[0].nal_length_size == (audio ? 0u : 4u));
        long read = 0;
        const Bytes got = read_all(source_of(file), tracks[0], 0, &read);
        CHECK(read == 16 && got[3] == (audio ? 12 : 1));
    }
    Knobs twice;
    twice.avcc_twice = true;
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(twice), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        const Bytes sps{0, 0, 0, 1, 0x67, 77, 0, 30};
        CHECK(tracks[0].csd0.size == sps.size() && Bytes(tracks[0].csd0.data, tracks[0].csd0.data + 8) == sps);
        const Bytes pps{0, 0, 0, 1, 0x68, 0xee};
        CHECK(tracks[0].csd1.size == pps.size() && Bytes(tracks[0].csd1.data, tracks[0].csd1.data + 6) == pps);
    }
}

// As MPEG4Extractor: each entry Android knows names the track's codec in place of the one before (one it does not know
// leaves it), every avcC, hvcC and esds in the entries stays, the track's final codec needs its own box at the trak's
// end (verifyTrack), and its NAL length and csd come from that box (MPEG4Source, convertMessageToMetaData), whatever
// boxes its other entries had (observed: an AAC track after an avc1 entry keeps its AudioSpecificConfig).
TEST(the_final_codec_and_its_configuration_read_as_androids) {
    const Bytes sps{0, 0, 0, 1, 0x67, 66, 0, 30}, vps{0, 0, 0, 1, 0x40, 1}, aac{0x12, 0x10};
    struct Case {
        std::vector<std::vector<std::string>> stsds;
        const char *mime;  // nullptr: no track
        uint32_t nal_length_size;
        Bytes csd0;
        int answer = 0;
    };
    const Case cases[] = {
        {{{"avc1", "hvc1-"}}, nullptr, 0, {}},
        {{{"hvc1", "avc1-"}}, nullptr, 0, {}},
        {{{"avc1-"}, {"avc1"}}, "video/avc", 2, sps},
        {{{"avc1-"}, {"mp4a"}}, "audio/mp4a-latm", 0, aac},
        {{{"mp4a", "mp4v"}}, nullptr, 0, {}},
        {{{"mp4v", "avc1"}}, "video/avc", 2, sps},
        {{{"Opus+"}, {"mp4a", "avc1"}}, nullptr, 0, {}},
        {{{"avc1", "zzzz"}}, "video/avc", 2, sps},
        {{{"avc1", "mp4a"}}, "audio/mp4a-latm", 0, aac},
        {{{"avc1", "hvc1"}}, "video/hevc", 4, vps},
        {{{"hvc1", "avc1"}}, "video/avc", 2, sps},
        {{{"hvc1", "mp4a"}}, "audio/mp4a-latm", 0, aac},
        {{{"hvc1"}}, "video/hevc", 4, vps},
        {{{"tx3g"}, {"mp4a", "avc1"}}, "video/avc", 2, sps},
        {{{"mp4v*", "avc1-"}}, "video/avc", 2, sps},
        {{{"mp4a-"}}, nullptr, 0, {}},
        {{{"avc1", "tx3g"}}, "video/avc", 2, sps},
        {{{"avc1+e"}}, "video/avc", 2, sps},
        {{{"mp4a~"}}, "audio/mp4a-latm", 0, aac},
        {{{"mp4a!"}}, nullptr, 0, {}, minato::mp4::kMalformed},
        {{{"avc1/0"}}, "video/avc", 2, {}},
        {{{"hvc1/0"}}, "video/hevc", 4, {}},
        {{{"Opus*", "avc1-"}}, nullptr, 0, {}, minato::mp4::kMalformed},  // its avcC read as a dOps of version 1
        {{{"mp4v+e", "mp4a-"}}, "audio/mp4a-latm", 0, aac},
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.stsds = test.stsds;
        knobs.nal_size = 2;  // the avcC's, the hvcC's being 4
        minato::mp4::Tracks tracks;
        const int answer = parsed(built_file(knobs), &tracks);
        const bool kept = answer == 0 && tracks.size() == 1;
        const bool right = answer == test.answer && tracks.size() == (test.mime == nullptr ? 0u : 1u) &&
                           (!kept || (text(tracks[0].mime) == test.mime &&
                                      tracks[0].nal_length_size == test.nal_length_size &&
                                      bytes(tracks[0].csd0) == test.csd0));
        CHECK(right);
        if (right) continue;
        std::string name;
        for (const auto &stsd : test.stsds) {
            name += "[";
            for (const auto &entry : stsd) name += entry + " ";
            name += "]";
        }
        std::fprintf(stderr, "  %s: answer %d, %zu tracks", name.c_str(), answer, tracks.size());
        if (kept) {
            std::fprintf(stderr, ", %s, NAL length %u, csd0 of %zu bytes", text(tracks[0].mime).c_str(),
                         tracks[0].nal_length_size, tracks[0].csd0.size);
        }
        std::fprintf(stderr, "\n");
    }
}

// The allocations the process makes while `counting`: ASan's malloc hook, which a realloc that moves calls too.
std::atomic<size_t> allocations{0};
std::atomic<bool> counting{false};
}  // namespace

extern "C" void __sanitizer_malloc_hook(const volatile void *, size_t) {
    if (counting) allocations++;
}

namespace {

// QuickTime's AAC (ADR-0060 e2d step 2a.2): only an esds whose parent is the mp4a gives the audio object
// (aac-profile) and the bit rates, each only above 0, so that a later mp4a's esds of bit rates 0 keeps them, as
// updateAudioTrackInfoFromESDS_MPEG4Audio sets them; the final esds of any parent stays apart, as the format's
// conversion reads it [verified: MPEG4Extractor.cpp and Utils.cpp at the tag].
TEST(quicktime_aac_keys_come_from_the_mp4a_esds_and_the_final_one_as_androids) {
    struct Case {
        std::vector<std::vector<std::string>> stsds;
        bool object;
        uint32_t max, avg, final_max, final_avg;
    };
    const Case cases[] = {
        {{{"mp4a~"}}, false, 0, 0, 128000, 96000},
        {{{"mp4a"}}, true, 128000, 96000, 128000, 96000},
        {{{"mp4a", "mp4a0"}}, true, 128000, 96000, 0, 0},
        {{{"mp4a0"}}, true, 0, 0, 0, 0},
        {{{"mp4a0", "mp4a~"}}, true, 0, 0, 128000, 96000},
        {{{"mp4aM"}}, true, 0, 0, INT32_MAX, INT32_MAX},
        {{{"mp4a3", "mp4a0"}}, true, 0, 0, 0, 0},  // QCELP's and MP3's esds give no bit rates
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.stsds = test.stsds;
        minato::mp4::Tracks tracks;
        const bool right = parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1 &&
                           tracks[0].has_audio_object == test.object &&
                           (!test.object || tracks[0].audio_object == 2) && tracks[0].max_bitrate == test.max &&
                           tracks[0].avg_bitrate == test.avg && tracks[0].esds_max_bitrate == test.final_max &&
                           tracks[0].esds_avg_bitrate == test.final_avg;
        CHECK(right);
        if (!right) std::printf("  case %s, %zu tracks\n", test.stsds[0][0].c_str(), tracks.size());
    }
}

// One hvcC of one array of as many units of no bytes as the csd holds: the csd made in one allocation, not grown by
// each unit (which copies it once per unit, quadratic in the units).
TEST(one_configuration_of_many_units_is_made_in_one_allocation) {
#if defined(__has_feature) && __has_feature(address_sanitizer)
    Knobs knobs;
    knobs.stsds = {{"hvc1"}};
    knobs.hvcc_empty_units = minato::mp4::kMaxCodecData / 4;
    const Bytes file = built_file(knobs);
    minato::mp4::Tracks tracks;
    allocations = 0;
    counting = true;
    const int answer = parsed(file, &tracks);
    counting = false;
    CHECK(answer == 0 && tracks.size() == 1 && tracks[0].csd0.size == minato::mp4::kMaxCodecData);
    CHECK(allocations < 100);
    if (allocations >= 100) std::fprintf(stderr, "  %zu allocations\n", allocations.load());
#endif
}

// One unit more than kMaxCodecData holds: the file is malformed, as Minato bounds a track's csd.
TEST(a_configuration_past_the_codec_data_bound_is_malformed) {
    Knobs knobs;
    knobs.stsds = {{"hvc1"}};
    knobs.hvcc_empty_units = minato::mp4::kMaxCodecData / 4 + 1;
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(knobs), &tracks) == minato::mp4::kMalformed);
    // An avcC of an SPS of 65,535 bytes, which with its start code passes the bound too, and one of such a PPS.
    Knobs avc;
    avc.avcc_body = {1, 66, 0, 30, 0xff, 0xe1, 0xff, 0xff};
    avc.avcc_body.insert(avc.avcc_body.end(), 0xffff, 0x67);
    avc.avcc_body.insert(avc.avcc_body.end(), {1, 0, 2, 0x68, 0xce});
    CHECK(parsed(built_file(avc), &tracks) == minato::mp4::kMalformed);
    avc.avcc_body = {1, 66, 0, 30, 0xff, 0xe1, 0, 4, 0x67, 66, 0, 30, 1, 0xff, 0xff};
    avc.avcc_body.insert(avc.avcc_body.end(), 0xffff, 0x68);
    CHECK(parsed(built_file(avc), &tracks) == minato::mp4::kMalformed);
    // A plain csd-0 past the bound: an av1C of a byte more.
    Knobs av1;
    av1.stsds = {{"avc1"}};
    av1.entry_boxes = {box("av1C", Bytes(minato::mp4::kMaxCodecData + 1, 0x81))};
    CHECK(parsed(built_file(av1), &tracks) == minato::mp4::kMalformed);
    // An esds of a DecoderSpecificInfo past the bound, the mp4a's own and one in a QuickTime wave.
    for (const char *entry : {"mp4aL", "mp4a~L"}) {
        Knobs aac;
        aac.stsds = {{entry}};
        const bool malformed = parsed(built_file(aac), &tracks) == minato::mp4::kMalformed;
        CHECK(malformed);
        if (!malformed) std::printf("  %s: %zu tracks\n", entry, tracks.size());
    }
}

// As convertMetaDataToMessage: every SPS of the avcC into csd-0 and every PPS into csd-1, each after a start code.
TEST(every_parameter_set_goes_into_the_codec_data) {
    Knobs knobs;
    knobs.avcc_body = {1, 66, 0, 30, 0xff, 0xe2, 0, 4, 0x67, 66, 0, 30, 0, 4, 0x67, 66, 0, 31,
                       2, 0, 2, 0x68, 0xce, 0, 2, 0x68, 0xcf};
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() != 1) return;
    CHECK(bytes(tracks[0].csd0) == (Bytes{0, 0, 0, 1, 0x67, 66, 0, 30, 0, 0, 0, 1, 0x67, 66, 0, 31}));
    CHECK(bytes(tracks[0].csd1) == (Bytes{0, 0, 0, 1, 0x68, 0xce, 0, 0, 0, 1, 0x68, 0xcf}));
}

// Each hvcC replaces the one before, as Android's csd-hevc does: a movie of many, of many units each, parses in time
// linear in its size, its codec data made once from the last.
TEST(many_configurations_parse_in_linear_time) {
    Knobs knobs;
    knobs.stsds = {{"hvc1"}};
    knobs.hvcc_boxes = 1000;
    knobs.hvcc_empty_units = 16000;
    const Bytes file = built_file(knobs);
    minato::mp4::Tracks tracks;
    const auto start = std::chrono::steady_clock::now();
    const int answer = parsed(file, &tracks);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(answer == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(tracks[0].csd0.size == 16000u * 4);
    CHECK(seconds < 10);  // about 100 s with ASan when each box made the csd again, under 2 s once
    if (seconds >= 10) std::fprintf(stderr, "  %.2f s\n", seconds);
}

// An HEVC SPS NAL unit as an encoder writes it (its header, then the RBSP with emulation prevention bytes): 4:2:0
// (or `chroma`) 320 x 180 with a VUI of a video signal type, its full range flag and, when described, `values` of
// its colour description's primaries, transfer and matrix, the unit ending after a cut description; `sets` short-term
// reference picture sets claimed but not there, or one of 2^31 negative and 2^31 positive pictures (`overflow`).
struct Vui {
    bool signal = true, described = true;
    uint32_t full_range = 0, primaries = 1, transfer = 1, matrix = 1;
    uint32_t chroma = 1;
    int values = 3;
    uint32_t sets = 0;
    bool overflow = false;
};

Bytes hevc_sps(const Vui &vui) {
    Bytes rbsp;
    uint32_t pending = 0;
    int count = 0;
    const auto put = [&](uint32_t value, int bits) {
        for (int bit = bits - 1; bit >= 0; bit--) {
            pending = pending << 1 | ((value >> bit) & 1u);
            if (++count == 8) {
                rbsp.push_back(static_cast<uint8_t>(pending));
                pending = 0;
                count = 0;
            }
        }
    };
    const auto ue = [&](uint32_t value) {  // Exp-Golomb
        const uint64_t code = static_cast<uint64_t>(value) + 1;
        int length = 0;
        while ((code >> (length + 1)) != 0) length++;
        put(0, length);
        for (int bit = length; bit >= 0; bit--) put(static_cast<uint32_t>((code >> bit) & 1u), 1);
    };
    put(0, 4);  // VPS id
    put(0, 3);  // one sub-layer
    put(1, 1);  // temporal id nesting
    put(0x01, 8), put(0x60000000, 32), put(0x9000, 16), put(0, 32), put(93, 8);  // Main profile, level 93: 96 bits
    ue(0);                                                // SPS id
    ue(vui.chroma);
    if (static_cast<uint8_t>(vui.chroma) == 3) put(0, 1);  // separate_colour_plane_flag
    ue(320), ue(180), put(0, 1);                          // the size, no conformance window
    ue(0), ue(0), ue(4);                                  // bit depths 8, POC LSB of 8 bits
    put(1, 1), ue(4), ue(0), ue(0);                       // the sub-layer's ordering
    ue(0), ue(1), ue(0), ue(2), ue(0), ue(0);             // block sizes and transform depths
    put(0, 1), put(0, 1), put(1, 1), put(0, 1);           // no scaling list, no AMP, SAO, no PCM
    ue(vui.overflow ? 1 : vui.sets);
    if (vui.overflow) ue(0x80000000u), ue(0x80000000u);
    put(0, 1), put(1, 1), put(1, 1);  // no long-term reference pictures, TMVP, strong smoothing
    put(1, 1), put(0, 1), put(0, 1);                      // a VUI, no aspect ratio, no overscan
    put(vui.signal ? 1 : 0, 1);
    if (vui.signal) {
        put(5, 3), put(vui.full_range, 1), put(vui.described ? 1 : 0, 1);  // unspecified video format
        const uint32_t values[] = {vui.primaries, vui.transfer, vui.matrix};
        for (int index = 0; vui.described && index < vui.values; index++) put(values[index], 8);
    }
    if (vui.values == 3) put(0x10, 5);  // no chroma location, ..., then the RBSP's stop bit
    if (count != 0) put(0, 8 - count);
    Bytes unit = {0x42, 0x01};  // an SPS (33) of layer 0, temporal id 1
    int zeros = 0;
    for (uint8_t byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            unit.push_back(3);
            zeros = 0;
        }
        unit.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return unit;
}

// A colr box of `type`, nclx with its full range flag's byte, and a pasp box.
Bytes colr_box(const char *type, uint16_t primaries, uint16_t transfer, uint16_t matrix, bool full) {
    Bytes body(type, type + 4);
    put16(body, primaries);
    put16(body, transfer);
    put16(body, matrix);
    if (std::string(type) == "nclx") body.push_back(full ? 0x80 : 0);
    return box("colr", body);
}

Bytes pasp_box(uint32_t horizontal, uint32_t vertical) {
    Bytes body;
    put32(body, horizontal);
    put32(body, vertical);
    return box("pasp", body);
}

TEST(colour_keys_come_from_the_colr_and_the_hvccs_vui_as_androids) {
    const Bytes hdr10 = colr_box("nclx", 9, 16, 9, true), bt709 = colr_box("nclc", 1, 1, 1, false);
    const Vui pq{.primaries = 9, .transfer = 16, .matrix = 9};
    struct Case {
        const char *what;
        std::vector<std::string> entries;
        std::vector<Vui> units;
        std::vector<Bytes> boxes;
        minato::mp4::ColorKeys keys;
    };
    const Case cases[] = {
        {"HEVC of HDR10", {"hvc1"}, {pq}, {}, {1, 6, 6, 2}},
        {"HEVC of BT.709, full range", {"hvc1"}, {Vui{.full_range = 1}}, {}, {0, 1, 3, 1}},
        {"HEVC without a description", {"hvc1"}, {Vui{.described = false, .full_range = 1}}, {}, {}},
        {"HEVC without a signal type", {"hvc1"}, {Vui{.signal = false}}, {}, {}},
        {"HEVC unspecified", {"hvc1"}, {Vui{.primaries = 2, .transfer = 2, .matrix = 2}}, {}, {0, 0, 0, 2}},
        {"HEVC unspecified, a colr", {"hvc1"}, {Vui{.primaries = 2, .matrix = 2}}, {hdr10}, {0, 6, 3, 2}},
        {"HEVC cut after its primaries", {"hvc1"}, {Vui{.primaries = 9, .transfer = 16, .values = 1}}, {},
         {0, -1, -1, -1}},
        {"HEVC of two SPSs", {"hvc1"}, {pq, Vui{.described = false, .full_range = 1}}, {}, {1, 6, 6, 1}},
        {"HEVC of chroma 259", {"hvc1"}, {Vui{.chroma = 259}}, {}, {0, 1, 3, 2}},
        {"HEVC with a colr alone", {"hvc1"}, {}, {colr_box("nclx", 1, 1, 1, true)}, {-1, 1, 3, 1}},
        {"HEVC, then AVC", {"hvc1", "avc1"}, {pq}, {}, {}},
        {"AVC nclc", {"avc1"}, {}, {bt709}, {-1, 1, 3, 2}},
        {"AVC nclx of extended values", {"avc1"}, {}, {colr_box("nclx", 11, 17, 0, true)}, {-1, 130823, 99, 1}},
        {"AVC two colrs", {"avc1"}, {}, {bt709, hdr10}, {-1, 1, 3, 2}},
        {"AVC prof, then nclx", {"avc1"}, {}, {box("colr", Bytes{'p', 'r', 'o', 'f', 0, 0, 0, 0}), hdr10},
         {-1, 6, 6, 1}},
        {"AVC short nclx, then nclc", {"avc1"}, {}, {box("colr", Bytes{'n', 'c', 'l', 'x', 0, 9, 0, 16, 0, 9}), bt709},
         {-1, 1, 3, 2}},
        {"AVC unknown transfer", {"avc1"}, {}, {colr_box("nclx", 1, 3, 1, false)}, {-1, 1, -1, 2}},
        {"HEVC of HLG", {"hvc1"}, {Vui{.primaries = 9, .transfer = 18, .matrix = 9}}, {}, {1, 6, 7, 2}},
        // Its colour description's bytes are 00 03 21: a 3 after one 0 is no emulation prevention byte.
        {"HEVC of a 3 after one 0", {"hvc1"}, {Vui{.primaries = 0, .transfer = 1, .matrix = 144}}, {}, {0, 0, 3, 2}},
        {"HEVC of 2^31 - 1 missing sets", {"hvc1"}, {Vui{.sets = 0x7fffffffu}}, {}, {}},
        {"HEVC of 2^32 pictures", {"hvc1"}, {Vui{.overflow = true}}, {}, {}},
        {"AVC nclc of 11 bytes", {"avc1"}, {}, {box("colr", Bytes{'n', 'c', 'l', 'c', 0, 1, 0, 1, 0, 1, 0x80})},
         {-1, 1, 3, 2}},
        {"AVC nclx of a vendor transfer", {"avc1"}, {}, {colr_box("nclx", 1, 9, 1, false)}, {-1, 1, 65791, 2}},
        {"HEVC of an unspecified transfer, a colr", {"hvc1"}, {Vui{.transfer = 2}}, {hdr10}, {0, 1, 6, 2}},
        {"AVC nclc of an extended standard", {"avc1"}, {}, {colr_box("nclc", 4, 1, 1, false)}, {-1, 73, 3, 2}},
        {"AVC nclc of 9 bytes", {"avc1"}, {}, {box("colr", Bytes{'n', 'c', 'l', 'c', 0, 1, 0, 1, 0})}, {}},
        {"AAC", {"mp4a"}, {}, {hdr10}, {}},
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.stsds = {test.entries};
        for (const Vui &vui : test.units) knobs.hvcc_units.push_back(hevc_sps(vui));
        knobs.entry_boxes = test.boxes;
        minato::mp4::Tracks tracks;
        const bool parsed_one = parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1;
        const minato::mp4::ColorKeys keys = parsed_one ? minato::mp4::color_keys(tracks[0]) : minato::mp4::ColorKeys{};
        const bool right = parsed_one && keys.hdr == test.keys.hdr && keys.standard == test.keys.standard &&
                           keys.transfer == test.keys.transfer && keys.range == test.keys.range;
        CHECK(right);
        if (!right) {
            std::printf("  %s: %zu tracks, %d %d %d %d\n", test.what, tracks.size(), keys.hdr, keys.standard,
                        keys.transfer, keys.range);
        }
    }
}

// A codec configuration that the format's conversion or MPEG4Source refuses keeps its track, as MPEG4Extractor keeps
// the box's bytes and verifyTrack only asks for one: Utils' convertMetaDataToMessage refuses an avcC under 7 bytes,
// of another version than 1 or with its sets past its end, an hvcC under 23 bytes, of a version but 0 and 1 or with
// its units past its end (its count of arrays a char, unsigned on arm64), and an esds ESDS refuses; MPEG4Extractor's
// getTrack (the selection) refuses an avcC under 7 bytes or of another version than 1, and an hvcC under 22 bytes
// or of another version than 1 [observed: the emulator, media-extractor-test's broken configurations].
TEST(a_broken_configuration_keeps_its_track_as_androids) {
    const Bytes avcc = {1, 66, 0, 30, 0xff, 0xe1, 0, 4, 0x67, 66, 0, 30, 1, 0, 2, 0x68, 0xce};
    const Bytes hvcc = {1, 1, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 93, 0xf0, 0, 0xfc, 0xfd, 0xf8, 0xf8, 0, 0, 0x0f,
                        1, 0x20, 0, 1, 0, 2, 0x40, 1};
    const auto with = [](Bytes bytes, size_t at, uint8_t value) {
        bytes[at] = value;
        return bytes;
    };
    Bytes arrays(hvcc.begin(), hvcc.begin() + 22);  // 128 arrays of no units, all there
    arrays.push_back(0x80);
    for (int array = 0; array < 128; array++) arrays.insert(arrays.end(), {0x20, 0, 0});
    struct Case {
        const char *what;
        std::string entry;
        Bytes avcc, hvcc;
        bool format, select;
    };
    const Case cases[] = {
        {"avcC whole", "avc1", avcc, {}, true, true},
        {"avcC of version 0", "avc1", with(avcc, 0, 0), {}, false, false},
        {"avcC of 6 bytes", "avc1", Bytes(avcc.begin(), avcc.begin() + 6), {}, false, false},
        {"avcC of 4 bytes", "avc1", Bytes(avcc.begin(), avcc.begin() + 4), {}, false, false},
        {"avcC of an SPS past its end", "avc1", with(avcc, 6, 0xff), {}, false, true},
        {"avcC without its PPS count", "avc1", Bytes(avcc.begin(), avcc.begin() + 12), {}, false, true},
        {"avcC of a PPS past its end", "avc1", with(avcc, 13, 0xff), {}, false, true},
        {"hvcC whole", "hvc1", {}, hvcc, true, true},
        {"hvcC of version 0", "hvc1", {}, with(hvcc, 0, 0), true, false},
        {"hvcC of version 2", "hvc1", {}, with(hvcc, 0, 2), false, false},
        {"hvcC of 22 bytes", "hvc1", {}, Bytes(hvcc.begin(), hvcc.begin() + 22), false, true},
        {"hvcC of 21 bytes", "hvc1", {}, Bytes(hvcc.begin(), hvcc.begin() + 21), false, false},
        {"hvcC of a unit past its end", "hvc1", {}, with(hvcc, 26, 0xff), false, true},
        {"hvcC of 128 arrays", "hvc1", {}, with(hvcc, 22, 0x80), false, true},
        {"hvcC of 128 arrays, all there", "hvc1", {}, arrays, true, true},
        {"esds of tag 5 in a wave", "mp4a~5", {}, {}, false, true},
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.stsds = {{test.entry}};
        knobs.avcc_body = test.avcc;
        knobs.hvcc_whole = test.hvcc;
        minato::mp4::Tracks tracks;
        const bool right = parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1 &&
                           tracks[0].format_refused == !test.format && tracks[0].select_refused == !test.select &&
                           (!test.select || test.entry == "mp4a~5" || tracks[0].nal_length_size == 4);
        CHECK(right);
        if (!right) std::printf("  %s: %zu tracks\n", test.what, tracks.size());
    }
}

// An SPS whose second reservoir fill finds only an emulation prevention byte: NALBitReader's fill of no bits,
// after which its read takes nothing and fails at the end (no shift by 32 bits, which UBSan reports).
TEST(an_sps_ending_in_an_emulation_prevention_byte_reads_nothing_more) {
    Knobs knobs;
    knobs.stsds = {{"hvc1"}};
    knobs.hvcc_units = {Bytes{0x42, 0x01, 0x01, 0x01, 0x00, 0x00, 0x03}};
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1 &&
          minato::mp4::color_keys(tracks[0]).hdr == -1);
}

TEST(a_pasp_and_a_colr_read_as_androids) {
    struct Case {
        std::vector<Bytes> boxes;
        bool sar;
        uint32_t width, height;
    };
    const Case cases[] = {
        {{pasp_box(4, 3)}, true, 4, 3},
        {{pasp_box(0, 3)}, false, 0, 0},
        {{pasp_box(4, 0)}, false, 0, 0},
        {{pasp_box(4, 3), pasp_box(16, 9)}, true, 16, 9},
        {{pasp_box(4, 3), pasp_box(1, 0)}, true, 4, 3},
        {{pasp_box(0x80000000u, 1)}, true, 0x80000000u, 1},
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.entry_boxes = test.boxes;
        minato::mp4::Tracks tracks;
        const bool right = parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1 &&
                           tracks[0].has_sar == test.sar && tracks[0].sar_width == test.width &&
                           tracks[0].sar_height == test.height;
        CHECK(right);
        if (!right) std::printf("  %zu boxes: %zu tracks\n", test.boxes.size(), tracks.size());
    }
    // parsePaspBox and parseColorInfo fail the file on a box too short for its fields.
    for (const Bytes &short_box : {box("pasp", Bytes(7, 1)), box("colr", Bytes{'n', 'c', 'l'})}) {
        Knobs knobs;
        knobs.entry_boxes = {short_box};
        minato::mp4::Tracks tracks;
        CHECK(parsed(built_file(knobs), &tracks) == minato::mp4::kMalformed);
    }
}

// MPEG4Extractor's plain csd-0 (an av1C's, a vpcC's, a dfLa's or an mhaC's bytes among an entry's boxes, an Opus
// entry's OpusHead with its codec delay and seek pre-roll as csd-1 and csd-2, an alac entry's cookie), which the
// extractor service's convertMessageToMetaData takes before csd-avc, csd-hevc and esds: an AVC track's format has it
// as is without a csd-1 (no profile or level), else an avcC reassembleAVCC makes of both; an HEVC track's format is
// refused (reassembleHVCC finds no start code); an AAC track's esds is reassembleESDS's (64 kbit/s, csd-0 its config).
// A dfLa of another size than 42, an mhaC under 13 bytes or of another config length, an Opus entry's dOps under 19 or
// over 276 bytes or of a version but 0, and an alac cookie of another header fail the file, as Android's
// [observed: media-extractor-test's csd variants on the emulator].
TEST(a_plain_csd_comes_before_the_configuration_as_androids) {
    const Bytes av1c = {0x81, 0x08, 0x0c, 0x00}, vpcc = {1, 0, 0, 0, 0, 10, 0x80, 1, 1, 1, 0, 0};
    Bytes dfla = {0, 0, 0, 0, 0x80, 0, 0, 34};
    for (int index = 0; index < 34; index++) dfla.push_back(static_cast<uint8_t>(index));
    Bytes flac = {'f', 'L', 'a', 'C'};
    flac.insert(flac.end(), dfla.begin() + 4, dfla.end());
    const Bytes mhac = {1, 0x0d, 6, 0, 2, 0xab, 0xcd};
    const Bytes opus_head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01, 0x80, 0xbb, 0, 0, 0, 0, 0};
    const Bytes delay = {0xa0, 0x2e, 0x63, 0, 0, 0, 0, 0}, pre_roll = {0, 0xb4, 0xc4, 0x04, 0, 0, 0, 0};
    const Bytes cookie = {0, 0, 0x10, 0, 0, 16, 40, 10, 14, 2, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xac, 0x44};
    const Bytes avcc_v0 = {0, 66, 0, 30, 0xff, 0xe1, 0, 4, 0x67, 66, 0, 30, 1, 0, 2, 0x68, 0xce};
    struct Case {
        const char *what;
        std::vector<std::string> entries;
        std::vector<Bytes> boxes;
        int answer;
        Bytes csd0;  // the format's
        int opaque;  // how many of csd-0, csd-1 and csd-2 MPEG4Extractor's meta has
        bool reassembled = false, refused = false;
    };
    const Case cases[] = {
        {"avc1 and av1C", {"avc1"}, {box("av1C", av1c)}, 0, av1c, 1},
        {"avc1 and vpcC", {"avc1"}, {box("vpcC", vpcc)}, 0, vpcc, 1},
        {"avc1 and dfLa", {"avc1"}, {box("dfLa", dfla)}, 0, flac, 1},
        {"avc1 and dfLa of 41 bytes", {"avc1"}, {box("dfLa", Bytes(dfla.begin(), dfla.end() - 1))},
         minato::mp4::kMalformed, {}, 0},
        {"avc1 and dfLa of 43 bytes", {"avc1"}, {box("dfLa", cat({dfla, {0}}))}, minato::mp4::kMalformed, {}, 0},
        {"avc1 and mhaC", {"avc1"}, {box("mhaC", mhac)}, 0, {0xab, 0xcd}, 1},
        {"avc1 and mhaC of 4 bytes", {"avc1"}, {box("mhaC", {1, 0x0d, 6, 0})}, minato::mp4::kMalformed, {}, 0},
        {"avc1 and mhaC of a longer config", {"avc1"}, {box("mhaC", {1, 0x0d, 6, 0, 3, 0xab, 0xcd})},
         minato::mp4::kMalformed, {}, 0},
        {"avc1 and mhaC of a shorter config", {"avc1"}, {box("mhaC", {1, 0x0d, 6, 0, 1, 0xab, 0xcd})},
         minato::mp4::kMalformed, {}, 0},
        {"avc1 and av1C, then vpcC", {"avc1"}, {box("av1C", av1c), box("vpcC", vpcc)}, 0, vpcc, 1},
        {"Opus, then avc1", {"Opus+", "avc1"}, {}, 0, {}, 3, true},
        {"alac, then avc1", {"alac+", "avc1"}, {}, 0, cookie, 1},
        {"Opus without a dOps", {"Opus", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"Opus of a dOps of version 1", {"Opus+v1", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"Opus of 18 bytes", {"Opus+18", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"Opus of 276 bytes", {"Opus+276", "avc1"}, {}, 0, {}, 3, true},
        {"Opus of 277 bytes", {"Opus+277", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"Opus of a sound entry of version 1", {"Opus+q", "avc1"}, {}, 0, {}, 3, true},
        {"Opus of a sound entry of version 2", {"Opus+w", "avc1"}, {}, 0, {}, 3, true},
        {"alac of a box claiming 40 bytes", {"alac+40", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"alac of a box of type alaC", {"alac+t", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"alac of flags 1", {"alac+f", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"alac of a box of 35 bytes", {"alac+35", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"alac of a byte after its box", {"alac+x", "avc1"}, {}, minato::mp4::kMalformed, {}, 0},
        {"hvc1 and av1C", {"hvc1"}, {box("av1C", av1c)}, 0, {}, 1, false, true},
        {"mp4a and av1C", {"mp4a"}, {box("av1C", av1c)}, 0, av1c, 1},
        {"mp4a and mhaC", {"mp4a"}, {box("mhaC", mhac)}, 0, {0xab, 0xcd}, 1},
        {"mp4a of a wave's esds of tag 5, and av1C", {"mp4a~5"}, {box("av1C", av1c)}, 0, av1c, 1},
        {"mp4a of a wave of an av1C", {"mp4a~+"}, {box("av1C", av1c)}, 0, av1c, 1},  // a wave's boxes are parsed too
        {"Opus, then mp4a", {"Opus+", "mp4a"}, {}, 0, opus_head, 3},
        {"alac, then mp4a", {"alac+", "mp4a"}, {}, 0, cookie, 1},
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.stsds = {test.entries};
        knobs.entry_boxes = test.boxes;
        minato::mp4::Tracks tracks;
        const int answer = parsed(built_file(knobs), &tracks);
        bool right = answer == test.answer;
        if (right && answer == 0) {
            const minato::mp4::Track &track = tracks[0];
            const auto same = [](const minato::mp4::Bytes &bytes, const Bytes &want) {
                return bytes.size == want.size() && std::equal(want.begin(), want.end(), bytes.data);
            };
            right = tracks.size() == 1 && track.format_refused == test.refused && !track.select_refused;
            for (int index = 0; index < 3; index++) right = right && track.has_opaque[index] == (index < test.opaque);
            if (test.opaque == 3) right = right && same(track.opaque[1], delay) && same(track.opaque[2], pre_roll);
            if (test.entries[0] == "Opus+" || test.entries[0] == "Opus+q" || test.entries[0] == "Opus+w") {
                right = right && same(track.opaque[0], opus_head);
            }
            if (!test.refused && !test.reassembled) right = right && track.csd_opaque && same(track.csd0, test.csd0);
            if (test.reassembled) {
                // reassembleAVCC of an OpusHead and a codec delay, which hold no start code: version 1, High
                // profile, no constraints, level 1.3, no SPS and no PPS.
                right = right && !track.csd_opaque && track.csd0.size == 0 && track.csd1.size == 0 &&
                        track.config_size >= 4 && track.config[0] == 1 && track.config[1] == 0x64 &&
                        track.config[2] == 0 && track.config[3] == 0x0d;
            }
            if (std::string(track.mime) == "audio/mp4a-latm") {
                right = right && track.esds_avg_bitrate == 64000 && track.esds_max_bitrate == 64000;
            }
            if (test.entries[0] == "alac+") right = right && track.bits_per_sample == 16;
            const bool of_mhac = !test.boxes.empty() && test.csd0 == Bytes{0xab, 0xcd};  // profile 13, layout 6
            right = right && track.mpegh_profile == (of_mhac ? 13 : -1) && track.mpegh_layout == (of_mhac ? 6 : -1);
        }
        CHECK(right);
        if (!right) std::printf("  %s: answer %d, %zu tracks\n", test.what, answer, tracks.size());
    }
    // reassembleAVCC of an OpusHead holding two SPSs: the first's profile, constraints and level (the second's
    // differ), each unit up to the next start code but one in the last 4 bytes, which stays in the unit; the gain
    // little-endian.
    {
        Knobs knobs;
        knobs.stsds = {{"Opus+s", "avc1"}};
        minato::mp4::Tracks tracks;
        const Bytes sets = {0, 0, 0, 1, 0x67, 0x64, 0, 0x1f, 0xaa, 0, 0, 0, 1, 0x67, 0x42, 0, 0x1e, 0xbb, 0, 0, 0, 1};
        const bool right = parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1 && !tracks[0].csd_opaque &&
                           tracks[0].opaque[0].size == 41 && tracks[0].opaque[0].data[16] == 2 &&
                           tracks[0].opaque[0].data[17] == 1 && tracks[0].csd0.size == sets.size() &&
                           std::equal(sets.begin(), sets.end(), tracks[0].csd0.data) && tracks[0].csd1.size == 0 &&
                           tracks[0].config[1] == 0x64 && tracks[0].config[2] == 0 && tracks[0].config[3] == 0x1f;
        CHECK(right);
    }
    // An avcC of version 0, which getTrack refuses, keeps refusing the selection; the format is the av1C's.
    Knobs knobs;
    knobs.stsds = {{"avc1"}};
    knobs.avcc_body = avcc_v0;
    knobs.entry_boxes = {box("av1C", av1c)};
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1 && tracks[0].select_refused &&
          !tracks[0].format_refused && tracks[0].csd_opaque && tracks[0].csd0.size == 4);
}

// As MPEG4Extractor's ac-3, ec-3 and ac-4 entries: parseAC3SpecificBox, parseEAC3SpecificBox and parseAC4SpecificBox
// read the dac3, dec3 or dac4 from the entry's 28th byte, by the file's bytes; one they refuse leaves the track out
// (skipTrack), whatever entries follow, and one they take names a codec the next entry replaces. A dac3 under 11
// bytes, of another type, of fscod 3, of a rate but the entry's or of a bsid above 8; a dec3 under 11 or over 152
// bytes, of another type, short of 23 bits a substream, of fscod 3, another rate, a bsid above 16, without the bit or
// 9 bits after a substream, or of flags whose bytes are not there (its `size` bytes read from its payload, 8 past the
// box); a dac4 under 11 bytes, of another type or of a DSI of 1176 bytes or more, or one AC4DSIParser refuses before
// its presentations: a DSI version above 1, a bitstream version above 1 in a DSI of version 0, bits short of a field
// [observed: media-extractor-test's ac-3, ec-3 and ac-4 variants on the emulator]. A dec3 whose substreams after the
// first run past its bytes fails the file: ABitReader's getBits aborts there (CHECK) [verified at android-15.0.0_r1].
TEST(a_refused_dolby_box_leaves_its_track_out_as_androids) {
    Bytes sound(6, 0);
    put16(sound, 1);
    sound.insert(sound.end(), 8, 0);
    put16(sound, 2);
    put16(sound, 16);
    put32(sound, 0);
    put32(sound, 44100u << 16);
    struct Case {
        const char *what;
        const char *entry, *box;
        uint32_t claim;  // the specific box's size field
        Bytes payload;   // and what follows it in the entry
        bool kept;
        int answer = 0;
    };
    const Case cases[] = {
        {"ac-3", "ac-3", "dac3", 11, {0x50, 0x08, 0}, true},
        {"ac-3 of fscod 3", "ac-3", "dac3", 11, {0xd0, 0x08, 0}, false},
        {"ac-3 of 48000 Hz", "ac-3", "dac3", 11, {0x10, 0x08, 0}, false},
        {"ac-3 of bsid 9", "ac-3", "dac3", 11, {0x52, 0x08, 0}, false},
        {"ac-3 of bsid 8", "ac-3", "dac3", 11, {0x50, 0x08, 0}, true},
        {"ac-3 of a dac3 of 10 bytes", "ac-3", "dac3", 10, {0x50, 0x08, 0}, false},
        {"ac-3 of a dacX", "ac-3", "dacX", 11, {0x50, 0x08, 0}, false},
        {"ec-3", "ec-3", "dec3", 13, {0x06, 0, 0x60, 0x04, 0}, true},
        {"ec-3 of a dependent substream", "ec-3", "dec3", 14, {0x06, 0, 0x60, 0x04, 0x02, 0x01}, true},
        {"ec-3 of fscod 3", "ec-3", "dec3", 13, {0x06, 0, 0xe0, 0x04, 0}, false},
        {"ec-3 of 48000 Hz", "ec-3", "dec3", 13, {0x06, 0, 0x20, 0x04, 0}, false},
        {"ec-3 of bsid 17", "ec-3", "dec3", 13, {0x06, 0, 0x62, 0x04, 0}, false},
        {"ec-3 of bsid 16", "ec-3", "dec3", 13, {0x06, 0, 0x60, 0x04, 0}, true},
        {"ec-3 of a mask of 8 flags", "ec-3", "dec3", 13, {0x06, 0, 0x60, 0x04, 0, 0xff, 1, 2}, false},
        {"ec-3 of a mask of 7 flags", "ec-3", "dec3", 13, {0x06, 0, 0x60, 0x04, 0, 0x7f, 1, 2}, true},
        {"ec-3 of a dec3 of 152 bytes", "ec-3", "dec3", 152, {0x06, 0, 0x60, 0x04, 0}, true},
        {"ec-3 of a dec3 of 153 bytes", "ec-3", "dec3", 153, {0x06, 0, 0x60, 0x04, 0}, false},
        {"ec-3 of a dec3 of 10 bytes", "ec-3", "dec3", 10, {0x06, 0, 0x60, 0x04, 0}, false},
        {"ec-3 of a decX", "ec-3", "decX", 13, {0x06, 0, 0x60, 0x04, 0}, false},
        {"ec-3 of 8 substreams", "ec-3", "dec3", 13, {0x06, 0x07, 0x60, 0x04, 0}, false},
        {"ec-3 of 4 substreams in 11 bytes", "ec-3", "dec3", 11,
         {0x06, 0x03, 0x60, 0x04, 0, 0x60, 0x04, 0, 0x60, 0x04, 0, 0x60, 0x04, 0}, false},
        {"ec-3 of a dependent substream and 7 flags", "ec-3", "dec3", 14,
         {0x06, 0, 0x60, 0x04, 0x02, 0x01, 0xfe, 0, 0, 0, 0, 0, 0, 0}, true},
        {"ec-3 of a dec3 of 11 bytes", "ec-3", "dec3", 11, {0x06, 0, 0x60, 0x04, 0, 0, 0, 0, 0, 0, 0}, true},
        {"ec-3 of 3 dependent substreams in 14 bytes", "ec-3", "dec3", 14,
         {0x06, 0x02, 0x60, 0x04, 0x02, 0x01, 0x60, 0x04, 0x02, 0x01, 0x60, 0x04, 0x02, 0x01}, true},
        {"ec-3 of 3 dependent substreams in 13 bytes", "ec-3", "dec3", 13,
         {0x06, 0x02, 0x60, 0x04, 0x02, 0x01, 0x60, 0x04, 0x02, 0x01, 0x60, 0x04, 0x02}, false},
        {"ec-3 of 8 substreams in 24 bytes", "ec-3", "dec3", 24,
         {0, 0x07, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40}, false},
        {"ec-3 of 8 substreams past its 25 bytes", "ec-3", "dec3", 25,
         {0, 0x07, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0xff,
          0xff, 0xff},
         false, minato::mp4::kMalformed},
        {"ec-3 of 8 substreams, the last at the end of its 26 bytes", "ec-3", "dec3", 26,
         {0, 0x07, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0x02, 0x01, 0x40, 0, 0x02, 0x01, 0x40, 0,
          0x02, 0x01, 0xff, 0xff},
         false, minato::mp4::kMalformed},
        {"ac-4", "ac-4", "dac4", 11, {0, 0, 0}, true},
        {"ac-4 of bitstream version 1", "ac-4", "dac4", 11, {0, 0x40, 0}, true},
        {"ac-4 of DSI version 1 and 64 bits of bit rate", "ac-4", "dac4", 19, cat({{0x20, 0, 0}, Bytes(8, 0)}), false},
        {"ac-4 of a program ID", "ac-4", "dac4", 22, cat({{0x20, 0x80, 0, 0x80}, Bytes(10, 0)}), true},
        {"ac-4 of a UUID without its bits", "ac-4", "dac4", 28, cat({{0x20, 0x80, 0, 0x80, 0, 0x40}, Bytes(14, 0)}),
         false},
        {"ac-4 of a UUID 2 bits short", "ac-4", "dac4", 29, cat({{0x20, 0x80, 0, 0x80, 0, 0x40}, Bytes(15, 0)}),
         false},
        {"ac-4 of a UUID, 4 bits short of a bit rate", "ac-4", "dac4", 37,
         cat({{0x20, 0x80, 0, 0x80, 0, 0x40}, Bytes(23, 0)}), false},
        {"ac-4 of a UUID", "ac-4", "dac4", 38, cat({{0x20, 0x80, 0, 0x80, 0, 0x40}, Bytes(24, 0)}), true},
        {"ac-4 of a dac4 of 10 bytes", "ac-4", "dac4", 10, {0, 0, 0}, false},
        {"ac-4 of a dacX", "ac-4", "dacX", 11, {0, 0, 0}, false},
        {"ac-4 of a DSI of 1176 bytes", "ac-4", "dac4", 1184, Bytes(1176, 0), false},
        {"ac-4 of a DSI of 1175 bytes", "ac-4", "dac4", 1183, Bytes(1175, 0), true},
        {"ac-4 of DSI version 2", "ac-4", "dac4", 11, {0x40, 0, 0}, false},
        {"ac-4 of DSI version 1 without its bit rate", "ac-4", "dac4", 11, {0x20, 0, 0}, false},
        {"ac-4 of DSI version 1", "ac-4", "dac4", 20, {0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, true},
        {"ac-4 of DSI version 0 and bitstream version 2", "ac-4", "dac4", 11, {0, 0x80, 0}, false},
        {"ac-4 of DSI version 1 and bitstream version 2", "ac-4", "dac4", 20,
         {0x20, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, true},
        {"ac-4 of a program ID without its bits", "ac-4", "dac4", 12, {0x20, 0x80, 0, 0x80}, false},
        {"ac-4 of DSI version 2 and a bit rate's bits", "ac-4", "dac4", 20, {0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
         false},
        {"ac-4 of DSI version 0, bitstream version 2 and a byte more", "ac-4", "dac4", 12, {0, 0x80, 0, 0}, false},
        {"ac-4 of DSI version 1 and 40 bits of bit rate", "ac-4", "dac4", 16, {0x20, 0, 0, 0, 0, 0, 0, 0}, false},
    };
    for (const Case &test : cases) {
        Bytes header;
        put32(header, test.claim);
        header.insert(header.end(), test.box, test.box + 4);
        Knobs knobs;
        knobs.audio_config = {0x12, 0x10};
        knobs.raw_entry = box(test.entry, cat({sound, header, test.payload}));
        knobs.stsds = {{"raw", "mp4a"}};
        minato::mp4::Tracks tracks;
        const int answer = parsed(built_file(knobs), &tracks);
        const bool right = answer == test.answer && tracks.size() == (test.kept ? 1u : 0u);
        CHECK(right);
        if (!right) std::printf("  %s: answer %d, %zu tracks\n", test.what, answer, tracks.size());
    }
}

TEST(an_aac_profile_reads_as_parse_aac_profile_from_csd) {
    const struct {
        Bytes csd;
        int32_t profile;
    } cases[] = {
        {{0x12, 0x10}, 2},  {{0x08, 0x00}, 1},  {{0xe8, 0x00}, 29}, {{0x38, 0x00}, 0},  {{0xf0, 0x00}, 0},
        {{0xf8, 0xe0}, 39}, {{0xf9, 0x40}, 42}, {{0xf8, 0x00}, 0},  {{0xf9, 0x20}, 0},  {{0x12}, 0},
        {{}, 0},
    };
    for (const auto &test : cases) {
        const int32_t profile = minato::mp4::aac_profile(test.csd.data(), test.csd.size());
        CHECK(profile == test.profile);
        if (profile != test.profile) std::printf("  %zu bytes: %d\n", test.csd.size(), profile);
    }
}

// The AudioSpecificConfig as MPEG4Extractor's updateAudioTrackInfoFromESDS_MPEG4Audio reads it: the rate by its
// index or in 24 bits, channel configurations 7, 11, 12 and 14 as 8, 7, 8 and 8 channels, others above 6
// unsupported (as no channels are), a reserved rate index or a config too short for its fields malformed.
TEST(an_audio_specific_config_reads_as_androids) {
    struct Case {
        Bytes config;
        int answer;
        uint32_t object;
        int32_t rate, channels;
    };
    const Case cases[] = {
        {{0x12, 0x10}, 0, 2, 44100, 2},
        {{0x11, 0xb8}, 0, 2, 48000, 8},
        {{0x11, 0xd8}, 0, 2, 48000, 7},
        {{0x11, 0xc0}, minato::mp4::kUnsupported, 0, 0, 0},
        {{0x16, 0x90}, minato::mp4::kMalformed, 0, 0, 0},
        {{0x17, 0x80, 0x2b, 0x11, 0x08}, 0, 2, 22050, 1},
        {{0x12}, minato::mp4::kMalformed, 0, 0, 0},
        {{0x12, 0x00}, minato::mp4::kMalformed, 0, 0, 0},  // no channel configuration and no program config element
        {{0x0a, 0x00}, minato::mp4::kUnsupported, 0, 0, 0},  // AAC Main without a channel configuration
        {{0xf9, 0x48, 0x40}, 0, 42, 44100, 2},
    };
    for (const Case &test : cases) {
        Knobs knobs;
        knobs.audio_config = test.config;
        minato::mp4::Tracks tracks;
        const int answer = parsed(built_file(knobs), &tracks);
        CHECK(answer == test.answer);
        if (answer != test.answer) std::fprintf(stderr, "  config %02x %02x: %d\n", test.config[0],
                                                test.config.size() > 1 ? test.config[1] : 0, answer);
        if (answer != 0) continue;
        CHECK(tracks.size() == 1);
        if (tracks.size() != 1) continue;
        CHECK(text(tracks[0].mime) == "audio/mp4a-latm" && tracks[0].audio_object == test.object);
        CHECK(tracks[0].sample_rate == test.rate && tracks[0].channels == test.channels);
    }
}

// MPEG4Source takes every AAC sample as a sync sample but USAC's (audio object 42), whose stss it reads.
TEST(usac_audio_seeks_to_its_sync_samples) {
    for (const bool usac : {false, true}) {
        Knobs knobs;
        knobs.audio_config = usac ? Bytes{0xf9, 0x48, 0x40} : Bytes{0x12, 0x10};
        knobs.syncs = {1, 3};
        minato::mp4::Tracks tracks;
        CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
        if (tracks.size() != 1) continue;
        size_t sample = 99;
        CHECK(minato::mp4::seek(tracks[0], 33333, minato::mp4::kSeekPreviousSync, &sample) == 0);
        CHECK(sample == (usac ? 0u : 1u));
    }
}

// SampleTable's seek index takes a running time that a negative composition offset larger than it resets to 0, so
// that the samples after it are earlier by the time it had: here sample 2 is at 1000 ticks and sample 3 at 2000.
TEST(the_seek_index_resets_its_time_as_androids) {
    Knobs knobs;
    knobs.ctts_offsets = {0, -1500, 0, 0};
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() != 1) return;
    size_t sample = 99;
    CHECK(minato::mp4::seek(tracks[0], 66667, minato::mp4::kSeekPreviousSync, &sample) == 0 && sample == 3);
    CHECK(minato::mp4::seek(tracks[0], 33333, minato::mp4::kSeekPreviousSync, &sample) == 0 && sample == 2);
}

// Times past int64_t's range saturate rather than overflow (Android's arithmetic overflows there): an empty edit of
// INT64_MAX ticks, and a seek to INT64_MAX past a shift.
TEST(times_past_int64_saturate) {
    Knobs knobs;
    knobs.timescale = 1;
    knobs.movie_timescale = 1;
    knobs.elst = edit_list(1, {{static_cast<uint64_t>(INT64_MAX), -1}, {40, 0}});
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        CHECK(minato::mp4::sample_time_us(tracks[0], sample_of(tracks[0], 1)) == INT64_MAX);
        size_t sample = 99;
        CHECK(minato::mp4::seek(tracks[0], 0, minato::mp4::kSeekPreviousSync, &sample) == 0 && sample == 0);
    }
    knobs.timescale = 30000;
    knobs.elst = edit_list(0, {{40, 5}});
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        size_t sample = 99;
        CHECK(minato::mp4::seek(tracks[0], INT64_MAX, minato::mp4::kSeekPreviousSync, &sample) == 0 && sample == 3);
    }
}

TEST(encrypted_and_fragmented_files_are_unsupported) {
    Knobs encrypted;
    encrypted.encrypted = true;
    Knobs fragmented;
    fragmented.fragmented = true;
    minato::mp4::Tracks tracks;
    CHECK(parsed(built_file(encrypted), &tracks) == minato::mp4::kUnsupported);
    CHECK(parsed(built_file(fragmented), &tracks) == minato::mp4::kUnsupported);
}

// --sweep: each test file and a built one with every byte changed three ways and cut at every length, each file for
// its share of the time budget; each answer is one of parse's, reading every sample of every track answers within
// its size, and nothing stays held once the tracks go.
int sweep(double seconds) {
    Knobs audio;
    audio.audio_config = {0x12, 0x10};
    Knobs short_lengths;
    short_lengths.nal_size = 2;
    short_lengths.first_sample = {0, 3, 0x65, 1, 2, 0, 0, 0, 5, 0x65, 3, 4, 5, 6};
    const std::vector<Bytes> files = {file_bytes(kAssets + "media-extractor-test/assets/baseline.mp4"),
                                      file_bytes(kAssets + "media-extractor-test/assets/main.mp4"),
                                      built_file(Knobs{}), built_file(audio), built_file(short_lengths)};
    size_t tried = 0, leaks = 0;
    auto exercise = [&](const Bytes &file) {
        const size_t before = held_bytes();
        {
            minato::mp4::Tracks tracks;
            const int answer = minato::mp4::parse(source_of(file), &tracks);
            // kIo as MPEG4Extractor gives it for an mdhd of a version above 1.
            CHECK(answer == 0 || answer == minato::mp4::kMalformed || answer == minato::mp4::kUnsupported ||
                  answer == minato::mp4::kNoMemory || answer == minato::mp4::kIo);
            for (const Track &track : tracks) {
                for (size_t index = 0; index < track.samples.count(); index++) {
                    long read = 0;
                    const Bytes got = read_all(source_of(file), track, index, &read);
                    CHECK(read <= static_cast<long>(got.size()));
                }
            }
        }
        leaks += held_bytes() != before;
        tried++;
    };
    // Once before counting, as in the test above: an unsorted stss makes qsort's first call.
    Knobs unsorted;
    unsorted.syncs = {3, 1};
    exercise(built_file(unsorted));
    leaks = 0;
    for (const Bytes &file : files) {
        const auto share = std::chrono::duration<double>(seconds / static_cast<double>(files.size()));
        const auto deadline = std::chrono::steady_clock::now() + share;
        for (size_t at = 0; at < file.size() && std::chrono::steady_clock::now() < deadline; at++) {
            for (const uint8_t change : {uint8_t{0x00}, uint8_t{0xff}, uint8_t{0x80}}) {
                Bytes changed = file;
                changed[at] ^= change == 0 ? changed[at] : change;
                exercise(changed);
            }
            exercise(Bytes(file.begin(), file.begin() + static_cast<long>(at)));
        }
    }
    std::printf("mp4 sweep: %zu files tried\n", tried);
    CHECK(leaks == 0);
    if (leaks != 0) std::printf("mp4 sweep: %zu files held memory afterwards\n", leaks);
    return apk::testing::failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--sweep") == 0) return sweep(std::atof(argv[2]));
    return apk::testing::run_all();
}
