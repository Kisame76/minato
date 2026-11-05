// Ours (ADR-0060 e2): ogg_wav.h's WAV of an Ogg Opus file (RFC 7845), at 48000 Hz as Opus always decodes, with
// OpusHead's pre-skip cut from its start. Its own reader splits the file's pages into packets, each with its place in
// the stream by the packets' tables of contents and the pages' granule positions; the host decodes them (the
// runtime's audio codec call, APKRT_AUDIO_CODEC_OPUS). A seek decodes from 80 ms before its sample, RFC 7845's
// pre-roll, as OggExtractor seeks. Channel mapping family 0 only (one or two channels): the host takes no other.
#pragma once

#include <vector>

#include "ogg_wav.h"

namespace android {

class OpusWav final : public OggWav {
public:
    ~OpusWav() override;
    const char *mime() const override { return "audio/opus"; }

private:
    friend class OggWav;
    OpusWav() = default;
    bool start(const uint8_t *bytes, int64_t length) override;
    int decode(uint32_t frame, int16_t *pcm, int count) override;
    /// The next packet into mDecoded: false past the last.
    bool decodeNext();

    struct Packet {
        size_t at, size;  // in mPackets
        uint64_t start;   // the stream's position of its first frame (granule positions count from 0 too)
        uint32_t frames;
    };
    std::vector<uint8_t> mPackets;  // the audio packets' bytes, one after another
    std::vector<Packet> mIndex;
    uint64_t mId = 0;  // the host's codec
    uint32_t mPreSkip = 0;
    std::vector<int16_t> mDecoded;  // the frames of the packet decoded last
    uint64_t mDecodedStart = 0;     // its position
    uint32_t mDecodedFrames = 0;
    size_t mNext = 0;               // the packet to decode next
    uint32_t mNextFrame = 0;        // the output's position after the last decode
};

}  // namespace android
