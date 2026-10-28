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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
    CHECK(track.samples.size() == units.size());
    if (track.samples.size() != units.size()) return;
    uint32_t largest = 0;
    for (size_t index = 0; index < units.size(); index++) {
        const auto &sample = track.samples[index];
        CHECK(sample.composition() == units[index].frame * 1000 + shift * 1000);
        CHECK(track.sync[index] == units[index].key);
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
    for (size_t index = 0; index < tracks[0].samples.size() && index < units.size(); index++) {
        long answer = 0;
        const Bytes got = read_all(source, tracks[0], index, &answer);
        CHECK(answer == static_cast<long>(got.size()) && got == expected_sample(units[index]));
    }
    const Track &audio = tracks[1];
    CHECK(audio.mime == "audio/mp4a-latm");
    CHECK(audio.sample_rate == 44100 && audio.channels == 1 && audio.timescale == 44100);
    CHECK((audio.csd0 == Bytes{0x12, 0x08}));  // AAC LC, 44.1 kHz, mono
    CHECK(audio.nal_length_size == 0 && audio.edit_media_time == 2112);  // the encoder's priming
    CHECK(audio.samples.size() == 46);
    for (size_t index = 0; index < audio.samples.size(); index++) {
        CHECK(audio.sync[index] && audio.samples[index].decode == static_cast<int64_t>(index) * 1024);
        long answer = 0;
        const Bytes got = read_all(source, audio, index, &answer);
        CHECK(answer == static_cast<long>(audio.samples[index].size) && got.size() == audio.samples[index].size);
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
    for (size_t index = 0; index < tracks[0].samples.size() && index < units.size(); index++) {
        CHECK(tracks[0].samples[index].decode == static_cast<int64_t>(index) * 1000);
        long answer = 0;
        CHECK(read_all(source, tracks[0], index, &answer) == expected_sample(units[index]));
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
    put32(stts, 1);
    put32(stts, constant != 0 ? constant : knobs.stts_count != 0 ? knobs.stts_count : count);
    put32(stts, 1000);
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
    put32(stco, knobs.chunk_past_end ? 0x7fffff00u : static_cast<uint32_t>(data));
    Bytes ctts = full(0);
    put32(ctts, 1);
    put32(ctts, count);
    put32(ctts, 0);
    Bytes stss = full(0);
    put32(stss, 1);
    put32(stss, 1);
    // A second stsz of another kind than the first: a constant size over many samples, past the first's entries.
    Bytes other_stsz = full(0);
    put32(other_stsz, 1);
    put32(other_stsz, 100000);
    Bytes tables = cat({stsd, box("stts", stts), box("stsc", stsc)});
    if (knobs.stsz) tables = cat({tables, box("stsz", stsz)});
    tables = cat({tables, box("stco", stco)});
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
        CHECK(tracks[0].samples.size() == 4 && tracks[0].csd0 == (Bytes{0, 0, 0, 1, 0x67, 66, 0, 30}));
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
        if (tracks.size() == 1) CHECK(tracks[0].samples.size() < 4);
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

// The samples of all tracks together stay within kMaxSamples, counted before they are made: a file of many tracks of
// constant-size samples cannot make each its own kMaxSamples. The source is long enough to hold them all.
TEST(the_tracks_share_one_sample_budget) {
    Knobs knobs;
    knobs.constant_count = static_cast<uint32_t>(minato::mp4::kMaxSamples / 2 + 1);
    const Bytes one = built_file(knobs);
    std::vector<Track> tracks;
    CHECK(minato::mp4::parse(Source{const_cast<Bytes *>(&one), read_padded, 1ull << 40}, &tracks) == 0 &&
          tracks.size() == 1);
    knobs.tracks = 2;
    const Bytes two = built_file(knobs);
    CHECK(minato::mp4::parse(Source{const_cast<Bytes *>(&two), read_padded, 1ull << 40}, &tracks) ==
          minato::mp4::kMalformed);
}

// A Sample costs 24 bytes and the samples of a file at most 48 MiB: a file of 1-byte constant-size samples asks
// for 24 bytes for each of its bytes, so the cap is what bounds it.
TEST(the_samples_of_a_file_stay_within_48_mib) {
    CHECK(sizeof(minato::mp4::Sample) <= 24);
    CHECK(minato::mp4::kMaxSamples * sizeof(minato::mp4::Sample) <= (48u << 20));
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
            for (size_t index = 0; index < track.samples.size(); index++) {
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
