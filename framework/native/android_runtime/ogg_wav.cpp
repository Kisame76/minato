// Ours (ADR-0060 e2): ogg_wav.h's Ogg file as a WAV file, decoded as it is read.
#include "ogg_wav.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>

#include "opus_wav.h"
#include "vorbis_wav.h"

namespace android {

namespace {

/// The first Ogg page's first packet, at least `size` bytes of it there: where it starts, or none.
const uint8_t *firstPacket(const uint8_t *bytes, int64_t length, int64_t size) {
    if (length < 27 || memcmp(bytes, "OggS", 4) != 0) return nullptr;
    const int64_t packet = 27 + bytes[26];  // the page's header and its segment table
    return length >= packet + size ? bytes + packet : nullptr;
}

bool holdsVorbis(const uint8_t *bytes, int64_t length) {
    const uint8_t *packet = firstPacket(bytes, length, 7);
    return packet != nullptr && memcmp(packet, "\x01vorbis", 7) == 0;
}

bool holdsOpus(const uint8_t *bytes, int64_t length) {
    const uint8_t *packet = firstPacket(bytes, length, 8);
    return packet != nullptr && memcmp(packet, "OpusHead", 8) == 0;
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

bool OggWav::holdsOgg(const uint8_t *bytes, int64_t length) {
    return holdsVorbis(bytes, length) || holdsOpus(bytes, length);
}

std::unique_ptr<OggWav> OggWav::make(const uint8_t *bytes, int64_t length) {
    if (holdsVorbis(bytes, length)) return std::unique_ptr<OggWav>(new VorbisWav);
    if (holdsOpus(bytes, length)) return std::unique_ptr<OggWav>(new OpusWav);
    return nullptr;
}

bool OggWav::opened(const uint8_t *bytes, int64_t length) {
    // A RIFF size of 32 bits: at most 4 GiB of samples.
    return start(bytes, length) && mRate != 0 && mChannels >= 1 && mChannels <= 2 && mFrames != 0 &&
           size() - 8 <= static_cast<int64_t>(UINT32_MAX);
}

std::unique_ptr<OggWav> OggWav::open(int fd, int64_t offset, int64_t length) {
    if (offset < 0 || length < 27 || length > INT32_MAX) return nullptr;  // stb_vorbis takes an int's length
    const int64_t page = sysconf(_SC_PAGESIZE);
    const int64_t start = offset - offset % page;
    const size_t mapSize = static_cast<size_t>(length + offset - start);
    void *map = mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, start);
    if (map == MAP_FAILED) return nullptr;
    const uint8_t *bytes = static_cast<const uint8_t *>(map) + (offset - start);
    std::unique_ptr<OggWav> wav = make(bytes, length);
    if (wav == nullptr) {
        munmap(map, mapSize);
        return nullptr;
    }
    wav->mMap = map;
    wav->mMapSize = mapSize;
    return wav->opened(bytes, length) ? std::move(wav) : nullptr;
}

std::unique_ptr<OggWav> OggWav::open(std::vector<uint8_t> bytes) {
    if (bytes.size() < 27 || bytes.size() > INT32_MAX) return nullptr;
    const int64_t length = static_cast<int64_t>(bytes.size());
    std::unique_ptr<OggWav> wav = make(bytes.data(), length);
    if (wav == nullptr) return nullptr;
    wav->mBytes = std::move(bytes);
    return wav->opened(wav->mBytes.data(), length) ? std::move(wav) : nullptr;
}

OggWav::~OggWav() {
    if (mMap != nullptr) munmap(mMap, mMapSize);
}

ssize_t OggWav::read(int64_t offset, uint8_t *buffer, size_t size) {
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
    int16_t pcm[kChunk * 2];
    while (done < size) {
        const int64_t data = offset - kHeader;
        const uint32_t frame = static_cast<uint32_t>(data / frameBytes);
        const int64_t skip = data % frameBytes;
        const int64_t wanted = (static_cast<int64_t>(size - done) + skip + frameBytes - 1) / frameBytes;
        const int frames = static_cast<int>(std::min<int64_t>({wanted, kChunk, mFrames - frame}));
        int got = decode(frame, pcm, frames);
        if (got < 0) return done > 0 ? static_cast<ssize_t>(done) : -1;
        if (got == 0) {
            // A stream that ends before its last page's granule position: silence up to it.
            memset(pcm, 0, static_cast<size_t>(frames * frameBytes));
            got = frames;
        }
        const size_t count = static_cast<size_t>(std::min<int64_t>(got * frameBytes - skip,
                                                                   static_cast<int64_t>(size - done)));
        memcpy(buffer + done, reinterpret_cast<const uint8_t *>(pcm) + skip, count);
        done += count;
        offset += static_cast<int64_t>(count);
    }
    return static_cast<ssize_t>(done);
}

}  // namespace android
