// The SoundPool plan's step 3: audio_decode (APKRT_NR_AUDIO_DECODE) through the real dispatcher, on soundpool-codec-
// test's files (tools/make-tone-samples.sh: a 440 Hz sine of half amplitude, 0.5 s, stereo at 44100 Hz, and in
// mono at 22050 Hz). Each of the formats Android 15's SoundPool decodes gives the sine at the file's
// rate and channels, within the encoders' padding and loss; a buffer that is too small takes the whole frames that
// fit; ALAC and AC-3 in MP4 and AIFF, which Android's SoundPool does not decode, Ogg and WAV, which the guest decodes
// itself, and bytes of no format are refused; and the struct's, the bytes' and the buffer's addresses and the file's
// size are checked.

#include "syscall_frame.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "APKRuntime/guest_interface.h"
#include "check.h"

namespace {

constexpr long efault = 14, einval = 22;

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

long decode(std::uint64_t argument) {
    apkrt_syscall_frame frame{};
    frame.x[0] = argument;
    frame.x[8] = APKRT_NR_AUDIO_DECODE;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

std::vector<unsigned char> file(const std::string &name) {
    std::ifstream in(std::string(TONE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Decoded {
    long answer = 0;
    apkrt_audio_decode call{};
    std::vector<std::int16_t> frames;
};

Decoded decoded(const std::vector<unsigned char> &bytes, std::size_t capacity_frames = 1 << 20) {
    Decoded result;
    result.frames.resize(capacity_frames * 2);
    result.call.data = address(bytes.data());
    result.call.size = bytes.size();
    result.call.out = address(result.frames.data());
    result.call.capacity = result.frames.size() * sizeof(std::int16_t);
    result.answer = decode(address(&result.call));
    return result;
}

TEST(the_formats_android_decodes_give_the_sine) {
    const struct {
        const char *name;
        std::uint32_t rate, channels;
    } files[] = {{"tone.mp3", 44100, 2}, {"tone-mono.mp3", 22050, 1}, {"tone.m4a", 44100, 2},
                 {"tone.aac", 44100, 2}, {"tone.flac", 44100, 2}};
    for (const auto &expected_file : files) {
        const char *name = expected_file.name;
        const std::uint32_t rate = expected_file.rate, channels = expected_file.channels;
        const std::vector<unsigned char> bytes = file(name);
        CHECK(!bytes.empty());
        const Decoded got = decoded(bytes);
        const std::uint64_t frames = got.call.frames;
        bool right = got.answer == 0 && got.call.rate == rate && got.call.channels == channels &&
                     frames >= rate * 45 / 100 && frames <= rate * 60 / 100;
        // 0.2 s from 0.18 s on: the first channel's RMS that of a sine of half amplitude, both channels alike, and
        // 440 Hz by the rising zero crossings.
        double sum = 0, difference = 0;
        int crossings = 0;
        const std::size_t first = rate * 18 / 100, last = first + rate / 5;
        for (std::size_t i = first; right && i < last; ++i) {
            const double left = got.frames[channels * i];
            const double right_value = got.frames[channels * i + channels - 1];
            sum += left * left;
            difference += std::fabs(left - right_value);
            if (got.frames[channels * (i - 1)] < 0 && got.frames[channels * i] >= 0) ++crossings;
        }
        const double count = static_cast<double>(last - first);
        const double rms = std::sqrt(sum / count), expected = 0.5 * 32767 / std::sqrt(2.0);
        right = right && std::fabs(rms - expected) < 0.1 * expected && difference / count < 0.01 * expected &&
                crossings >= 86 && crossings <= 90;  // 0.2 s of 440 Hz: 88
        CHECK(right);
        if (!right) {
            std::printf("  %s: answer %ld, %u Hz, %u channels, %llu frames, rms %.0f, %d crossings\n", name, got.answer,
                        got.call.rate, got.call.channels, static_cast<unsigned long long>(frames), rms, crossings);
        }
    }
}

TEST(an_mp3_file_with_an_id3_tag_decodes) {
    std::vector<unsigned char> bytes = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 10, 'T', 'I', 'T', '2', 0, 0, 0, 0, 0, 0};
    const std::vector<unsigned char> mp3 = file("tone.mp3");
    bytes.insert(bytes.end(), mp3.begin(), mp3.end());
    const Decoded got = decoded(bytes);
    CHECK(got.answer == 0 && got.call.rate == 44100 && got.call.channels == 2 && got.call.frames > 20000);
}

TEST(a_small_buffer_takes_the_whole_frames_that_fit) {
    const std::vector<unsigned char> bytes = file("tone.flac");
    Decoded got = decoded(bytes, 1000);
    got.call.capacity = 1000 * 4 + 3;  // a frame and a half too few for 1001
    got.answer = decode(address(&got.call));
    CHECK(got.answer == 0 && got.call.frames == 1000 && got.call.channels == 2);
    const Decoded whole = decoded(bytes);
    bool same = whole.answer == 0;
    for (std::size_t i = 0; same && i < 2000; ++i) same = got.frames[i] == whole.frames[i];
    CHECK(same);
}

TEST(other_files_are_refused) {
    std::vector<unsigned char> noise(4096);
    for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<unsigned char>(i * 7919 >> 3);
    const std::vector<std::vector<unsigned char>> refused = {file("tone-alac.m4a"), file("tone-ac3.mp4"),
                                                             file("tone.aiff"), file("tone.ogg"), noise};
    for (const auto &bytes : refused) {
        CHECK(!bytes.empty());
        CHECK(decoded(bytes).answer == -einval);
    }
    // A WAV file: the guest's own.
    std::vector<unsigned char> wav = {'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0,
                                      0, 1, 0, 1, 0, 0x44, 0xac, 0, 0, 0x88, 0x58, 1, 0, 2, 0, 16, 0, 'd', 'a', 't',
                                      'a', 0, 0, 0, 0};
    CHECK(decoded(wav).answer == -einval);
}

TEST(sizes_and_addresses_are_checked) {
    const std::vector<unsigned char> bytes = file("tone.mp3");
    CHECK(decode(0) == -efault);
    Decoded got = decoded(bytes);
    got.call.size = 0;
    CHECK(decode(address(&got.call)) == -einval);
    got.call.size = APKRT_AUDIO_DECODE_MAX_FILE + 1ull;
    CHECK(decode(address(&got.call)) == -einval);
    got.call.size = bytes.size();
    got.call.data = 0;
    CHECK(decode(address(&got.call)) == -efault);
    got.call.data = address(bytes.data());
    got.call.out = 0;
    CHECK(decode(address(&got.call)) == -efault);
}

}  // namespace

int main() { return apk::testing::run_all(); }
