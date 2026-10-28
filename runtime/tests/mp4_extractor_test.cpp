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
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <random>
#include <string>
#include <vector>

// The bytes operator new hands out while `counting` is set: what a parse allocates.
size_t allocated = 0;
bool counting = false;

void *operator new(size_t size) {
    if (counting) allocated += size;
    if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void *operator new[](size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, size_t) noexcept { std::free(memory); }

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
    CHECK(track.mime == "video/avc");
    CHECK(track.width == 320 && track.height == 180);
    CHECK(track.timescale == 30000 && track.nal_length_size == 4);
    CHECK(track.edit_media_time == shift * 1000 && track.edit_empty == 0);
    CHECK(track.language == "und");
    std::vector<Bytes> sets = nal_units(units[0].bytes);
    Bytes sps, pps;
    for (const Bytes &nal : sets) {
        if ((nal[0] & 0x1f) == 7 && sps.empty()) sps = with_start_code(nal);
        if ((nal[0] & 0x1f) == 8 && pps.empty()) pps = with_start_code(nal);
    }
    CHECK(track.csd0 == sps && track.csd1 == pps);
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
    std::vector<Track> tracks;
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
    CHECK(audio.mime == "audio/mp4a-latm");
    CHECK(audio.sample_rate == 44100 && audio.channels == 1 && audio.timescale == 44100);
    CHECK((audio.csd0 == Bytes{0x12, 0x08}));  // AAC LC, 44.1 kHz, mono
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
    std::vector<Track> tracks;
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
        std::vector<Track> tracks;
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
        put32(mdat_body, index == 0 ? knobs.nal_length : 12);
        mdat_body.push_back(0x65);  // an IDR slice's header
        for (uint32_t byte = 0; byte < 11; byte++) mdat_body.push_back(static_cast<uint8_t>(index * 16 + byte));
    }
    const Bytes ftyp = box("ftyp", cat({{'i', 's', 'o', 'm'}, {0, 0, 2, 0}, {'i', 's', 'o', 'm'}}));
    // avcC: version 1, profile 66, compat 0, level 30, length size 4, one SPS and one PPS.
    const Bytes avcc = box("avcC", {1, 66, 0, 30, 0xff, 0xe1, 0, 4, 0x67, 66, 0, 30, 1, 0, 2, 0x68, 0xce});
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
    if (knobs.encrypted) visual = cat({visual, box("sinf", box("frma", {'a', 'v', 'c', '1'}))});
    const Bytes stsd = box("stsd", cat({full(0), {0, 0, 0, 1}, box(knobs.encrypted ? "encv" : "avc1", visual)}));
    const uint32_t constant = knobs.constant_count;
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
        for (uint32_t index = 0; index < entries; index++) put32(stsz, index == 0 ? knobs.first_size : 16);
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
    tables = cat({tables, box(knobs.chunk_offset64 != 0 ? "co64" : "stco", stco)});
    if (!knobs.syncs.empty()) tables = cat({tables, box("stss", syncs)});
    if (knobs.repeat == "stsz") tables = cat({tables, box("stsz", other_stsz)});
    if (knobs.repeat == "stco") tables = cat({tables, box("stco", stco)});
    if (knobs.repeat == "stsc") tables = cat({tables, box("stsc", stsc)});
    if (knobs.repeat == "stts") tables = cat({tables, box("stts", stts)});
    if (knobs.repeat == "ctts") tables = cat({tables, box("ctts", ctts), box("ctts", ctts)});
    if (knobs.repeat == "stss") tables = cat({tables, box("stss", stss), box("stss", stss)});
    const Bytes stbl = box("stbl", tables);
    Bytes mdhd = full(0);
    put32(mdhd, 0);
    put32(mdhd, 0);
    put32(mdhd, knobs.timescale);
    put32(mdhd, count * 1000);
    put16(mdhd, 0x55c4);  // "und"
    put16(mdhd, 0);
    Bytes hdlr = full(0);
    put32(hdlr, 0);
    hdlr.insert(hdlr.end(), {'v', 'i', 'd', 'e'});
    hdlr.insert(hdlr.end(), 13, 0);
    const Bytes minf = box("minf", cat({box("vmhd", cat({full(1), Bytes(8, 0)})), stbl}));
    const Bytes mdia = box("mdia", knobs.mdhd ? cat({box("mdhd", mdhd), box("hdlr", hdlr), minf})
                                              : cat({box("hdlr", hdlr), minf}));
    Bytes tkhd = full(3);
    tkhd.insert(tkhd.end(), 8, 0);
    put32(tkhd, 1);  // track ID
    tkhd.insert(tkhd.end(), 68, 0);
    Bytes nested;
    for (int level = 0; level < knobs.depth; level++) nested = box("udta", nested);
    Bytes mvhd = full(0);
    mvhd.insert(mvhd.end(), 8, 0);  // creation and modification times
    put32(mvhd, knobs.movie_timescale);
    mvhd.insert(mvhd.end(), 84, 0);
    const Bytes edts = knobs.elst.empty() ? Bytes{} : box("edts", box("elst", knobs.elst));
    const Bytes trak = box("trak", cat({box("tkhd", tkhd), edts, mdia}));
    Bytes moov_body = box("mvhd", mvhd);
    for (int track = 0; track < knobs.tracks; track++) moov_body = cat({moov_body, trak});
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

