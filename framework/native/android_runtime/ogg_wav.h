// Ours (ADR-0060 e2): an Ogg file's audio, Vorbis or Opus, as a WAV file of 16-bit PCM for MediaPlayer, whose host
// player (AVFoundation) reads no Ogg. The WAV's bytes are decoded when they are read, from the file mapped in memory
// (a descriptor's) or held there (a MediaDataSource's), so a long piece of music costs its decoder's memory and not
// its samples'. Its data has the stream's exact length, as Android's OggExtractor takes it from the granule position
// of the file's last page; a read at any offset seeks to its sample. The decoders are vorbis_wav.h's (stb_vorbis in
// this library) and opus_wav.h's (the host's, through the runtime's audio codec call).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <memory>
#include <vector>

namespace android {

class OggWav {
public:
    /// The Vorbis or Opus file of `length` bytes at `offset` of `fd` (the descriptor stays the caller's), or none for
    /// one whose first page holds neither identification header or whose decoder does not open it.
    static std::unique_ptr<OggWav> open(int fd, int64_t offset, int64_t length);
    /// The Vorbis or Opus file in `bytes`, which it keeps (an app's MediaDataSource read whole), or none as for a
    /// descriptor.
    static std::unique_ptr<OggWav> open(std::vector<uint8_t> bytes);
    /// Whether the `length` bytes begin with an Ogg page whose first packet is Vorbis' or Opus' identification header;
    /// the first kSniff bytes of a file tell.
    static bool holdsOgg(const uint8_t *bytes, int64_t length);

    virtual ~OggWav();
    OggWav(const OggWav &) = delete;
    OggWav &operator=(const OggWav &) = delete;

    /// The WAV file's size in bytes: its 44-byte header and its samples.
    int64_t size() const { return kHeader + static_cast<int64_t>(mFrames) * mChannels * 2; }
    /// OggExtractor's duration: the samples at the stream's rate, in microseconds.
    int64_t durationUs() const { return static_cast<int64_t>(mFrames) * 1000000 / mRate; }
    /// The WAV's bytes at `offset`, at most `size`: how many (0 at the end), or -1 for a decoder's failure.
    ssize_t read(int64_t offset, uint8_t *buffer, size_t size);
    /// The track's MIME type, as OggExtractor names it.
    virtual const char *mime() const = 0;

    static constexpr int64_t kHeader = 44;
    static constexpr size_t kSniff = 27 + 255 + 8;  // a page's header, its longest segment table, "OpusHead"

protected:
    OggWav() = default;
    /// The decoder of the file's `length` bytes at `bytes`, which stay this object's: whether it opened, with
    /// mFrames, mRate and mChannels set.
    virtual bool start(const uint8_t *bytes, int64_t length) = 0;
    /// Up to `count` frames from `frame` into `pcm`, interleaved: how many; 0 where the stream gives none (silence
    /// up to its last page's granule position), -1 for a failure.
    virtual int decode(uint32_t frame, int16_t *pcm, int count) = 0;

    uint32_t mFrames = 0, mRate = 0;
    int mChannels = 0;  // 1 or 2

private:
    /// The kind of file the first packet names, not yet started; none for another.
    static std::unique_ptr<OggWav> make(const uint8_t *bytes, int64_t length);
    bool opened(const uint8_t *bytes, int64_t length);

    void *mMap = nullptr;  // a descriptor's file, mapped
    size_t mMapSize = 0;
    std::vector<uint8_t> mBytes;  // or a MediaDataSource's
};

}  // namespace android
