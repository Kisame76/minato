// ADR-0060 e2f: audio_codec (APKRT_NR_AUDIO_CODEC) through the real dispatcher, on soundpool-codec-test's tone.aac
// (tools/make-tone-samples.sh: a 440 Hz sine of half amplitude, 0.5 s, stereo at 44100 Hz, AAC-LC in ADTS). Its
// units, without their ADTS headers, and the AudioSpecificConfig the header names are what MediaCodec's decoder gets
// from an extractor: each unit gives 1024 frames, the encoder's delay included, and together the sine; a flush
// forgets the units before it; a unit that gives nothing leaves the codec working; a long configuration and an HE-AAC
// one are read whole; and the ops, types, IDs, sizes, addresses (memory the guest cannot write among them) and the
// codec limit are checked.

#include "syscall_frame.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "check.h"

namespace {

constexpr long ebadf = 9, efault = 14, ebusy = 16, einval = 22, enospc = 28;

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

long codec_call(std::uint64_t op, std::uint64_t argument) {
    apkrt_syscall_frame frame{};
    frame.x[0] = op;
    frame.x[1] = argument;
    frame.x[8] = APKRT_NR_AUDIO_CODEC;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

/// tone.aac's units and the AudioSpecificConfig of its first ADTS header.
struct Stream {
    std::vector<std::vector<unsigned char>> units;
    unsigned char config[2] = {};
};

Stream tone() {
    std::ifstream in(std::string(TONE_DIR) + "/tone.aac", std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    Stream stream;
    for (std::size_t at = 0; at + 7 <= bytes.size();) {
        const unsigned char *header = bytes.data() + at;
        const std::size_t length = (header[3] & 3u) << 11 | unsigned{header[4]} << 3 | unsigned{header[5]} >> 5;
        const std::size_t header_length = (header[1] & 1) != 0 ? 7 : 9;  // without or with a CRC
        if (length <= header_length || at + length > bytes.size()) break;
        if (stream.units.empty()) {
            const unsigned profile = (unsigned{header[2]} >> 6) + 1u, rate_index = unsigned{header[2]} >> 2 & 15u;
            const unsigned channels = (header[2] & 1u) << 2 | unsigned{header[3]} >> 6;
            stream.config[0] = static_cast<unsigned char>(profile << 3 | rate_index >> 1);
            stream.config[1] = static_cast<unsigned char>((rate_index & 1u) << 7 | channels << 3);
        }
        stream.units.emplace_back(header + header_length, header + length);
        at += length;
    }
    return stream;
}

/// A CREATE of type AAC with `config`; the answer, and the struct in `call`.
long create(const unsigned char *config, std::uint64_t size, apkrt_audio_codec &call) {
    call = {};
    call.type = APKRT_AUDIO_CODEC_AAC;
    call.data = address(config);
    call.size = size;
    return codec_call(APKRT_AUDIO_CODEC_CREATE, address(&call));
}

/// A DECODE of `unit` into `frames`; the answer, and the frames written in `call.frames`.
long decode(std::uint64_t id, const std::vector<unsigned char> &unit, std::vector<std::int16_t> &frames,
            apkrt_audio_codec &call) {
    call = {};
    call.id = id;
    call.data = address(unit.data());
    call.size = unit.size();
    call.out = address(frames.data());
    call.capacity = frames.size() * sizeof(std::int16_t);
    return codec_call(APKRT_AUDIO_CODEC_DECODE, address(&call));
}

TEST(aac_units_give_the_sine) {
    const Stream stream = tone();
    CHECK(stream.units.size() == 24);  // 2112 frames of delay, 22050 of the sine, the padding: 24 units of 1024
    apkrt_audio_codec made{};
    CHECK(create(stream.config, 2, made) == 0);
    CHECK(made.id != 0 && made.rate == 44100 && made.channels == 2 && made.frames == 1024);
    std::vector<std::int16_t> all, frames(2048);
    bool each = true;
    for (const auto &unit : stream.units) {
        apkrt_audio_codec call{};
        each = each && decode(made.id, unit, frames, call) == 0 && call.frames == 1024;
        all.insert(all.end(), frames.begin(), frames.begin() + 2 * static_cast<std::ptrdiff_t>(call.frames));
    }
    CHECK(each && all.size() == 2 * 24 * 1024u);
    // 0.2 s from 0.18 s into the sine, which starts after the encoder's 2112 frames: the first channel's RMS that of a
    // sine of half amplitude, both channels alike, and 440 Hz by the rising zero crossings.
    const std::size_t first = 2112 + 44100 * 18 / 100, last = first + 44100 / 5;
    double sum = 0, difference = 0;
    int crossings = 0;
    for (std::size_t i = first; each && i < last; ++i) {
        const double left = all[2 * i], right = all[2 * i + 1];
        sum += left * left;
        difference += std::fabs(left - right);
        if (all[2 * (i - 1)] < 0 && all[2 * i] >= 0) ++crossings;
    }
    const double count = static_cast<double>(last - first);
    const double rms = std::sqrt(sum / count), expected = 0.5 * 32767 / std::sqrt(2.0);
    const bool sine = std::fabs(rms - expected) < 0.1 * expected && difference / count < 0.01 * expected &&
                      crossings >= 86 && crossings <= 90;  // 0.2 s of 440 Hz: 88
    CHECK(sine);
    if (!sine) std::printf("  rms %.0f, difference %.1f, %d crossings\n", rms, difference / count, crossings);
    // The encoder's delay is near silence.
    double delay = 0;
    for (std::size_t i = 0; each && i < 2 * 1024; ++i) delay += std::fabs(all[i]);
    CHECK(each && delay / 2048 < 0.01 * expected);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
}

TEST(a_flush_forgets_the_units_before_it) {
    const Stream stream = tone();
    apkrt_audio_codec made{};
    CHECK(create(stream.config, 2, made) == 0);
    std::vector<std::int16_t> frames(2048);
    // From the sine's middle, as a seek's units: what a codec gives after a flush is what a new one gives.
    std::vector<std::int16_t> fresh;
    for (std::size_t unit = 10; unit < 13; ++unit) {
        apkrt_audio_codec call{};
        CHECK(decode(made.id, stream.units[unit], frames, call) == 0);
        fresh.insert(fresh.end(), frames.begin(), frames.end());
    }
    for (std::size_t unit = 3; unit < 8; ++unit) {
        apkrt_audio_codec call{};
        CHECK(decode(made.id, stream.units[unit], frames, call) == 0);
    }
    CHECK(codec_call(APKRT_AUDIO_CODEC_FLUSH, made.id) == 0);
    std::vector<std::int16_t> flushed;
    for (std::size_t unit = 10; unit < 13; ++unit) {
        apkrt_audio_codec call{};
        CHECK(decode(made.id, stream.units[unit], frames, call) == 0 && call.frames == 1024);
        flushed.insert(flushed.end(), frames.begin(), frames.end());
    }
    // The first run had units 0 to 9 nowhere before it either, so a flush that forgot nothing would differ.
    CHECK(flushed == fresh);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
}

TEST(a_unit_that_gives_nothing_leaves_the_codec_working) {
    const Stream stream = tone();
    apkrt_audio_codec made{};
    CHECK(create(stream.config, 2, made) == 0);
    std::vector<std::int16_t> frames(2048);
    std::vector<unsigned char> broken = stream.units[4];
    for (std::size_t i = 0; i < broken.size(); ++i) broken[i] = static_cast<unsigned char>(i * 37);
    apkrt_audio_codec call{};
    CHECK(decode(made.id, stream.units[3], frames, call) == 0 && call.frames == 1024);
    CHECK(decode(made.id, broken, frames, call) == 0 && call.frames == 0);
    CHECK(decode(made.id, stream.units[5], frames, call) == 0 && call.frames == 1024);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
}

TEST(creates_are_checked) {
    const Stream stream = tone();
    apkrt_audio_codec call{};
    CHECK(codec_call(APKRT_AUDIO_CODEC_CREATE, 0) == -efault);
    CHECK(codec_call(9, address(&call)) == -einval);
    CHECK(create(stream.config, 0, call) == -einval);
    const std::vector<unsigned char> long_config(APKRT_AUDIO_CODEC_MAX_CONFIG + 1, 0x12);
    CHECK(create(long_config.data(), long_config.size(), call) == -einval);
    const unsigned char garbage[2] = {0xff, 0xff};
    CHECK(create(garbage, 2, call) == -einval);
    const unsigned char no_channels[2] = {0x12, 0x00};  // AAC LC at 44100 Hz, channel configuration 0
    CHECK(create(no_channels, 2, call) == -einval);
    CHECK(create(nullptr, 2, call) == -efault);
    call = {};
    call.data = address(stream.config);
    call.size = 2;
    CHECK(codec_call(APKRT_AUDIO_CODEC_CREATE, address(&call)) == -einval);  // type 0
    call.type = 2;
    CHECK(codec_call(APKRT_AUDIO_CODEC_CREATE, address(&call)) == -einval);
    call.type = APKRT_AUDIO_CODEC_AAC;
    call.reserved = 1;
    CHECK(codec_call(APKRT_AUDIO_CODEC_CREATE, address(&call)) == -einval);
}

TEST(decodes_and_ids_are_checked) {
    const Stream stream = tone();
    apkrt_audio_codec made{};
    CHECK(create(stream.config, 2, made) == 0);
    std::vector<std::int16_t> frames(2048);
    apkrt_audio_codec call{};
    CHECK(codec_call(APKRT_AUDIO_CODEC_DECODE, 0) == -efault);
    CHECK(decode(made.id + 1000, stream.units[0], frames, call) == -ebadf);
    CHECK(decode(0, stream.units[0], frames, call) == -ebadf);
    const std::vector<unsigned char> empty;
    CHECK(decode(made.id, empty, frames, call) == -einval);
    const std::vector<unsigned char> large(APKRT_AUDIO_CODEC_MAX_UNIT + 1ull);
    CHECK(decode(made.id, large, frames, call) == -einval);
    std::vector<std::int16_t> small(2047);  // half a frame short of a unit's
    CHECK(decode(made.id, stream.units[0], small, call) == -enospc && call.frames == 1024);
    decode(made.id, stream.units[0], frames, call);
    call.data = 0;
    CHECK(codec_call(APKRT_AUDIO_CODEC_DECODE, address(&call)) == -efault);
    decode(made.id, stream.units[0], frames, call);
    call.out = 0;
    CHECK(codec_call(APKRT_AUDIO_CODEC_DECODE, address(&call)) == -efault);
    decode(made.id, stream.units[0], frames, call);
    call.reserved = 1;
    CHECK(codec_call(APKRT_AUDIO_CODEC_DECODE, address(&call)) == -einval);
    CHECK(codec_call(APKRT_AUDIO_CODEC_FLUSH, made.id + 1000) == -ebadf);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == -ebadf);
    CHECK(codec_call(APKRT_AUDIO_CODEC_FLUSH, made.id) == -ebadf);
    CHECK(decode(made.id, stream.units[0], frames, call) == -ebadf);
    // A new codec never gets an old ID.
    apkrt_audio_codec again{};
    CHECK(create(stream.config, 2, again) == 0 && again.id != made.id);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, again.id) == 0);
}

