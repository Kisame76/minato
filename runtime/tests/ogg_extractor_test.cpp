// ADR-0060 e2: the Ogg Opus reader of libmediandk's AMediaExtractor (runtime/android/ogg_extractor.cpp), on the
// host. media-player-ogg-test's Ogg Opus files against their own pages, split here by their lacing values; then
// files of pages this test builds (OggExtractor's reading of a code 3 packet's count, a packet across pages, Vorbis,
// a stream that is not Opus); and the mono file with each byte changed and cut at each length: never a crash, a read
// past a buffer (ASan) or memory held afterwards.
#include "ogg_extractor.h"

#include "check.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace ogg = minato::ogg;
using Bytes = std::vector<uint8_t>;

Bytes file_bytes(const std::string &name) {
    std::ifstream in(std::string(OGG_DIR) + "/" + name, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ssize_t read_memory(void *context, uint64_t offset, void *data, size_t size) {
    const auto *bytes = static_cast<const Bytes *>(context);
    if (offset >= bytes->size()) return -1;  // as a MediaDataSource answers past its end
    const size_t got = std::min<uint64_t>(size, bytes->size() - offset);
    std::memcpy(data, bytes->data() + offset, got);
    return static_cast<ssize_t>(got);
}

struct Opened {
    Bytes bytes;
    minato::mp4::Source source;
    ogg::Kind kind = ogg::Kind::kNotOgg;
    ogg::Reader *reader = nullptr;
    int answer = 0;

    Opened(Bytes file, bool sized) : bytes(std::move(file)) {
        source = minato::mp4::Source{&bytes, read_memory, bytes.size()};
        answer = ogg::open(source, sized, &kind, &reader);
    }
    ~Opened() { ogg::close(reader); }
};

struct Read {
    Bytes data;
    int64_t time_us;
};

// The packets to the end, at most `most`: a first data page's granule position of 2^63 or more reads its first
// packet again and again, as OggExtractor does (its start granule position stays below 0).
std::vector<Read> packets(ogg::Reader *reader, int *ended = nullptr, size_t most = 100000) {
    std::vector<Read> out;
    ogg::Packet packet{};
    int answer = 0;
    while (out.size() < most && (answer = ogg::next(reader, &packet)) == 0) {
        out.push_back(Read{Bytes(packet.data, packet.data + packet.size), packet.time_us});
    }
    if (ended != nullptr) *ended = answer;
    return out;
}

// The file's packets by its pages' lacing values, the way RFC 3533 joins them.
std::vector<Bytes> laced(const Bytes &file) {
    std::vector<Bytes> out;
    Bytes partial;
    for (size_t at = 0; at + 27 <= file.size() && std::memcmp(&file[at], "OggS", 4) == 0;) {
        const size_t segments = file[at + 26];
        size_t body = at + 27 + segments;
        for (size_t i = 0; i < segments; i++) {
            const size_t lace = file[at + 27 + i];
            partial.insert(partial.end(), file.begin() + static_cast<long>(body),
                           file.begin() + static_cast<long>(body + lace));
            body += lace;
            if (lace < 255) {
                out.push_back(partial);
                partial.clear();
            }
        }
        at = body;
    }
    return out;
}

// A page of `packets` (lacing values made here), its first continued from the page before where `continued`.
void page(Bytes &file, uint8_t flags, uint64_t granule, uint32_t number, const std::vector<Bytes> &packets,
          bool open_last = false) {
    Bytes lace, body;
    for (size_t index = 0; index < packets.size(); index++) {
        const Bytes &packet = packets[index];
        size_t left = packet.size();
        while (left >= 255) {
            lace.push_back(255);
            left -= 255;
        }
        if (!(open_last && index + 1 == packets.size())) lace.push_back(static_cast<uint8_t>(left));
        body.insert(body.end(), packet.begin(), packet.end());
    }
    const uint8_t header[] = {'O', 'g', 'g', 'S', 0, flags};
    file.insert(file.end(), header, header + sizeof header);
    for (int i = 0; i < 8; i++) file.push_back(static_cast<uint8_t>(granule >> (8 * i)));
    for (int i = 0; i < 4; i++) file.push_back(0x11);  // serial
    for (int i = 0; i < 4; i++) file.push_back(static_cast<uint8_t>(number >> (8 * i)));
    for (int i = 0; i < 4; i++) file.push_back(0);  // CRC, which OggExtractor does not check
    file.push_back(static_cast<uint8_t>(lace.size()));
    file.insert(file.end(), lace.begin(), lace.end());
    file.insert(file.end(), body.begin(), body.end());
}

Bytes opus_head(uint8_t channels, uint16_t pre_skip) {
    Bytes head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, channels, static_cast<uint8_t>(pre_skip),
                  static_cast<uint8_t>(pre_skip >> 8), 0x80, 0xbb, 0, 0, 0, 0, 0};
    return head;
}

Bytes opus_tags() {
    return {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 2, 0, 0, 0, 'm', 't', 0, 0, 0, 0};
}

// The headers' pages of a built file.
Bytes headers(uint16_t pre_skip = 0) {
    Bytes file;
    page(file, 2, 0, 0, {opus_head(2, pre_skip)});
    page(file, 0, 0, 1, {opus_tags()});
    return file;
}

TEST(opus_ogg_has_its_track_as_opus_extractor_gives_it) {
    Opened file(file_bytes("opus.ogg"), true);
    CHECK(file.answer == 0 && file.kind == ogg::Kind::kOpus && file.reader != nullptr);
    const ogg::Format &format = ogg::format(file.reader);
    const Bytes head = laced(file.bytes)[0];
    CHECK(format.head_size == head.size() && std::memcmp(format.head, head.data(), head.size()) == 0);
    CHECK(format.channels == 2);
    CHECK(format.delay_ns == 312 * 1000000000LL / 48000);
    CHECK(format.pre_roll_ns == 80000000);
    CHECK(format.duration_us == 1200000);  // the last granule position, 57912, less the pre-skip of 312
}

TEST(packets_are_the_files_own_at_their_granule_times) {
    Opened file(file_bytes("opus.ogg"), true);
    const std::vector<Bytes> expected = laced(file.bytes);
    int ended = 0;
    const std::vector<Read> got = packets(file.reader, &ended);
    CHECK(ended == minato::mp4::kEndOfStream);
    CHECK(got.size() + 2 == expected.size());  // OpusHead and OpusTags left out
    int64_t frames = 0;
    for (size_t index = 0; index < got.size() && index + 2 < expected.size(); index++) {
        CHECK(got[index].data == expected[index + 2]);
        CHECK((got[index].data[0] & 3) != 3);  // the stereo file's packets count by their codes
        const int64_t pcm = frames > 312 ? frames - 312 : 0;
        CHECK(got[index].time_us == pcm * 1000000 / 48000);
        frames += 960;
    }
}

TEST(zeros_after_the_last_page_keep_the_duration) {
    Bytes file = file_bytes("opus-mono.ogg");
    file.resize(6000);  // as media-extractor-ogg-test's copy of its first 6000 bytes
    Opened opened(file, true);
    CHECK(opened.kind == ogg::Kind::kOpus);
    CHECK(opened.reader != nullptr && ogg::format(opened.reader).duration_us == 900000);
}

TEST(without_a_size_there_is_no_duration_and_no_seek) {
    Opened file(file_bytes("opus.ogg"), false);
    CHECK(file.kind == ogg::Kind::kOpus);
    CHECK(ogg::format(file.reader).duration_us == -1);
    CHECK(ogg::seek(file.reader, 600000) == minato::mp4::kUnknown);
    CHECK(packets(file.reader).size() == 61);
}

TEST(a_seek_goes_80_ms_back_to_a_page_of_the_table) {
    Opened file(file_bytes("opus.ogg"), true);
    const std::vector<Read> all = packets(file.reader);
    CHECK(ogg::seek(file.reader, 600000) == 0);
    ogg::Packet packet{};
    CHECK(ogg::next(file.reader, &packet) == 0);
    CHECK(packet.time_us <= 520000);
    bool found = false;
    for (const Read &read : all) {
        found |= read.time_us == packet.time_us && read.data == Bytes(packet.data, packet.data + packet.size);
    }
    CHECK(found);
    CHECK(ogg::seek(file.reader, 0) == 0);
    CHECK(ogg::next(file.reader, &packet) == 0 && packet.time_us == 0);
    CHECK(Bytes(packet.data, packet.data + packet.size) == all[0].data);
}

TEST(a_code_3_packet_counts_its_frames_by_its_third_byte_as_android_does) {
    Bytes file = headers();
    // Code 3 with 2 frames of 10 ms by its second byte, 5 by its third; then a packet of 10 ms.
    const Bytes code3 = {0x03, 0x02, 0x05, 0xaa, 0xbb}, code0 = {0x00, 0xcc};
    page(file, 4, 1440, 2, {code3, code0});
    Opened opened(file, true);
    CHECK(opened.kind == ogg::Kind::kOpus);
    const std::vector<Read> got = packets(opened.reader);
    CHECK(got.size() == 2);
    CHECK(got.size() == 2 && got[0].time_us == 0);
    CHECK(got.size() == 2 && got[1].time_us == 50000);  // after 2400 frames, not 960
}

TEST(a_packet_across_pages_is_one_packet) {
    Bytes file = headers();
    Bytes long_packet(600, 0x08);  // config 1, code 0
    Bytes first(long_packet.begin(), long_packet.begin() + 510), rest(long_packet.begin() + 510, long_packet.end());
    page(file, 0, static_cast<uint64_t>(-1), 2, {first}, true);
    page(file, 5, 960, 3, {rest});
    Opened opened(file, true);
    const std::vector<Read> got = packets(opened.reader);
    CHECK(got.size() == 1 && got[0].data == long_packet);
}

// Gap 13: media-player-ogg-test's tone.ogg (Ogg Vorbis, stereo, 22050 Hz, 7 audio packets on one page of 5568 samples)
// as OggExtractor reads it [verified: media-extractor-vorbis-test on Android 15]: the packets' times, the 4 bytes
// that follow each (the page's samples after its first packet, else -1) and the format.
TEST(vorbis_file_has_its_track_and_times) {
    Opened opened(file_bytes("tone.ogg"), true);
    CHECK(opened.kind == ogg::Kind::kVorbis && opened.reader != nullptr);
    const ogg::Format &format = ogg::format(opened.reader);
    CHECK(format.vorbis && format.channels == 2 && format.sample_rate == 22050 && format.head_size == 30);
    CHECK(format.head[0] == 1 && format.setup != nullptr && format.setup[0] == 5);
    const std::vector<Read> got = packets(opened.reader);
    const int64_t times[] = {0, 0, 46439, 92879, 139319, 185759, 232199};
    const size_t sizes[] = {241, 79, 88, 86, 89, 233, 264};
    CHECK(got.size() == 7);
    for (size_t i = 0; i < got.size() && i < 7; i++) {
        CHECK(got[i].time_us == times[i] && got[i].data.size() == sizes[i]);
        int32_t after;
        std::memcpy(&after, &got[i].data[got[i].data.size() - 4], 4);
        CHECK(after == (i == 0 ? 5568 : -1));
    }
}

TEST(a_broken_vorbis_stream_and_another_stream_have_no_track) {
    Bytes vorbis;
    page(vorbis, 2, 0, 0, {Bytes{1, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0}});
    CHECK(Opened(vorbis, true).kind == ogg::Kind::kNoTrack);
    Bytes other;
    page(other, 2, 0, 0, {Bytes{'X', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0}});
    page(other, 0, 0, 1, {opus_tags()});
    Opened none(other, true);
    CHECK(none.kind == ogg::Kind::kNoTrack && none.reader == nullptr && none.answer == 0);
    CHECK(Opened(Bytes{'R', 'I', 'F', 'F', 0, 0}, true).kind == ogg::Kind::kNotOgg);
    Bytes tags = headers();
    tags[tags.size() - 10] = 0xff;  // OpusTags' vendor length past the packet
    CHECK(Opened(tags, true).kind == ogg::Kind::kNoTrack);
}

TEST(a_first_page_past_2_to_the_63_gives_its_first_packet_again_as_android_does) {
    Bytes file = file_bytes("opus-mono.ogg");
    file[134] = 0xff;  // the first data page's granule position's top byte
    Opened opened(file, true);
    const std::vector<Read> got = packets(opened.reader, nullptr, 5);
    CHECK(got.size() == 5 && got[4].data == got[0].data);
}

TEST(every_changed_or_cut_mono_file_reads_safely) {
    const Bytes file = file_bytes("opus-mono.ogg");
    size_t tried = 0;
    const auto exercise = [&](const Bytes &bytes) {
        for (const bool sized : {true, false}) {
            Opened opened(bytes, sized);
            tried++;
            if (opened.reader == nullptr) continue;
            packets(opened.reader, nullptr, 200);
            ogg::seek(opened.reader, 300000);
            packets(opened.reader, nullptr, 200);
        }
    };
    for (size_t at = 0; at < file.size(); at++) {
        for (const uint8_t change : {uint8_t{0x00}, uint8_t{0xff}, uint8_t{0x80}}) {
            Bytes changed = file;
            changed[at] ^= change == 0 ? changed[at] : change;
            exercise(changed);
        }
        exercise(Bytes(file.begin(), file.begin() + static_cast<long>(at)));
    }
    CHECK(tried > 4 * file.size());
}

}  // namespace

int main() { return apk::testing::run_all(); }