int parsed(const Bytes &file, std::vector<Track> *tracks) {
    return minato::mp4::parse(source_of(file), tracks);
}

TEST(a_built_file_parses_and_reads) {
    for (const bool largesize : {false, true}) {
        Knobs knobs;
        knobs.largesize = largesize;
        const Bytes file = built_file(knobs);
        std::vector<Track> tracks;
        CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
        if (tracks.size() != 1) continue;
        CHECK(tracks[0].samples.count() == 4 && tracks[0].csd0 == (Bytes{0, 0, 0, 1, 0x67, 66, 0, 30}));
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
        std::vector<Track> tracks;
        const int answer = parsed(built_file(knobs), &tracks);
        CHECK(answer == 0 && tracks.size() == 1);
        if (tracks.size() == 1) CHECK(tracks[0].samples.count() < 4);
    }
}

TEST(counts_a_box_cannot_hold_and_sizes_past_the_end_are_malformed) {
    Knobs huge;
    huge.stsz_box_count = 0xffffffffu;
    std::vector<Track> tracks;
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
        std::vector<Track> tracks;
        const int answer = parsed(cut, &tracks);
        CHECK(answer == minato::mp4::kMalformed || answer == 0);
    }
}

TEST(reads_past_the_file_or_the_sample_are_refused) {
    Knobs past;
    past.chunk_past_end = true;
    std::vector<Track> tracks;
    const Bytes file = built_file(past);
    CHECK(parsed(file, &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        long answer = 0;
        read_all(source_of(file), tracks[0], 0, &answer);
        CHECK(answer == minato::mp4::kMalformed || answer == minato::mp4::kIo);
    }
    Knobs long_nal;
    long_nal.nal_length = 13;  // one byte past the sample
    const Bytes other = built_file(long_nal);
    CHECK(parsed(other, &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) {
        long answer = 0;
        read_all(source_of(other), tracks[0], 0, &answer);
        CHECK(answer == minato::mp4::kMalformed);
        Bytes small(4);
        CHECK(minato::mp4::read_sample(source_of(other), tracks[0], 1, small.data(), small.size()) ==
              minato::mp4::kNoMemory);
        CHECK(minato::mp4::output_size(tracks[0], 99) < 0);
    }
}

TEST(a_track_with_a_sample_larger_than_androids_input_buffer_is_refused) {
    // MPEG4Source::start refuses a track whose largest sample plus 20 bytes is above 64 MiB, so a size field
    // never decides how much a reader allocates.
    std::vector<Track> tracks;
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
        std::vector<Track> tracks;
        const int answer = parsed(built_file(knobs), &tracks);
        CHECK(answer == minato::mp4::kMalformed);
        if (answer != minato::mp4::kMalformed) std::fprintf(stderr, "  %s twice: %d\n", table, answer);
    }
}

// As Android's verifyTrack: a track without a timescale (none, or no mdhd), without its sample tables or without
// its codec configuration is left out, and the file still parses.
TEST(a_track_without_a_timescale_tables_or_configuration_is_left_out) {
    Knobs no_timescale;
    no_timescale.timescale = 0;
    Knobs no_mdhd;
    no_mdhd.mdhd = false;
    Knobs no_avcc;
    no_avcc.avcc = false;
    Knobs no_stsz;
    no_stsz.stsz = false;
    Knobs two;
    two.tracks = 2;
    two.timescale = 0;
    for (const Knobs &knobs : {no_timescale, no_mdhd, no_avcc, no_stsz, two}) {
        std::vector<Track> tracks;
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
    std::vector<Track> tracks;
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
// asked for: 2,097,152 samples of a file of about 1 KiB allocate far less than one byte a sample.
TEST(parsing_allocates_no_memory_per_sample) {
    Knobs knobs;
    knobs.constant_count = 1u << 21;
    const Bytes file = built_file(knobs);
    std::vector<Track> tracks;
    allocated = 0;
    counting = true;
    const int answer = minato::mp4::parse(Source{const_cast<Bytes *>(&file), read_padded, 1ull << 40}, &tracks);
    counting = false;
    CHECK(answer == 0 && tracks.size() == 1);
    CHECK(allocated < (64u << 10));
    if (allocated >= (64u << 10)) std::fprintf(stderr, "  allocated %zu bytes\n", allocated);
}

// A chunk near the end of the 64-bit range: a sample whose bytes would end past it is malformed when it is asked for,
// as SampleIterator answers it, and the file and its other samples stay.
TEST(a_sample_past_the_64_bit_range_fails_alone) {
    Knobs knobs;
    knobs.chunk_offset64 = ~uint64_t{0} - 39;  // samples of 16 bytes: the third ends past the range
    const Bytes file = built_file(knobs);
    std::vector<Track> tracks;
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
    std::vector<Track> tracks;
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
        std::vector<Track> tracks;
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
    std::vector<Track> tracks;
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
    std::vector<Track> tracks;
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
    std::vector<Track> tracks;
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
        std::vector<Track> tracks;
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
    std::vector<Track> tracks;
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
            std::vector<Track> tracks;
            const int answer = minato::mp4::parse(Source{&sparse, read_sparse, length}, &tracks);
            CHECK(data > cap ? answer == minato::mp4::kMalformed : answer == 0 && tracks.size() == 1);
        }
    }
    // After the moov (no ftyp, so the walk reaches it), a large box is not read and ends the walk.
    Sparse after{{{0, rest}, {rest.size(), header("free", cap + 1)}}};
    std::vector<Track> tracks;
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
// from the movie's timescale into the track's; and one whose conversion overflows leaves the track out (getTrack
// fails for it on Android).
TEST(an_edit_list_reads_as_android_reads_it) {
    Knobs knobs;
    knobs.movie_timescale = 7;
    knobs.timescale = 10;
    knobs.elst = edit_list(0, {{2, -1}, {40, 5}});
    std::vector<Track> tracks;
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(tracks[0].edit_empty == 3 && tracks[0].edit_media_time == 5);  // (2*10+3)/7
    knobs.elst = edit_list(0, {{2, -1}, {40, 5}, {40, 6}});
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.size() == 1);
    if (tracks.size() == 1) CHECK(tracks[0].edit_empty == 0 && tracks[0].edit_media_time == 0);
    knobs.timescale = 30000;
    knobs.elst = edit_list(1, {{1ull << 63, -1}, {40, 5}});
    CHECK(parsed(built_file(knobs), &tracks) == 0 && tracks.empty());
}

TEST(encrypted_and_fragmented_files_are_unsupported) {
    Knobs encrypted;
    encrypted.encrypted = true;
    Knobs fragmented;
    fragmented.fragmented = true;
    std::vector<Track> tracks;
    CHECK(parsed(built_file(encrypted), &tracks) == minato::mp4::kUnsupported);
    CHECK(parsed(built_file(fragmented), &tracks) == minato::mp4::kUnsupported);
}

// --sweep: each test file and a built one with every byte changed three ways and cut at every length, each file for
// its share of the time budget; each answer is one of parse's, and reading every sample of every track answers within
// its size.
int sweep(double seconds) {
    const std::vector<Bytes> files = {file_bytes(kAssets + "media-extractor-test/assets/baseline.mp4"),
                                      file_bytes(kAssets + "media-extractor-test/assets/main.mp4"),
                                      built_file(Knobs{})};
    size_t tried = 0;
    auto exercise = [&](const Bytes &file) {
        std::vector<Track> tracks;
        const int answer = minato::mp4::parse(source_of(file), &tracks);
        CHECK(answer == 0 || answer == minato::mp4::kMalformed || answer == minato::mp4::kUnsupported ||
              answer == minato::mp4::kNoMemory);
        for (const Track &track : tracks) {
            for (size_t index = 0; index < track.samples.count(); index++) {
                long read = 0;
                const Bytes got = read_all(source_of(file), track, index, &read);
                CHECK(read <= static_cast<long>(got.size()));
            }
        }
        tried++;
    };
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
    return apk::testing::failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--sweep") == 0) return sweep(std::atof(argv[2]));
    return apk::testing::run_all();
}