TEST(a_long_config_is_read_whole) {
    // An AudioSpecificConfig of 200 bytes, tone.aac's and zeros after it, as a long program config element's: the
    // descriptors around it have sizes past 127, which take more than one byte's seven bits.
    const Stream stream = tone();
    std::vector<unsigned char> config(200, 0);
    config[0] = stream.config[0];
    config[1] = stream.config[1];
    apkrt_audio_codec made{};
    CHECK(create(config.data(), config.size(), made) == 0);
    CHECK(made.rate == 44100 && made.channels == 2 && made.frames == 1024);
    if (made.id != 0) CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
}

TEST(an_he_aac_config_gives_the_full_rate) {
    // HE-AAC over a 22050 Hz mono core with SBR to 44100 Hz, signalled explicitly (object type 5, AAC LC under it) and
    // compatibly (AAC LC, then the SBR extension after the sync word 0x2b7, as Apple's encoder writes it). Android's
    // decoder gives 2048 frames a unit at 44100 Hz for both.
    const std::vector<std::vector<unsigned char>> configs = {{0x2b, 0x8a, 0x08, 0x00}, {0x13, 0x88, 0x56, 0xe5, 0xa0}};
    for (const auto &config : configs) {
        apkrt_audio_codec made{};
        CHECK(create(config.data(), config.size(), made) == 0);
        CHECK(made.rate == 44100 && made.channels == 1 && made.frames == 2048);
        if (made.id != 0) CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
    }
}

