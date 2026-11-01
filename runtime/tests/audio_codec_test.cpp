// ADR-0060 e2f: audio_codec (APKRT_NR_AUDIO_CODEC) through the real dispatcher, on soundpool-codec-test's tone.aac
// (tools/make-tone-samples.sh: a 440 Hz sine of half amplitude, 0.5 s, stereo at 44100 Hz, AAC-LC in ADTS). Its
// units, without their ADTS headers, and the AudioSpecificConfig the header names are what MediaCodec's decoder gets
// from an extractor: each unit gives 1024 frames, the encoder's delay included, and together the sine; a flush
// forgets the units before it; a unit that gives nothing leaves the codec working; a long configuration and an HE-AAC
// one are read whole; and the ops, types, IDs, sizes, addresses (memory the guest cannot write among them) and the
// codec limit are checked. e2f.2: tone.mp3's and tone-mono.mp3's frames give the sine without the decoder's delay of
// 529 frames, cut again after a flush; tone.flac's frames (16 bits a sample) and tone-24.flac's (24) give tone.aiff's
// samples exactly, each at the top of 32 bits; and their configurations are checked.

#include "syscall_frame.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
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

/// A CREATE of `type` (AAC where not given) with `config`; the answer, and the struct in `call`.
long create(const unsigned char *config, std::uint64_t size, apkrt_audio_codec &call,
            std::uint32_t type = APKRT_AUDIO_CODEC_AAC) {
    call = {};
    call.type = type;
    call.data = address(config);
    call.size = size;
    return codec_call(APKRT_AUDIO_CODEC_CREATE, address(&call));
}

