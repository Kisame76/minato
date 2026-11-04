// Ours (ADR-0060 e2): vorbis_wav.h's Ogg Vorbis file as a WAV file, decoded by stb_vorbis as it is read.
#include "vorbis_wav.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

namespace {

/// Whether the first Ogg page's first packet is a Vorbis identification header (type 1, "vorbis").
bool holdsVorbis(const uint8_t *bytes, int64_t length) {
    if (length < 27 || memcmp(bytes, "OggS", 4) != 0) return false;
    const int64_t packet = 27 + bytes[26];  // the page's header and its segment table
    return length >= packet + 7 && memcmp(bytes + packet, "\x01vorbis", 7) == 0;
}

void put16(uint8_t *at, uint32_t value) {
    at[0] = static_cast<uint8_t>(value);
    at[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t *at, uint32_t value) {
    put16(at, value);
    put16(at + 2, value >> 16);
}

}  // namespace

std::unique_ptr<VorbisWav> VorbisWav::open(int fd, int64_t offset, int64_t length) {
    if (offset < 0 || length < 27 || length > INT32_MAX) return nullptr;  // stb_vorbis takes an int's length
    const int64_t page = sysconf(_SC_PAGESIZE);
    const int64_t start = offset - offset % page;
    std::unique_ptr<VorbisWav> wav(new VorbisWav);
    wav->mMapSize = static_cast<size_t>(length + offset - start);
    void *map = mmap(nullptr, wav->mMapSize, PROT_READ, MAP_PRIVATE, fd, start);
    if (map == MAP_FAILED) return nullptr;
    wav->mMap = map;
    const uint8_t *bytes = static_cast<const uint8_t *>(map) + (offset - start);
    if (!holdsVorbis(bytes, length)) return nullptr;
    int error = 0;
    wav->mDecoder = stb_vorbis_open_memory(bytes, static_cast<int>(length), &error, nullptr);
    if (wav->mDecoder == nullptr) return nullptr;
    const stb_vorbis_info info = stb_vorbis_get_info(wav->mDecoder);
    wav->mRate = info.sample_rate;
    wav->mChannels = std::min(info.channels, 2);
    wav->mFrames = stb_vorbis_stream_length_in_samples(wav->mDecoder);
    // A RIFF size of 32 bits: at most 4 GiB of samples.
    if (wav->mRate == 0 || wav->mChannels < 1 || wav->mFrames == 0 ||
        wav->size() - 8 > static_cast<int64_t>(UINT32_MAX)) {
        return nullptr;
    }
    return wav;
}

VorbisWav::~VorbisWav() {
    if (mDecoder != nullptr) stb_vorbis_close(mDecoder);
    if (mMap != nullptr) munmap(mMap, mMapSize);
}

ssize_t VorbisWav::read(int64_t offset, uint8_t *buffer, size_t size) {
    const int64_t total = this->size();
    if (offset < 0) return -1;
    if (offset >= total) return 0;
    size = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(size), total - offset));
    size_t done = 0;
    if (offset < kHeader) {
        uint8_t header[kHeader];
        const uint32_t data = static_cast<uint32_t>(total - kHeader);
        const uint32_t frameBytes = static_cast<uint32_t>(mChannels) * 2;
        memcpy(header, "RIFF", 4);
        put32(header + 4, static_cast<uint32_t>(total - 8));
        memcpy(header + 8, "WAVEfmt ", 8);
        put32(header + 16, 16);  // the fmt chunk's size
        put16(header + 20, 1);   // PCM
        put16(header + 22, static_cast<uint32_t>(mChannels));
        put32(header + 24, mRate);
        put32(header + 28, mRate * frameBytes);
        put16(header + 32, frameBytes);
        put16(header + 34, 16);
        memcpy(header + 36, "data", 4);
        put32(header + 40, data);
        done = static_cast<size_t>(std::min<int64_t>(kHeader - offset, static_cast<int64_t>(size)));
        memcpy(buffer, header + offset, done);
        offset += static_cast<int64_t>(done);
    }
    const int64_t frameBytes = mChannels * 2;
    constexpr int kChunk = 2048;  // frames decoded at a time
    short pcm[kChunk * 2];
    while (done < size) {
        const int64_t data = offset - kHeader;
        const uint32_t frame = static_cast<uint32_t>(data / frameBytes);
        const int64_t skip = data % frameBytes;
        if (frame != mNextFrame) {
            if (!stb_vorbis_seek(mDecoder, frame)) return done > 0 ? static_cast<ssize_t>(done) : -1;
            mNextFrame = frame;
        }
        const int64_t wanted = (static_cast<int64_t>(size - done) + skip + frameBytes - 1) / frameBytes;
        const int frames = static_cast<int>(std::min<int64_t>({wanted, kChunk, mFrames - frame}));
        int got = stb_vorbis_get_samples_short_interleaved(mDecoder, mChannels, pcm, frames * mChannels);
        if (got <= 0) {
            // A stream that ends before its last page's granule position: silence up to it.
            memset(pcm, 0, static_cast<size_t>(frames * frameBytes));
            got = frames;
        }
        mNextFrame = frame + static_cast<uint32_t>(got);
        const size_t count = static_cast<size_t>(std::min<int64_t>(got * frameBytes - skip,
                                                                   static_cast<int64_t>(size - done)));
        memcpy(buffer + done, reinterpret_cast<const uint8_t *>(pcm) + skip, count);
        done += count;
        offset += static_cast<int64_t>(count);
    }
    return static_cast<ssize_t>(done);
}

}  // namespace android