TEST(memory_the_guest_cannot_write_is_a_fault) {
    const Stream stream = tone();
    const long page = sysconf(_SC_PAGESIZE);
    void *mapped = mmap(nullptr, static_cast<std::size_t>(page), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mapped != MAP_FAILED);
    if (mapped == MAP_FAILED) return;
    auto *fixed = static_cast<apkrt_audio_codec *>(mapped);
    *fixed = {};
    fixed->type = APKRT_AUDIO_CODEC_AAC;
    fixed->data = address(stream.config);
    fixed->size = 2;
    CHECK(mprotect(mapped, static_cast<std::size_t>(page), PROT_READ) == 0);
    // A CREATE whose answer cannot be written leaves no codec behind: more than the limit of them, then one more.
    for (int n = 0; n <= APKRT_AUDIO_CODEC_MAX_CODECS; ++n) {
        CHECK(codec_call(APKRT_AUDIO_CODEC_CREATE, address(fixed)) == -efault);
    }
    apkrt_audio_codec made{};
    CHECK(create(stream.config, 2, made) == 0);
    std::vector<std::int16_t> frames(2048);
    apkrt_audio_codec call{};
    CHECK(decode(made.id, stream.units[0], frames, call) == 0);
    call.out = address(mapped);  // frames into a page the guest cannot write
    CHECK(codec_call(APKRT_AUDIO_CODEC_DECODE, address(&call)) == -efault);
    CHECK(mprotect(mapped, static_cast<std::size_t>(page), PROT_READ | PROT_WRITE) == 0);
    *fixed = {};
    fixed->id = made.id;
    fixed->data = address(stream.units[1].data());
    fixed->size = stream.units[1].size();
    fixed->out = address(frames.data());
    fixed->capacity = 2;  // too little, and the need cannot be written back
    CHECK(mprotect(mapped, static_cast<std::size_t>(page), PROT_READ) == 0);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DECODE, address(fixed)) == -efault);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
    munmap(mapped, static_cast<std::size_t>(page));
}

TEST(at_most_32_codecs_live_at_once) {
    const Stream stream = tone();
    std::vector<std::uint64_t> ids;
    apkrt_audio_codec made{};
    for (int n = 0; n < APKRT_AUDIO_CODEC_MAX_CODECS; ++n) {
        CHECK(create(stream.config, 2, made) == 0);
        ids.push_back(made.id);
    }
    CHECK(create(stream.config, 2, made) == -ebusy);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, ids.back()) == 0);
    ids.pop_back();
    CHECK(create(stream.config, 2, made) == 0);
    ids.push_back(made.id);
    for (const std::uint64_t id : ids) CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, id) == 0);
}

}  // namespace

int main() { return apk::testing::run_all(); }