std::vector<unsigned char> asset(const char *name) {
    std::ifstream in(std::string(TONE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// An MPEG-1 or MPEG-2 Layer III frame's size from its header; 0 for another header.
std::size_t mp3_frame(const std::vector<unsigned char> &bytes, std::size_t at) {
    if (at + 4 > bytes.size() || bytes[at] != 0xff || (bytes[at + 1] & 0xe0) != 0xe0) return 0;
    const unsigned version = bytes[at + 1] >> 3 & 3u, layer = bytes[at + 1] >> 1 & 3u;
    const unsigned bitrate = bytes[at + 2] >> 4, rate = bytes[at + 2] >> 2 & 3u, padding = bytes[at + 2] >> 1 & 1u;
    if (version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 || rate == 3) return 0;
    static const unsigned rates[] = {44100, 48000, 32000};
    static const unsigned v1[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const unsigned v2[] = {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const unsigned hz = rates[rate] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    return (version == 3 ? 144000 * v1[bitrate - 1] : 72000 * v2[bitrate - 1]) / hz + padding;
}

/// An MP3 file's frames (no ID3 tag in these), the LAME tag's Info frame among them.
std::vector<std::vector<unsigned char>> mp3_frames(const char *name) {
    const std::vector<unsigned char> bytes = asset(name);
    std::vector<std::vector<unsigned char>> frames;
    for (std::size_t at = 0, size; (size = mp3_frame(bytes, at)) != 0 && at + size <= bytes.size(); at += size) {
        frames.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                            bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
    }
    return frames;
}

/// A FLAC file's STREAMINFO and its frames, split before each sync code whose header's CRC-8 holds.
struct Flac {
    std::vector<unsigned char> info;
    std::vector<std::vector<unsigned char>> frames;
};

Flac flac(const char *name) {
    const std::vector<unsigned char> bytes = asset(name);
    Flac stream;
    stream.info.assign(bytes.begin() + 8, bytes.begin() + 8 + 34);
    std::size_t at = 4;
    for (bool last = false; !last && at + 4 <= bytes.size();) {
        last = (bytes[at] & 0x80) != 0;
        at += 4 + (std::size_t{bytes[at + 1]} << 16 | std::size_t{bytes[at + 2]} << 8 | bytes[at + 3]);
    }
    // A frame header: the sync code, its number (UTF-8 coded), the block size's and the rate's extra bytes, and the
    // CRC-8 of all before it.
    const auto header = [&bytes](std::size_t at) {
        if (at + 6 > bytes.size() || bytes[at] != 0xff || (bytes[at + 1] & 0xfe) != 0xf8) return false;
        const unsigned block = bytes[at + 2] >> 4, rate = bytes[at + 2] & 15u, lead = bytes[at + 4];
        std::size_t length = 5 + (lead < 0x80 ? 0 : lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : lead < 0xf8 ? 3 : 4);
        length += block == 6 ? 1 : block == 7 ? 2 : 0;
        length += rate == 12 ? 1 : rate == 13 || rate == 14 ? 2 : 0;
        if (at + length >= bytes.size()) return false;
        unsigned crc = 0;
        for (std::size_t i = at; i < at + length; ++i) {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) != 0 ? (crc << 1 ^ 7) & 0xff : crc << 1 & 0xff;
        }
        return crc == bytes[at + length];
    };
    while (at < bytes.size()) {
        std::size_t end = at + 1;
        while (end < bytes.size() && !header(end)) ++end;
        stream.frames.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(end));
        at = end;
    }
    return stream;
}

/// tone.aiff's samples (16-bit big endian, after its SSND chunk's offset and block size).
std::vector<std::int16_t> aiff_samples() {
    const std::vector<unsigned char> bytes = asset("tone.aiff");
    std::vector<std::int16_t> samples;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const std::size_t size = std::size_t{bytes[at + 4]} << 24 | std::size_t{bytes[at + 5]} << 16 |
                                 std::size_t{bytes[at + 6]} << 8 | bytes[at + 7];
        if (std::memcmp(&bytes[at], "SSND", 4) == 0) {
            for (std::size_t i = at + 16; i + 1 < at + 8 + size && i + 1 < bytes.size(); i += 2) {
                samples.push_back(static_cast<std::int16_t>(bytes[i] << 8 | bytes[i + 1]));
            }
            break;
        }
        at += 8 + size + (size & 1);
    }
    return samples;
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
    call.type = 4;
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

/// Decodes `units` one by one through codec `id`, each into room the size of `frames`, appending what each gives
/// (frames of `channels` samples of `sample_size` bytes) to `all`; the frames each gave, ~0 for an error.
std::vector<std::uint64_t> decode_all(std::uint64_t id, const std::vector<std::vector<unsigned char>> &units,
                                      std::vector<std::int16_t> &frames, std::vector<std::int16_t> &all,
                                      std::size_t channels, std::size_t sample_size = 2) {
    std::vector<std::uint64_t> given;
    std::vector<unsigned char> out(frames.size() * sizeof(std::int16_t));
    for (const auto &unit : units) {
        apkrt_audio_codec call{};
        call.id = id;
        call.data = address(unit.data());
        call.size = unit.size();
        call.out = address(out.data());
        call.capacity = out.size();
        given.push_back(codec_call(APKRT_AUDIO_CODEC_DECODE, address(&call)) == 0 ? call.frames : ~0ull);
        if (given.back() == ~0ull) continue;
        const std::size_t bytes = static_cast<std::size_t>(call.frames) * channels * sample_size;
        const std::size_t first = all.size();
        all.resize(first + bytes / 2);
        std::memcpy(all.data() + first, out.data(), bytes);
    }
    return given;
}

/// The first channel's RMS and rising zero crossings over `count` frames from `first` of `channels`.
std::pair<double, int> measure(const std::vector<std::int16_t> &all, std::size_t channels, std::size_t first,
                               std::size_t count) {
    double sum = 0;
    int crossings = 0;
    for (std::size_t i = first; i < first + count && (i + 1) * channels <= all.size(); ++i) {
        const double value = all[i * channels];
        sum += value * value;
        if (all[(i - 1) * channels] < 0 && all[i * channels] >= 0) ++crossings;
    }
    return {std::sqrt(sum / static_cast<double>(count)), crossings};
}

TEST(mp3_frames_give_the_sine_without_the_decoders_delay) {
    // MPEG-1 stereo at 44100 Hz, 1152 frames a frame; MPEG-2 mono at 22050 Hz, 576. The configuration is a frame's
    // header; the first frame gives 529 frames fewer, as Android's decoder cuts its own delay.
    struct Case {
        const char *name;
        std::uint32_t rate, channels, frames;
    };
    for (const Case each : {Case{"tone.mp3", 44100, 2, 1152}, Case{"tone-mono.mp3", 22050, 1, 576}}) {
        const auto units = mp3_frames(each.name);
        CHECK(units.size() > 20);
        if (units.size() <= 20) continue;
        apkrt_audio_codec made{};
        CHECK(create(units[0].data(), 4, made, APKRT_AUDIO_CODEC_MP3) == 0);
        CHECK(made.rate == each.rate && made.channels == each.channels && made.frames == each.frames);
        std::vector<std::int16_t> frames(2 * 1152), all;
        const auto given = decode_all(made.id, units, frames, all, each.channels);
        bool sizes = given[0] == each.frames - 529;
        for (std::size_t i = 1; i < given.size(); ++i) sizes = sizes && given[i] == each.frames;
        CHECK(sizes);
        // 0.2 s from 0.15 s into the output: a sine of half amplitude at 440 Hz.
        const auto [rms, crossings] = measure(all, each.channels, each.rate * 15 / 100, each.rate / 5);
        const double expected = 0.5 * 32767 / std::sqrt(2.0);
        CHECK(std::fabs(rms - expected) < 0.1 * expected && crossings >= 86 && crossings <= 90);
        // After a flush, the delay is cut again from the first frame.
        CHECK(codec_call(APKRT_AUDIO_CODEC_FLUSH, made.id) == 0);
        std::vector<std::int16_t> again;
        const auto after = decode_all(made.id, {units[5], units[6]}, frames, again, each.channels);
        CHECK(after[0] == each.frames - 529 && after[1] == each.frames);
        CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
    }
}

TEST(mp3_configurations_are_checked) {
    const auto units = mp3_frames("tone.mp3");
    apkrt_audio_codec call{};
    CHECK(create(units[0].data(), 3, call, APKRT_AUDIO_CODEC_MP3) == -einval);
    CHECK(create(units[0].data(), 5, call, APKRT_AUDIO_CODEC_MP3) == -einval);
    const unsigned char layer2[4] = {0xff, 0xfd, 0x90, 0x64};  // MPEG-1 Layer II
    CHECK(create(layer2, 4, call, APKRT_AUDIO_CODEC_MP3) == -einval);
    const unsigned char no_sync[4] = {0xff, 0x1b, 0x90, 0x64};
    CHECK(create(no_sync, 4, call, APKRT_AUDIO_CODEC_MP3) == -einval);
    const unsigned char free_rate[4] = {0xff, 0xfb, 0x00, 0x64};  // bitrate index 0, the free format
    CHECK(create(free_rate, 4, call, APKRT_AUDIO_CODEC_MP3) == -einval);
    const unsigned char no_rate[4] = {0xff, 0xfb, 0x9c, 0x64};  // sample rate index 3
    CHECK(create(no_rate, 4, call, APKRT_AUDIO_CODEC_MP3) == -einval);
    const unsigned char mpeg25[4] = {0xff, 0xe3, 0x80, 0xc4};  // MPEG-2.5 Layer III, 64 kbit/s, 11025 Hz, mono
    CHECK(create(mpeg25, 4, call, APKRT_AUDIO_CODEC_MP3) == 0);
    CHECK(call.rate == 11025 && call.channels == 1 && call.frames == 576);
    if (call.id != 0) CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, call.id) == 0);
}

TEST(flac_frames_give_the_samples_exactly) {
    const Flac stream = flac("tone.flac");
    CHECK(stream.frames.size() == 5);
    apkrt_audio_codec made{};
    CHECK(create(stream.info.data(), stream.info.size(), made, APKRT_AUDIO_CODEC_FLAC) == 0);
    CHECK(made.rate == 44100 && made.channels == 2 && made.frames == 4608);
    // 32 bits a sample: twice the 16-bit samples' room.
    std::vector<std::int16_t> frames(2 * 2 * 4608), words;
    const auto given = decode_all(made.id, stream.frames, frames, words, 2, 4);
    CHECK((given == std::vector<std::uint64_t>{4608, 4608, 4608, 4608, 3618}));
    const std::vector<std::int16_t> expected = aiff_samples();
    // Each 16-bit sample at the top of its 32 bits (little endian: the low half 0, then the sample).
    bool exact = words.size() == 2 * expected.size() && !expected.empty();
    for (std::size_t i = 0; exact && i < expected.size(); ++i) {
        exact = words[2 * i] == 0 && words[2 * i + 1] == expected[i];
    }
    CHECK(exact);
    CHECK(codec_call(APKRT_AUDIO_CODEC_FLUSH, made.id) == 0);
    std::vector<std::int16_t> again;
    CHECK(decode_all(made.id, {stream.frames[2]}, frames, again, 2, 4) == std::vector<std::uint64_t>{4608});
    CHECK(words.size() >= again.size() + 2 * 2 * 2 * 4608 &&
          std::equal(again.begin(), again.end(), words.begin() + 2 * 2 * 2 * 4608));
    std::vector<std::int16_t> small(2 * 2 * 4608 - 1);
    apkrt_audio_codec call{};
    CHECK(decode(made.id, stream.frames[1], small, call) == -enospc && call.frames == 4608);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
}

TEST(flac_of_24_bits_gives_the_samples_at_the_top) {
    // tone.aiff's 16-bit samples as 24-bit FLAC: each the 16 bits and 8 zero bits, so at the top of 32 bits exactly
    // the 16-bit sample, as FLACDecoder's shift by 8 makes it.
    const Flac stream = flac("tone-24.flac");
    CHECK(stream.frames.size() == 22);
    apkrt_audio_codec made{};
    CHECK(create(stream.info.data(), stream.info.size(), made, APKRT_AUDIO_CODEC_FLAC) == 0);
    CHECK(made.rate == 44100 && made.channels == 2 && made.frames == 1024);
    std::vector<std::int16_t> frames(2 * 2 * 1024), words;
    const auto given = decode_all(made.id, stream.frames, frames, words, 2, 4);
    const std::vector<std::int16_t> expected = aiff_samples();
    bool exact = words.size() == 2 * expected.size() && !expected.empty();
    for (std::size_t i = 0; exact && i < expected.size(); ++i) {
        exact = words[2 * i] == 0 && words[2 * i + 1] == expected[i];
    }
    CHECK(exact);
    CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, made.id) == 0);
}

