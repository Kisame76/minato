// Ours (ADR-0060 e2): vorbis_wav.h's Ogg Vorbis decoder, stb_vorbis' pull API.
#include "vorbis_wav.h"

#include <algorithm>

// stb_vorbis.c's pull API (third_party/sources.json), built into this library for SoundPool.
extern "C" {
struct stb_vorbis;
struct stb_vorbis_alloc;
struct stb_vorbis_info {
    unsigned int sample_rate;
    int channels;
    unsigned int setup_memory_required;
    unsigned int setup_temp_memory_required;
    unsigned int temp_memory_required;
    int max_frame_size;
};
stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len, int *error, const stb_vorbis_alloc *alloc);
stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis *f);
int stb_vorbis_seek(stb_vorbis *f, unsigned int sample_number);
int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
void stb_vorbis_close(stb_vorbis *f);
}

namespace android {

bool VorbisWav::start(const uint8_t *bytes, int64_t length) {
    int error = 0;
    mDecoder = stb_vorbis_open_memory(bytes, static_cast<int>(length), &error, nullptr);
    if (mDecoder == nullptr) return false;
    const stb_vorbis_info info = stb_vorbis_get_info(mDecoder);
    mRate = info.sample_rate;
    mChannels = std::min(info.channels, 2);
    mFrames = stb_vorbis_stream_length_in_samples(mDecoder);
    return true;
}

VorbisWav::~VorbisWav() {
    if (mDecoder != nullptr) stb_vorbis_close(mDecoder);
}

int VorbisWav::decode(uint32_t frame, int16_t *pcm, int count) {
    if (frame != mNextFrame) {
        if (!stb_vorbis_seek(mDecoder, frame)) return -1;
        mNextFrame = frame;
    }
    const int got = stb_vorbis_get_samples_short_interleaved(mDecoder, mChannels, pcm, count * mChannels);
    // None: the silence OggWav gives in their place moves the position on as well.
    mNextFrame = frame + static_cast<uint32_t>(got > 0 ? got : count);
    return std::max(got, 0);
}

}  // namespace android
