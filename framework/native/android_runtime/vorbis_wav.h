// Ours (ADR-0060 e2): ogg_wav.h's WAV of an Ogg Vorbis file, decoded by stb_vorbis (the one SoundPool decodes with)
// from the file in memory. More than two channels are mixed down to two (stb_vorbis' mix), as a phone's speakers
// play them.
#pragma once

#include "ogg_wav.h"

struct stb_vorbis;  // stb_vorbis.c's decoder

namespace android {

class VorbisWav final : public OggWav {
public:
    ~VorbisWav() override;
    const char *mime() const override { return "audio/vorbis"; }

private:
    friend class OggWav;
    VorbisWav() = default;
    bool start(const uint8_t *bytes, int64_t length) override;
    int decode(uint32_t frame, int16_t *pcm, int count) override;

    ::stb_vorbis *mDecoder = nullptr;
    uint32_t mNextFrame = 0;  // the decoder's position
};

}  // namespace android
