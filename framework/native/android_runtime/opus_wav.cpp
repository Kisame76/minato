// Ours (ADR-0060 e2): opus_wav.h's Ogg Opus reader and its host decoder.
#include "opus_wav.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include <APKRuntime/guest_interface.h>

namespace android {

namespace {

constexpr uint32_t kPreRoll = 3840;  // 80 ms at 48000 Hz (RFC 7845 section 4.6)

long codecCall(uint64_t op, uintptr_t argument) {
    long result;
    do {
        result = syscall(APKRT_NR_AUDIO_CODEC, op, argument);
    } while (result == -1 && errno == EINTR);
    return result == -1 ? -errno : result;
}

/// An Opus packet's frames at 48000 Hz by its table of contents (RFC 6716 section 3.1); 0 for one that is not one.
uint32_t opusFrames(const uint8_t *packet, size_t size) {
    if (size == 0) return 0;
    static constexpr uint32_t kLengths[32] = {480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920,
                                              2880, 480, 960, 480,  960, 120, 240, 480, 960, 120, 240,
                                              480, 960, 120, 240, 480, 960, 120, 240, 480, 960};
    const uint32_t length = kLengths[packet[0] >> 3], code = packet[0] & 3u;
    uint32_t count = code == 0 ? 1 : 2;
    if (code == 3) {
        if (size < 2) return 0;
        count = packet[1] & 0x3fu;
    }
    return count == 0 || length * count > 5760 ? 0 : length * count;
}

uint32_t le32(const uint8_t *at) {
    return static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 | static_cast<uint32_t>(at[2]) << 16 |
           static_cast<uint32_t>(at[3]) << 24;
}

uint64_t le64(const uint8_t *at) { return le32(at) | static_cast<uint64_t>(le32(at + 4)) << 32; }

}  // namespace

bool OpusWav::start(const uint8_t *bytes, int64_t length) {
    // The pages of the first page's stream (RFC 3533): another stream's pages are passed over, a page that does not
    // hold together ends the file.
    std::vector<uint8_t> packet, head;
    uint32_t serial = 0;
    size_t packets = 0;  // so far, the two headers among them
    uint64_t firstGranule = 0, lastGranule = 0;
    uint64_t firstFrames = 0;   // of the audio packets completed on the first audio page
    bool firstPage = true, audioSeen = false, granuleSeen = false;
    for (int64_t at = 0; at + 27 <= length;) {
        const uint8_t *page = bytes + at;
        if (memcmp(page, "OggS", 4) != 0 || page[4] != 0) break;
        const int64_t segments = page[26];
        if (at + 27 + segments > length) break;
        int64_t body = 0;
        for (int64_t i = 0; i < segments; ++i) body += page[27 + i];
        if (at + 27 + segments + body > length) break;
        if (firstPage) serial = le32(page + 14);
        firstPage = false;
        if (le32(page + 14) != serial) {
            at += 27 + segments + body;
            continue;
        }
        const uint64_t granule = le64(page + 6);
        const uint8_t *data = page + 27 + segments;
        size_t completed = 0;
        uint64_t completedFrames = 0;
        for (int64_t i = 0; i < segments; ++i) {
            const uint8_t lacing = page[27 + i];
            packet.insert(packet.end(), data, data + lacing);
            data += lacing;
            if (lacing == 255) continue;
            if (packets == 0) {
                head = packet;
            } else if (packets >= 2) {
                const uint32_t frames = opusFrames(packet.data(), packet.size());
                if (frames != 0) {
                    mIndex.push_back({mPackets.size(), packet.size(), 0, frames});
                    mPackets.insert(mPackets.end(), packet.begin(), packet.end());
                    ++completed;
                    completedFrames += frames;
                }
            }
            ++packets;
            packet.clear();
        }
        if (granule != UINT64_MAX && packets > 2) {  // a page on which an audio packet ends
            if (!audioSeen && completed > 0) {
                audioSeen = true;
                firstGranule = granule;
                firstFrames = completedFrames;
            }
            lastGranule = granule;
            granuleSeen = true;
        }
        at += 27 + segments + body;
    }
    // OpusHead (RFC 7845 section 5.1): version 0.x, the channels, the pre-skip; mapping family 0 only.
    if (head.size() < 19 || memcmp(head.data(), "OpusHead", 8) != 0 || (head[8] & 0xf0) != 0 || head[18] != 0 ||
        head[9] < 1 || head[9] > 2 || mIndex.empty() || !granuleSeen) {
        return false;
    }
    mPreSkip = static_cast<uint32_t>(head[10] | head[11] << 8);
    // The first packet's position: the first audio page's granule position less its packets' frames, from 0.
    uint64_t position = firstGranule > firstFrames ? firstGranule - firstFrames : 0;
    for (Packet &each : mIndex) {
        each.start = position;
        position += each.frames;
    }
    if (lastGranule <= mPreSkip || lastGranule - mPreSkip > UINT32_MAX) return false;
    apkrt_audio_codec call{};
    call.type = APKRT_AUDIO_CODEC_OPUS;
    call.data = reinterpret_cast<uintptr_t>(head.data());
    call.size = std::min<size_t>(head.size(), APKRT_AUDIO_CODEC_MAX_CONFIG);
    if (codecCall(APKRT_AUDIO_CODEC_CREATE, reinterpret_cast<uintptr_t>(&call)) != 0) return false;
    mId = call.id;
    mRate = 48000;
    mChannels = head[9];
    mFrames = static_cast<uint32_t>(lastGranule - mPreSkip);
    mDecoded.resize(static_cast<size_t>(call.frames) * call.channels);
    return call.channels == static_cast<uint32_t>(mChannels);
}

OpusWav::~OpusWav() {
    if (mId != 0) codecCall(APKRT_AUDIO_CODEC_DESTROY, mId);
}

bool OpusWav::decodeNext() {
    if (mNext >= mIndex.size()) return false;
    const Packet &packet = mIndex[mNext++];
    apkrt_audio_codec call{};
    call.id = mId;
    call.data = reinterpret_cast<uintptr_t>(mPackets.data() + packet.at);
    call.size = packet.size;
    call.out = reinterpret_cast<uintptr_t>(mDecoded.data());
    call.capacity = mDecoded.size() * sizeof(int16_t);
    mDecodedStart = packet.start;
    mDecodedFrames = packet.frames;
    // A packet the host cannot decode keeps its place as silence.
    if (codecCall(APKRT_AUDIO_CODEC_DECODE, reinterpret_cast<uintptr_t>(&call)) != 0 || call.frames != packet.frames) {
        std::fill(mDecoded.begin(), mDecoded.begin() + static_cast<ptrdiff_t>(packet.frames) * mChannels, 0);
    }
    return true;
}

int OpusWav::decode(uint32_t frame, int16_t *pcm, int count) {
    uint64_t want = static_cast<uint64_t>(frame) + mPreSkip;  // the stream's position
    if (frame != mNextFrame) {
        // From the last packet that starts at or before the pre-roll, the decoder as new.
        const uint64_t from = want > kPreRoll ? want - kPreRoll : 0;
        const auto after = std::upper_bound(mIndex.begin(), mIndex.end(), from,
                                            [](uint64_t at, const Packet &each) { return at < each.start; });
        mNext = after == mIndex.begin() ? 0 : static_cast<size_t>(after - mIndex.begin() - 1);
        mDecodedFrames = 0;
        if (codecCall(APKRT_AUDIO_CODEC_FLUSH, mId) != 0) return -1;
    }
    int done = 0;
    while (done < count) {
        if (mDecodedFrames == 0 || want >= mDecodedStart + mDecodedFrames) {
            if (!decodeNext()) break;
            continue;
        }
        const size_t channels = static_cast<size_t>(mChannels);
        int n = static_cast<int>(
            std::min<uint64_t>(static_cast<uint64_t>(count - done), mDecodedStart + mDecodedFrames - want));
        if (want < mDecodedStart) {
            // Before the first packet: silence.
            n = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(count - done), mDecodedStart - want));
            std::fill(pcm + static_cast<size_t>(done) * channels, pcm + static_cast<size_t>(done + n) * channels, 0);
        } else {
            const int16_t *from = mDecoded.data() + static_cast<size_t>(want - mDecodedStart) * channels;
            std::copy(from, from + static_cast<size_t>(n) * channels, pcm + static_cast<size_t>(done) * channels);
        }
        done += n;
        want += static_cast<uint64_t>(n);
    }
    // Past the last packet: the silence OggWav gives in its place moves the position on as well.
    mNextFrame = frame + static_cast<uint32_t>(done > 0 ? done : count);
    return done;
}

}  // namespace android
