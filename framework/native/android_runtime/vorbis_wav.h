// Ours (ADR-0060 e2): an Ogg Vorbis file as a WAV file of 16-bit PCM, for MediaPlayer, whose host player
// (AVFoundation) reads no Ogg. The WAV's bytes are decoded when they are read, by stb_vorbis (the one SoundPool
// decodes with), from the file mapped in memory, so a long piece of music costs its decoder's memory and not its
// samples'. Its data names the stream's exact length in samples, the granule position of the file's last page, as
// Android's OggExtractor takes it for the duration; a read at any offset seeks to its sample. More than two channels
// are mixed down to two (stb_vorbis' mix), as a phone's speakers play them.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <memory>
#include <vector>

struct stb_vorbis;  // stb_vorbis.c's decoder

namespace android {

class VorbisWav {
public:
    /// The Vorbis file of `length` bytes at `offset` of `fd` (the descriptor stays the caller's), or none for one
    /// whose first page holds no Vorbis identification header or that stb_vorbis does not open.
    static std::unique_ptr<VorbisWav> open(int fd, int64_t offset, int64_t length);
    /// The Vorbis file in `bytes`, which it keeps (an app's MediaDataSource read whole), or none as for a descriptor.
    static std::unique_ptr<VorbisWav> open(std::vector<uint8_t> bytes);
    /// Whether the `length` bytes begin with an Ogg page whose first packet is a Vorbis identification header; the
    /// first kSniff bytes of a file tell.
    static bool holdsVorbis(const uint8_t *bytes, int64_t length);
    ~VorbisWav();
    VorbisWav(const VorbisWav &) = delete;
    VorbisWav &operator=(const VorbisWav &) = delete;

    /// The WAV file's size in bytes: its 44-byte header and its samples.
    int64_t size() const { return kHeader + static_cast<int64_t>(mFrames) * mChannels * 2; }
    /// OggExtractor's duration: the samples at the stream's rate, in microseconds.
    int64_t durationUs() const { return static_cast<int64_t>(mFrames) * 1000000 / mRate; }
    /// The WAV's bytes at `offset`, at most `size`: how many (0 at the end), or -1 for a decoder's failure.
    ssize_t read(int64_t offset, uint8_t *buffer, size_t size);

    static constexpr int64_t kHeader = 44;
    static constexpr size_t kSniff = 27 + 255 + 7;  // a page's header, its longest segment table, "\x01vorbis"

private:
    VorbisWav() = default;
    /// The decoder of the file's `length` bytes at `bytes`, which stay this object's: whether it opened.
    bool decode(const uint8_t *bytes, int64_t length);

    void *mMap = nullptr;  // a descriptor's file, mapped
    std::vector<uint8_t> mBytes;  // or a MediaDataSource's
    size_t mMapSize = 0;
    ::stb_vorbis *mDecoder = nullptr;
    uint32_t mFrames = 0, mRate = 0;
    int mChannels = 0;
    uint32_t mNextFrame = 0;  // the decoder's position
};

}  // namespace android