TEST(flac_configurations_are_checked) {
    const Flac stream = flac("tone.flac");
    apkrt_audio_codec call{};
    CHECK(create(stream.info.data(), 33, call, APKRT_AUDIO_CODEC_FLAC) == -einval);
    CHECK(create(stream.info.data(), 35, call, APKRT_AUDIO_CODEC_FLAC) == -einval);
    std::vector<unsigned char> info = stream.info;
    info[0] = info[1] = info[2] = info[3] = 0;  // block sizes of 0
    CHECK(create(info.data(), info.size(), call, APKRT_AUDIO_CODEC_FLAC) == -einval);
    info = stream.info;
    info[10] = info[11] = 0;
    info[12] &= 0x0f;  // a sample rate of 0
    CHECK(create(info.data(), info.size(), call, APKRT_AUDIO_CODEC_FLAC) == -einval);
    CHECK(create(stream.info.data(), stream.info.size(), call, APKRT_AUDIO_CODEC_FLAC + 1) == -einval);
    info = stream.info;
    info[12] |= 1;
    info[13] = static_cast<unsigned char>((info[13] & 0x0f) | 0x30);  // 20 bits a sample, which Android refuses
    CHECK(create(info.data(), info.size(), call, APKRT_AUDIO_CODEC_FLAC) == -einval);
    info = stream.info;
    info[12] = static_cast<unsigned char>((info[12] & 0xf1) | 0x0e);  // 8 channels: taken
    CHECK(create(info.data(), info.size(), call, APKRT_AUDIO_CODEC_FLAC) == 0 && call.channels == 8);
    if (call.id != 0) CHECK(codec_call(APKRT_AUDIO_CODEC_DESTROY, call.id) == 0);
}

}  // namespace

int main() { return apk::testing::run_all(); }
