// MediaRecorder's audio: audio_encode (APKRT_NR_AUDIO_ENCODE) streaming through the real dispatcher. A session
// (CREATE, WRITE a PCM chunk at a time, FINISH) encodes a 440 Hz sine to AAC-LC in an MPEG-4 file and hands the bytes
// back as records; the test writes each record at its offset into a buffer, as the guest would to its output file,
// then the decoder host (APKRT_NR_AUDIO_DECODE) reads the buffer back as one track of about the input's length with
// the sine's peak. A long recording's per-WRITE output stays small (the host does not keep the whole file); ABORT
// drops a session; and the ops, ids and sizes are checked.

#include "syscall_frame.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "APKRuntime/guest_interface.h"
#include "check.h"

namespace {

constexpr long efault = 14, ebadf = 9, einval = 22, enospc = 28, eagain = 11;
constexpr double pi = 3.14159265358979323846;

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

long encode(std::uint64_t op, std::uint64_t argument) {
    apkrt_syscall_frame frame{};
    frame.x[0] = op;
    frame.x[1] = argument;
    frame.x[8] = APKRT_NR_AUDIO_ENCODE;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

long decode(std::uint64_t argument) {
    apkrt_syscall_frame frame{};
    frame.x[0] = argument;
    frame.x[8] = APKRT_NR_AUDIO_DECODE;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

std::vector<std::int16_t> sine(std::uint32_t rate, std::uint32_t channels, double seconds = 1.0) {
    const std::size_t frames = static_cast<std::size_t>(rate * seconds);
    std::vector<std::int16_t> pcm(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const double v = 0.5 * 32767 * std::sin(2 * pi * 440.0 * static_cast<double>(i) / rate);
        const std::int16_t s = static_cast<std::int16_t>(std::lround(v));
        for (std::uint32_t c = 0; c < channels; ++c) pcm[i * channels + c] = s;
    }
    return pcm;
}

std::vector<std::int16_t> noise(std::uint32_t rate, std::uint32_t channels, double seconds = 1.0) {
    const std::size_t frames = static_cast<std::size_t>(rate * seconds);
    std::vector<std::int16_t> pcm(frames * channels);
    std::uint32_t state = 0x1234567u;
    for (auto &sample : pcm) {
        state = state * 1664525u + 1013904223u;
        sample = static_cast<std::int16_t>(state >> 16);
    }
    return pcm;
}

/// An encoder session driven as the guest drives it: it writes PCM, drains the records and lays each one at its offset
/// into `file`, exactly as the guest's pwrite would, so `file` ends up the whole MPEG-4 file.
struct Session {
    std::uint64_t id = 0;
    std::vector<unsigned char> file;
    std::uint64_t max_write_produced = 0;  // the most one WRITE handed back (the bounded-memory check)

    long create(std::uint32_t rate, std::uint32_t channels, std::uint32_t bitrate = 0) {
        apkrt_audio_encode_create c{rate, channels, bitrate, 0, 0};
        const long r = encode(APKRT_AUDIO_ENCODE_CREATE, address(&c));
        if (r == 0) id = c.id;
        return r;
    }

    void apply(const std::vector<unsigned char> &out, std::uint64_t produced) {
        std::uint64_t at = 0;
        while (at + sizeof(apkrt_audio_encode_record) <= produced) {
            apkrt_audio_encode_record header{};
            std::memcpy(&header, out.data() + at, sizeof header);
            at += sizeof header;
            if (at + header.length > produced) break;
            if (header.offset + header.length > file.size())
                file.resize(static_cast<std::size_t>(header.offset + header.length));
            std::memcpy(file.data() + header.offset, out.data() + at, static_cast<std::size_t>(header.length));
            at += header.length;
        }
    }

    // One WRITE or FINISH, then drain the rest with size-0 WRITEs; grows `out` on -ENOSPC. Returns 0 or -errno.
    long pump(const std::int16_t *pcm, std::uint64_t bytes, bool finish, bool record_write,
              std::uint64_t cap = 1u << 16) {
        std::vector<unsigned char> out(cap);
        apkrt_audio_encode_io io{};
        io.id = id;
        io.data = pcm != nullptr ? address(pcm) : 0;
        io.size = bytes;
        io.out = address(out.data());
        io.capacity = out.size();
        long r = encode(finish ? APKRT_AUDIO_ENCODE_FINISH : APKRT_AUDIO_ENCODE_WRITE, address(&io));
        // A WRITE with PCM never returns -ENOSPC (it always takes the PCM and sets pending); only a FINISH or a
        // drain can, and those carry no PCM, so growing `out` and retrying never re-sends audio.
        if (r == -enospc && (finish || bytes == 0)) {
            out.assign(static_cast<std::size_t>(io.pending), 0);
            io.out = address(out.data());
            io.capacity = out.size();
            r = encode(finish ? APKRT_AUDIO_ENCODE_FINISH : APKRT_AUDIO_ENCODE_WRITE, address(&io));
        }
        if (r != 0) return r;
        apply(out, io.produced);
        if (record_write && io.produced > max_write_produced) max_write_produced = io.produced;
        std::uint64_t pending = io.pending;
        while (pending > 0) {
            out.assign(static_cast<std::size_t>(std::max<std::uint64_t>(cap, pending)), 0);
            apkrt_audio_encode_io drain{};
            drain.id = id;
            drain.out = address(out.data());
            drain.capacity = out.size();
            r = encode(APKRT_AUDIO_ENCODE_WRITE, address(&drain));
            if (r != 0) return r;
            apply(out, drain.produced);
            pending = drain.pending;
        }
        return 0;
    }

    // Feeds `pcm` (interleaved, `channels`) in `chunk_frames`-frame WRITEs, then FINISH.
    long record(const std::vector<std::int16_t> &pcm, std::uint32_t channels, std::size_t chunk_frames) {
        const std::size_t total = pcm.size() / channels;
        for (std::size_t done = 0; done < total;) {
            const std::size_t n = std::min(chunk_frames, total - done);
            const std::uint64_t bytes = static_cast<std::uint64_t>(n) * channels * 2;
            const long r = pump(pcm.data() + done * channels, bytes, false, true);
            if (r != 0) return r;
            done += n;
        }
        return pump(nullptr, 0, true, false);
    }
};

struct Back {
    long answer = 0;
    apkrt_audio_decode call{};
    std::vector<std::int16_t> frames;
};
Back decoded(const std::vector<unsigned char> &file, std::size_t capacity_frames = 1 << 21) {
    Back b;
    b.frames.resize(capacity_frames * 2);
    b.call.data = address(file.data());
    b.call.size = file.size();
    b.call.out = address(b.frames.data());
    b.call.capacity = b.frames.size() * sizeof(std::int16_t);
    b.answer = decode(address(&b.call));
    return b;
}

void round_trips(std::uint32_t rate, std::uint32_t channels, std::size_t chunk_frames) {
    const std::vector<std::int16_t> pcm = sine(rate, channels);
    Session s;
    CHECK(s.create(rate, channels) == 0 && s.id > 0);
    CHECK(s.record(pcm, channels, chunk_frames) == 0);
    bool right = s.file.size() >= 12 && std::memcmp(s.file.data() + 4, "ftyp", 4) == 0;
    if (!right) {
        std::printf("  %u Hz %u ch: no ftyp, %zu bytes\n", rate, channels, s.file.size());
        CHECK(right);
        return;
    }
    const Back back = decoded(s.file);
    const std::uint64_t frames = back.call.frames;
    right = back.answer == 0 && back.call.rate == rate && back.call.channels == channels &&
            frames >= static_cast<std::uint64_t>(rate) * 95 / 100 &&
            frames <= static_cast<std::uint64_t>(rate) * 105 / 100;
    std::int16_t peak = 0;
    const std::size_t first = rate * 30 / 100, last = first + rate / 5;
    for (std::size_t i = first; right && i < last && i < frames; ++i) {
        const std::int16_t v = back.frames[i * channels];
        if (v > peak) peak = v;
    }
    const double expected = 0.5 * 32767;
    right = right && std::fabs(peak - expected) < 0.15 * expected;
    CHECK(right);
    if (!right)
        std::printf("  %u Hz %u ch: decode answer %ld, %u Hz %u ch, %llu frames, peak %d\n", rate, channels,
                    back.answer, back.call.rate, back.call.channels, static_cast<unsigned long long>(frames), peak);
}

TEST(a_mono_sine_streams_to_aac_in_mp4_the_decoder_reads_back) { round_trips(44100, 1, 11025); }
TEST(a_stereo_sine_streams_to_aac_in_mp4_the_decoder_reads_back) { round_trips(44100, 2, 11025); }
TEST(other_aac_rates_round_trip) {
    round_trips(48000, 2, 12000);
    round_trips(16000, 1, 4000);
}
TEST(a_tiny_chunk_size_round_trips) { round_trips(44100, 1, 128); }  // many small WRITEs, partial last chunk

TEST(a_long_recording_keeps_the_hosts_per_write_output_small) {
    // 20 s fed one second at a time: each WRITE hands back only that second's bytes, so the most any WRITE produced
    // does not grow with the recording's length (the host is not keeping the whole file).
    const std::uint32_t rate = 44100, channels = 1;
    const std::vector<std::int16_t> pcm = sine(rate, channels, 20.0);
    Session s;
    CHECK(s.create(rate, channels) == 0);
    CHECK(s.record(pcm, channels, rate) == 0);  // 1 s chunks
    CHECK(s.max_write_produced > 0 && s.max_write_produced < (256u << 10));  // under 256 KiB for a 20 s file
    const Back back = decoded(s.file);
    CHECK(back.answer == 0 && back.call.frames >= static_cast<std::uint64_t>(rate) * 20 * 95 / 100);
}

TEST(a_set_bitrate_changes_a_noisy_files_size) {
    const std::vector<std::int16_t> n = noise(44100, 1);
    Session lo, hi;
    CHECK(lo.create(44100, 1, 32000) == 0 && lo.record(n, 1, 11025) == 0);
    CHECK(hi.create(44100, 1, 256000) == 0 && hi.record(n, 1, 11025) == 0);
    const bool right = lo.file.size() < hi.file.size() && std::memcmp(lo.file.data() + 4, "ftyp", 4) == 0 &&
                       decoded(lo.file).answer == 0 && decoded(hi.file).answer == 0;
    CHECK(right);
    if (!right) std::printf("  bitrate: lo %zu B, hi %zu B\n", lo.file.size(), hi.file.size());
}

TEST(abort_drops_the_session) {
    Session s;
    CHECK(s.create(44100, 1) == 0);
    const auto pcm = sine(44100, 1, 0.5);
    CHECK(s.pump(pcm.data(), pcm.size() * 2, false, true) == 0);  // one chunk in
    CHECK(encode(APKRT_AUDIO_ENCODE_ABORT, s.id) == 0);
    // The id is gone: a WRITE or a second ABORT on it is -EBADF.
    apkrt_audio_encode_io io{};
    io.id = s.id;
    CHECK(encode(APKRT_AUDIO_ENCODE_WRITE, address(&io)) == -ebadf);
    CHECK(encode(APKRT_AUDIO_ENCODE_ABORT, s.id) == -ebadf);
}

TEST(ops_ids_and_sizes_are_checked) {
    CHECK(encode(999, 0) == -einval);  // an unknown op
    apkrt_audio_encode_create bad_create{44100, 3, 0, 0, 0};  // 3 channels
    CHECK(encode(APKRT_AUDIO_ENCODE_CREATE, address(&bad_create)) == -einval);
    bad_create = {0, 1, 0, 0, 0};  // rate 0
    CHECK(encode(APKRT_AUDIO_ENCODE_CREATE, address(&bad_create)) == -einval);
    bad_create = {44100, 1, 0, 1, 0};  // reserved != 0
    CHECK(encode(APKRT_AUDIO_ENCODE_CREATE, address(&bad_create)) == -einval);
    CHECK(encode(APKRT_AUDIO_ENCODE_CREATE, 0) == -efault);

    apkrt_audio_encode_io io{};
    io.id = 0x9999;  // not open
    CHECK(encode(APKRT_AUDIO_ENCODE_WRITE, address(&io)) == -ebadf);

    Session s;
    CHECK(s.create(44100, 2) == 0);  // stereo: a frame is 4 bytes
    std::vector<unsigned char> out(1u << 16);
    apkrt_audio_encode_io w{};
    w.id = s.id;
    const auto pcm = sine(44100, 2, 0.1);
    w.data = address(pcm.data());
    w.size = pcm.size() * 2 - 1;  // not a whole number of frames
    w.out = address(out.data());
    w.capacity = out.size();
    CHECK(encode(APKRT_AUDIO_ENCODE_WRITE, address(&w)) == -einval);
    w.size = static_cast<std::uint64_t>(APKRT_AUDIO_ENCODE_MAX_CHUNK) + 4;  // past the chunk limit
    CHECK(encode(APKRT_AUDIO_ENCODE_WRITE, address(&w)) == -einval);
    CHECK(encode(APKRT_AUDIO_ENCODE_ABORT, s.id) == 0);
}

TEST(a_tiny_out_buffer_is_refused_then_the_record_fits) {
    // FINISH's moov record is larger than a 16-byte buffer: -ENOSPC names its size, and a buffer that big takes it.
    Session s;
    CHECK(s.create(44100, 1) == 0);
    const auto pcm = sine(44100, 1, 0.5);
    CHECK(s.pump(pcm.data(), pcm.size() * 2, false, true) == 0);
    std::vector<unsigned char> out(16);
    apkrt_audio_encode_io io{};
    io.id = s.id;
    io.out = address(out.data());
    io.capacity = out.size();
    const long r = encode(APKRT_AUDIO_ENCODE_FINISH, address(&io));
    CHECK(r == -enospc && io.pending > 16 && io.produced == 0);
    out.assign(static_cast<std::size_t>(io.pending), 0);  // a buffer the next record fits
    io.out = address(out.data());
    io.capacity = out.size();
    CHECK(encode(APKRT_AUDIO_ENCODE_FINISH, address(&io)) == 0 && io.produced > 0);
    // drain any remainder so the session ends cleanly
    while (io.pending > 0) {
        apkrt_audio_encode_io d{};
        d.id = s.id;
        out.assign(static_cast<std::size_t>(io.pending), 0);
        d.out = address(out.data());
        d.capacity = out.size();
        CHECK(encode(APKRT_AUDIO_ENCODE_WRITE, address(&d)) == 0);
        io.pending = d.pending;
    }
    CHECK(encode(APKRT_AUDIO_ENCODE_ABORT, s.id) == -ebadf);  // finished and drained: the id is gone
}

TEST(a_small_out_on_a_write_takes_the_pcm_once_no_doubling) {
    // The old bug: a WRITE that returned -ENOSPC was retried with the same PCM, doubling the audio. Now a WRITE always
    // takes the PCM (returns 0) and the guest drains separately, so a 16-byte `out` cannot double anything.
    const std::uint32_t rate = 44100, channels = 1;
    const std::vector<std::int16_t> pcm = sine(rate, channels, 0.5);
    Session plain;
    CHECK(plain.create(rate, channels) == 0 && plain.record(pcm, channels, rate / 4) == 0);
    const std::uint64_t plain_frames = decoded(plain.file).call.frames;
    CHECK(plain_frames > 0);
    Session small;
    CHECK(small.create(rate, channels) == 0);
    const std::size_t total = pcm.size();  // mono
    for (std::size_t done = 0; done < total;) {
        const std::size_t n = std::min<std::size_t>(rate / 4, total - done);
        CHECK(small.pump(pcm.data() + done, static_cast<std::uint64_t>(n) * 2, false, false, 16) == 0);  // out = 16 B
        done += n;
    }
    CHECK(small.pump(nullptr, 0, true, false, 16) == 0);
    const std::uint64_t small_frames = decoded(small.file).call.frames;
    const bool right = small_frames >= plain_frames * 90 / 100 && small_frames <= plain_frames * 110 / 100;
    CHECK(right);
    if (!right)
        std::printf("  plain %llu vs tiny-out %llu frames\n", static_cast<unsigned long long>(plain_frames),
                    static_cast<unsigned long long>(small_frames));
}

TEST(writes_without_draining_hit_the_cap_and_recover) {
    // A guest that never drains cannot grow the host without bound: once the undrained records pass the cap, a WRITE
    // with PCM is refused with -EAGAIN and takes nothing; after draining, writing works again.
    Session s;
    CHECK(s.create(44100, 1) == 0);
    // Max-size chunks, so each WRITE adds a big run of records and the 4 MiB cap is passed in a few of them.
    std::vector<std::int16_t> chunk(APKRT_AUDIO_ENCODE_MAX_CHUNK / sizeof(std::int16_t));
    std::uint32_t state = 0x2b9f1u;
    for (auto &x : chunk) {
        state = state * 1664525u + 1013904223u;
        x = static_cast<std::int16_t>(state >> 16);
    }
    std::vector<unsigned char> tiny(16);  // nothing drains to this, so `pending` climbs
    long r = 0;
    int writes = 0;
    for (; writes < 20; ++writes) {
        apkrt_audio_encode_io io{};
        io.id = s.id;
        io.data = address(chunk.data());
        io.size = chunk.size() * sizeof(std::int16_t);
        io.out = address(tiny.data());
        io.capacity = tiny.size();
        r = encode(APKRT_AUDIO_ENCODE_WRITE, address(&io));
        if (r == -eagain) break;
        CHECK(r == 0 && io.produced == 0);  // took the PCM, drained nothing to the 16-byte out
    }
    CHECK(r == -eagain && writes > 0 && writes < 20);  // the cap stopped it, and well before the bound
    std::uint64_t pending = 1;
    while (pending > 0) {  // drain it all
        std::vector<unsigned char> out(16u << 20);
        apkrt_audio_encode_io d{};
        d.id = s.id;
        d.out = address(out.data());
        d.capacity = out.size();
        CHECK(encode(APKRT_AUDIO_ENCODE_WRITE, address(&d)) == 0);
        pending = d.pending;
    }
    const std::vector<std::int16_t> small = sine(44100, 1, 0.05);
    CHECK(s.pump(small.data(), small.size() * sizeof(std::int16_t), false, false) == 0);  // writes again
    CHECK(encode(APKRT_AUDIO_ENCODE_ABORT, s.id) == 0);
}

TEST(a_write_with_pcm_after_finish_is_refused) {
    Session s;
    CHECK(s.create(44100, 1) == 0);
    const std::vector<std::int16_t> pcm = sine(44100, 1, 0.3);
    CHECK(s.pump(pcm.data(), pcm.size() * sizeof(std::int16_t), false, false) == 0);
    // FINISH into a 16-byte buffer: -ENOSPC, but the track is finalized and the encoder disposed.
    std::vector<unsigned char> tiny(16);
    apkrt_audio_encode_io f{};
    f.id = s.id;
    f.out = address(tiny.data());
    f.capacity = tiny.size();
    CHECK(encode(APKRT_AUDIO_ENCODE_FINISH, address(&f)) == -enospc);
    // A WRITE with PCM now must be refused, not reach AudioToolbox with a disposed encoder.
    std::vector<unsigned char> out(1u << 16);
    apkrt_audio_encode_io w{};
    w.id = s.id;
    w.data = address(pcm.data());
    w.size = pcm.size() * sizeof(std::int16_t);
    w.out = address(out.data());
    w.capacity = out.size();
    CHECK(encode(APKRT_AUDIO_ENCODE_WRITE, address(&w)) == -einval);
    CHECK(encode(APKRT_AUDIO_ENCODE_ABORT, s.id) == 0);
}

}  // namespace

int main() { return apk::testing::run_all(); }
