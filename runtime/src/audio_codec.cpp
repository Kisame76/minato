// The guest's audio codecs (audio_codec.h, ADR-0060 e2f). The registry maps the runtime's IDs to AudioToolbox's
// converters under one mutex, which no converter call holds; each codec has a mutex of its own, held around every
// converter call, so that they come one at a time and a DESTROY waits for the call still running on another thread.
// IDs count up from 1 and are never used again. An AAC codec's configuration, csd-0's AudioSpecificConfig, goes to
// AudioToolbox inside an MPEG-4 ES descriptor, the magic cookie of an MP4 file's esds, from which AudioToolbox names
// the format, the rate, the channels and the frames a unit gives, of its richest layer (HE-AAC's SBR at the full rate,
// as Android's decoder gives it); only the AAC family is taken. An MP3 codec's configuration is a frame's header,
// which names the format, the rate, the channels and the frames a frame gives, Layer III only; AudioToolbox's decoder
// cuts its own delay of 529 frames from the first frame, as Android's does. A FLAC codec's configuration is
// STREAMINFO, which goes to AudioToolbox inside a dfLa box, the magic cookie of a CAF or MP4 file's FLAC; its frames
// come out as 32-bit samples, the sample's bits at the top, so that the guest shifts them as Android does; a frame
// gives at most 65535 frames of 8 channels, 2 MiB, more than a unit's limit, which bounds only the input. An Opus
// codec's configuration is OpusHead (RFC 7845): channel mapping family 0 (one or two channels), or another family's
// streams and stream map (one to eight channels, as Android's decoder takes them), which go to AudioToolbox as the
// magic cookie, OpusHead in family 1, whose decoder then gives the channels in the stream map's order as libopus's
// multistream decoder does [measured: the same samples]; its units
// are packets, decoded at 48000 Hz as Opus always is; AudioToolbox's converter takes only packets of the length it
// was made for, so a packet of another length gets a new converter, and a new converter keeps back the first 120
// frames it decodes, which the codec gives as silence before them, so that each packet gives its table of contents'
// frames and the frames stay in line with the encoder's input (its pre-skip the guest's to cut). A DECODE
// hands the converter its one unit and copies the frames it gives into the guest's buffer, each size checked first: a
// unit the converter cannot decode gives no frames, and the codec goes on with the next. The guest's struct is read
// once and the unit copied into host memory before AudioToolbox sees it. AudioToolbox is opened by its path, not
// linked, as audio_decode.cpp opens it.
#include "audio_codec.h"

#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

#include <AudioToolbox/AudioConverter.h>
#include <AudioToolbox/AudioFormat.h>
#include <dlfcn.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace apkrt::audio_codec {
namespace {

namespace linux_errno = sys::linux_errno;

static_assert(sizeof(apkrt_audio_codec) == 64 && offsetof(apkrt_audio_codec, frames) == 40);
static_assert(offsetof(apkrt_audio_codec, type) == 48 && offsetof(apkrt_audio_codec, reserved) == 60);

/// AudioToolbox's functions, found once.
struct Toolbox {
    decltype(&AudioFormatGetProperty) format = nullptr;
    decltype(&AudioFormatGetPropertyInfo) format_size = nullptr;
    decltype(&AudioConverterNew) make = nullptr;
    decltype(&AudioConverterSetProperty) set = nullptr;
    decltype(&AudioConverterFillComplexBuffer) fill = nullptr;
    decltype(&AudioConverterReset) reset = nullptr;
    decltype(&AudioConverterDispose) dispose = nullptr;
    bool found() const { return format && format_size && make && set && fill && reset && dispose; }
};

const Toolbox &toolbox() {
    static Toolbox found;
    static std::once_flag once;
    std::call_once(once, [] {
        void *library = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY);
        if (library == nullptr) return;
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
        FIND(format, AudioFormatGetProperty);
        FIND(format_size, AudioFormatGetPropertyInfo);
        FIND(make, AudioConverterNew);
        FIND(set, AudioConverterSetProperty);
        FIND(fill, AudioConverterFillComplexBuffer);
        FIND(reset, AudioConverterReset);
        FIND(dispose, AudioConverterDispose);
#undef FIND
    });
    return found;
}

struct Codec {
    AudioConverterRef converter = nullptr;
    std::uint32_t channels = 0;
    std::uint32_t frames = 0;  // the most a unit gives
    std::uint32_t sample = 2;  // a sample's bytes
    std::uint32_t type = 0;
    // Opus: the formats for a new converter, the packets' length its converter takes, and whether that converter
    // has yet to give a packet's frames whole.
    AudioStreamBasicDescription source{}, pcm{};
    std::vector<unsigned char> cookie;  // Opus beyond family 0: every new converter's magic cookie
    std::uint32_t packet = 0;
    bool fresh = false;
    std::mutex mutex;          // around each converter call, and for `gone`
    bool gone = false;         // destroyed: no more calls
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Codec>> codecs;
    std::uint64_t next_id = 1;
};

Registry &registry() {
    static auto *one = new Registry;  // as long as the process
    return *one;
}

std::shared_ptr<Codec> find(std::uint64_t id) {
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    const auto found = all.codecs.find(id);
    return found == all.codecs.end() ? nullptr : found->second;
}

/// Copies `size` of the guest's bytes at `address` into `bytes`; false where the guest does not have them.
bool take(std::uint64_t address, std::uint64_t size, std::vector<unsigned char> &bytes) {
    if (address == 0) return false;
    bytes.resize(size);
    return sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(bytes.data()), address, size);
}

/// Appends an MPEG-4 descriptor's tag and its size in the four-byte form: seven bits a byte, the high bit set on all
/// but the last.
void descriptor(std::vector<unsigned char> &out, unsigned char tag, std::size_t size) {
    const auto bits = [size](unsigned shift) { return static_cast<unsigned char>(size >> shift & 0x7f); };
    const auto more = [&bits](unsigned shift) { return static_cast<unsigned char>(0x80 | bits(shift)); };
    out.insert(out.end(), {tag, more(21), more(14), more(7), bits(0)});
}

/// The ES descriptor of an esds around `config`: the ES's, its decoder configuration (MPEG-4 audio, an audio
/// stream) with the AudioSpecificConfig, and the sync layer's.
std::vector<unsigned char> es_descriptor(const std::vector<unsigned char> &config) {
    std::vector<unsigned char> out;
    descriptor(out, 0x03, 3 + 5 + 13 + 5 + config.size() + 5 + 1);
    out.insert(out.end(), {0, 0, 0});  // ES_ID and flags
    descriptor(out, 0x04, 13 + 5 + config.size());
    out.insert(out.end(), {0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});  // MPEG-4 audio; buffer and bitrates 0
    descriptor(out, 0x05, config.size());
    out.insert(out.end(), config.begin(), config.end());
    descriptor(out, 0x06, 1);
    out.push_back(0x02);
    return out;
}

bool aac(AudioFormatID format) {
    return format == kAudioFormatMPEG4AAC || format == kAudioFormatMPEG4AAC_HE ||
           format == kAudioFormatMPEG4AAC_HE_V2 || format == kAudioFormatMPEG4AAC_LD ||
           format == kAudioFormatMPEG4AAC_ELD;
}

/// The format `cookie` describes, read as `format`'s, its richest layer: FormatList's first entry, HE-AAC's at its
/// full rate where FormatInfo names the core's; false where AudioToolbox cannot read it.
bool describe(const Toolbox &calls, AudioFormatID format, const std::vector<unsigned char> &cookie,
              AudioStreamBasicDescription &source) {
    AudioFormatInfo info{};
    info.mASBD.mFormatID = format;
    info.mMagicCookie = cookie.data();
    info.mMagicCookieSize = static_cast<UInt32>(cookie.size());
    UInt32 length = 0;
    if (calls.format_size(kAudioFormatProperty_FormatList, sizeof info, &info, &length) != noErr ||
        length < sizeof(AudioFormatListItem) || length > 64 * sizeof(AudioFormatListItem)) {
        return false;
    }
    std::vector<AudioFormatListItem> items(length / sizeof(AudioFormatListItem));
    length = static_cast<UInt32>(items.size() * sizeof(AudioFormatListItem));
    if (calls.format(kAudioFormatProperty_FormatList, sizeof info, &info, &length, items.data()) != noErr ||
        length < sizeof(AudioFormatListItem)) {
        return false;
    }
    source = items[0].mASBD;
    return true;
}

/// An MP3 frame's header as its format, as Android's decoder reads one (C2SoftMp3Dec's parseMp3Header): MPEG-1,
/// MPEG-2 or MPEG-2.5, Layer III only, neither the free nor a bad bitrate, nor a reserved rate; false for another.
bool mp3_format(const std::vector<unsigned char> &header, AudioStreamBasicDescription &source) {
    if (header.size() != 4 || header[0] != 0xff || (header[1] & 0xe0) != 0xe0) return false;
    const unsigned version = header[1] >> 3 & 3u, layer = header[1] >> 1 & 3u;
    const unsigned bitrate = header[2] >> 4, rate = header[2] >> 2 & 3u;
    if (version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 || rate == 3) return false;
    static constexpr unsigned rates[] = {44100, 48000, 32000};
    source = {};
    source.mSampleRate = rates[rate] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    source.mFormatID = kAudioFormatMPEGLayer3;
    source.mFramesPerPacket = version == 3 ? 1152 : 576;
    source.mChannelsPerFrame = (header[3] >> 6) == 3 ? 1 : 2;
    return true;
}

/// The dfLa box around STREAMINFO, FLAC's magic cookie: its size and type, version and flags 0, and STREAMINFO as the
/// last metadata block; empty where `info` is not STREAMINFO's 34 bytes with Android's bits a sample (8, 16, 24, 32;
/// FLACDecoder's parseMetadata refuses the others).
std::vector<unsigned char> flac_cookie(const std::vector<unsigned char> &info) {
    if (info.size() != 34) return {};
    const unsigned bits = ((info[12] & 1u) << 4 | info[13] >> 4) + 1;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return {};
    std::vector<unsigned char> cookie = {0, 0, 0, 50, 'd', 'f', 'L', 'a', 0, 0, 0, 0, 0x80, 0, 0, 34};
    cookie.insert(cookie.end(), info.begin(), info.end());
    return cookie;
}

/// OpusHead (RFC 7845 section 5.1) as its format, as Android's decoder reads it (OpusHeader.cpp's ParseOpusHeader):
/// "OpusHead", major version 0, 1 or 2 channels in mapping family 0; in another family 1 to 8 channels, streams with
/// no more coupled ones than streams and as many channels as the head's, and a stream map of those channels or 255
/// (none), with `cookie` the head in family 1, which AudioToolbox takes (not 2 or 255 [measured]) and decodes the same
/// way; false for another.
bool opus_format(const std::vector<unsigned char> &head, AudioStreamBasicDescription &source,
                 std::vector<unsigned char> &cookie) {
    if (head.size() < 19 || std::memcmp(head.data(), "OpusHead", 8) != 0 || (head[8] & 0xf0) != 0) return false;
    const unsigned channels = head[9], family = head[18];
    if (channels < 1 || channels > (family == 0 ? 2u : 8u)) return false;
    if (family != 0) {
        if (head.size() < 21 + std::size_t{channels}) return false;
        const unsigned streams = head[19], coupled = head[20];
        if (coupled > streams || streams + coupled != channels) return false;
        for (unsigned n = 0; n < channels; ++n) {
            if (head[21 + n] != 255 && head[21 + n] >= channels) return false;
        }
        cookie.assign(head.begin(), head.begin() + 21 + channels);
        cookie[18] = 1;
    }
    source = {};
    source.mSampleRate = 48000;
    source.mFormatID = kAudioFormatOpus;
    source.mFramesPerPacket = 960;  // 20 ms, the usual; a packet of another length makes its own converter
    source.mChannelsPerFrame = channels;
    return true;
}

/// An Opus packet's frames at 48000 Hz by its table of contents (RFC 6716 section 3.1): its frames' length by the
/// configuration and their count by the code; 0 for a packet that is not one (code 3 without its count, no frames or
/// more than 120 ms).
std::uint32_t opus_frames(const std::vector<unsigned char> &packet) {
    if (packet.empty()) return 0;
    static constexpr std::uint32_t lengths[32] = {480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920,
                                                  2880, 480, 960, 480,  960, 120, 240, 480, 960, 120, 240,
                                                  480, 960, 120, 240, 480, 960, 120, 240, 480, 960};
    const std::uint32_t length = lengths[packet[0] >> 3], code = packet[0] & 3u;
    std::uint32_t count = code == 0 ? 1 : 2;
    if (code == 3) {
        if (packet.size() < 2) return 0;
        count = packet[1] & 0x3fu;
    }
    return count == 0 || length * count > 5760 ? 0 : length * count;
}

long destroy(std::uint64_t id) {
    std::shared_ptr<Codec> codec;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        const auto found = all.codecs.find(id);
        if (found == all.codecs.end()) return -linux_errno::ebadf;
        codec = std::move(found->second);
        all.codecs.erase(found);
    }
    const std::lock_guard hold(codec->mutex);  // after the call still running on it
    codec->gone = true;
    toolbox().dispose(codec->converter);
    return 0;
}

/// The source format and magic cookie of a codec of `type` with `config`, and its samples' bytes; false for a
/// configuration the host does not decode.
bool source_format(const Toolbox &calls, std::uint32_t type, const std::vector<unsigned char> &config,
                   AudioStreamBasicDescription &source, std::vector<unsigned char> &cookie, std::uint32_t &sample) {
    sample = 2;
    if (type == APKRT_AUDIO_CODEC_MP3) return mp3_format(config, source);
    if (type == APKRT_AUDIO_CODEC_AAC) {
        cookie = es_descriptor(config);
        return describe(calls, kAudioFormatMPEG4AAC, cookie, source) && aac(source.mFormatID) &&
               source.mChannelsPerFrame >= 1 && source.mChannelsPerFrame <= 8 &&
               source.mSampleRate >= 1 && source.mSampleRate <= 96000 && source.mFramesPerPacket >= 1 &&
               source.mFramesPerPacket <= 4096;
    }
    if (type == APKRT_AUDIO_CODEC_OPUS) return opus_format(config, source, cookie);
    sample = 4;
    cookie = flac_cookie(config);
    return !cookie.empty() && describe(calls, kAudioFormatFLAC, cookie, source) &&
           source.mFormatID == kAudioFormatFLAC && source.mChannelsPerFrame >= 1 && source.mChannelsPerFrame <= 8 &&
           source.mSampleRate >= 1 && source.mSampleRate <= 655350 && source.mFramesPerPacket >= 1 &&
           source.mFramesPerPacket <= 65535;
}

long create(std::uint64_t argument, apkrt_audio_codec &asked) {
    if (asked.type < APKRT_AUDIO_CODEC_AAC || asked.type > APKRT_AUDIO_CODEC_OPUS || asked.size == 0 ||
        asked.size > APKRT_AUDIO_CODEC_MAX_CONFIG) {
        return -linux_errno::einval;
    }
    std::vector<unsigned char> config;
    if (!take(asked.data, asked.size, config)) return -linux_errno::efault;
    const Toolbox &calls = toolbox();
    if (!calls.found()) return -linux_errno::enosys;
    AudioStreamBasicDescription source{};
    std::vector<unsigned char> cookie;
    std::uint32_t sample = 2;
    if (!source_format(calls, asked.type, config, source, cookie, sample)) return -linux_errno::einval;
    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = source.mSampleRate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    pcm.mChannelsPerFrame = source.mChannelsPerFrame;
    pcm.mBitsPerChannel = 8 * sample;
    pcm.mBytesPerFrame = sample * pcm.mChannelsPerFrame;
    pcm.mFramesPerPacket = 1;
    pcm.mBytesPerPacket = pcm.mBytesPerFrame;
    AudioConverterRef converter = nullptr;
    if (calls.make(&source, &pcm, &converter) != noErr) return -linux_errno::einval;
    if (!cookie.empty() && calls.set(converter, kAudioConverterDecompressionMagicCookie,
                                     static_cast<UInt32>(cookie.size()), cookie.data()) != noErr) {
        calls.dispose(converter);
        return -linux_errno::einval;
    }
    // An Opus packet gives up to 120 ms, whatever length its converter takes.
    const std::uint32_t most = asked.type == APKRT_AUDIO_CODEC_OPUS ? 5760 : source.mFramesPerPacket;
    std::uint64_t id = 0;
    long answer = 0;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        if (all.codecs.size() >= APKRT_AUDIO_CODEC_MAX_CODECS) {
            answer = -linux_errno::ebusy;
        } else {
            try {
                auto codec = std::make_shared<Codec>();
                codec->converter = converter;
                codec->channels = source.mChannelsPerFrame;
                codec->frames = most;
                codec->sample = sample;
                codec->type = asked.type;
                codec->source = source;
                codec->pcm = pcm;
                if (asked.type == APKRT_AUDIO_CODEC_OPUS) codec->cookie = cookie;
                codec->packet = source.mFramesPerPacket;
                codec->fresh = true;
                all.codecs.emplace(all.next_id, std::move(codec));
                id = all.next_id++;
            } catch (const std::bad_alloc &) {
                answer = -linux_errno::enomem;
            }
        }
    }
    if (answer != 0) {
        calls.dispose(converter);
        return answer;
    }
    asked.id = id;
    asked.rate = static_cast<std::uint32_t>(std::lround(source.mSampleRate));
    asked.channels = source.mChannelsPerFrame;
    asked.frames = most;
    if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&asked), sizeof asked)) {
        destroy(id);  // the guest cannot learn of it
        return -linux_errno::efault;
    }
    return 0;
}

/// An Opus codec's converter for packets of `packet` frames: its own, or a new one for another length; false where
/// AudioToolbox makes none (the old one stays).
bool opus_converter(Codec &codec, std::uint32_t packet) {
    if (packet == codec.packet) return true;
    AudioStreamBasicDescription source = codec.source;
    source.mFramesPerPacket = packet;
    AudioConverterRef converter = nullptr;
    if (toolbox().make(&source, &codec.pcm, &converter) != noErr) return false;
    const auto cookie_size = static_cast<UInt32>(codec.cookie.size());
    if (!codec.cookie.empty() &&
        toolbox().set(converter, kAudioConverterDecompressionMagicCookie, cookie_size, codec.cookie.data()) != noErr) {
        toolbox().dispose(converter);
        return false;
    }
    toolbox().dispose(codec.converter);
    codec.converter = converter;
    codec.packet = packet;
    codec.fresh = true;
    return true;
}

/// The one unit a DECODE hands the converter.
struct Feed {
    std::vector<unsigned char> *unit;
    bool given;
    AudioStreamPacketDescription packet;
};

/// The converter's input: the unit once, then no more for this call.
OSStatus feed(AudioConverterRef, UInt32 *packets, AudioBufferList *data, AudioStreamPacketDescription **described,
              void *client) {
    auto &fed = *static_cast<Feed *>(client);
    if (fed.given) {
        *packets = 0;
        data->mBuffers[0].mDataByteSize = 0;
        return 1;  // not an error of AudioToolbox's: the converter returns what it has
    }
    fed.given = true;
    *packets = 1;
    data->mBuffers[0].mData = fed.unit->data();
    data->mBuffers[0].mDataByteSize = static_cast<UInt32>(fed.unit->size());
    fed.packet = AudioStreamPacketDescription{0, 0, static_cast<UInt32>(fed.unit->size())};
    if (described != nullptr) *described = &fed.packet;
    return noErr;
}

long decode(std::uint64_t argument, apkrt_audio_codec &asked) {
    const auto codec = find(asked.id);
    if (codec == nullptr) return -linux_errno::ebadf;
    if (asked.size == 0 || asked.size > APKRT_AUDIO_CODEC_MAX_UNIT) return -linux_errno::einval;
    const std::uint64_t frame_bytes = std::uint64_t{codec->sample} * codec->channels;
    if (asked.capacity < codec->frames * frame_bytes) {
        asked.frames = codec->frames;
        if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&asked), sizeof asked)) {
            return -linux_errno::efault;
        }
        return -linux_errno::enospc;
    }
    std::vector<unsigned char> unit;
    std::vector<std::int32_t> pcm;  // 16-bit samples use its first half
    try {
        if (!take(asked.data, asked.size, unit)) return -linux_errno::efault;
        pcm.resize(std::size_t{codec->frames} * codec->channels * codec->sample / sizeof(std::int32_t) + 1);
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    UInt32 frames = codec->frames;
    {
        const std::lock_guard hold(codec->mutex);
        if (codec->gone) return -linux_errno::ebadf;
        const std::uint32_t packet = codec->type == APKRT_AUDIO_CODEC_OPUS ? opus_frames(unit) : 0;
        if (codec->type == APKRT_AUDIO_CODEC_OPUS && (packet == 0 || !opus_converter(*codec, packet))) {
            frames = 0;
        } else {
            Feed fed{&unit, false, {}};
            AudioBufferList list{};
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = codec->channels;
            list.mBuffers[0].mDataByteSize = static_cast<UInt32>(codec->frames * frame_bytes);
            list.mBuffers[0].mData = pcm.data();
            // frames: what came out
            const OSStatus status = toolbox().fill(codec->converter, feed, &fed, &frames, &list, nullptr);
            if (codec->type == APKRT_AUDIO_CODEC_OPUS) {
                if (status != noErr && status != 1) {
                    frames = 0;
                } else if (frames < packet && codec->fresh) {
                    // What a new converter keeps back, as silence before what it gave.
                    auto *bytes = reinterpret_cast<unsigned char *>(pcm.data());
                    const std::size_t kept = (packet - frames) * frame_bytes;
                    std::memmove(bytes + kept, bytes, frames * frame_bytes);
                    std::memset(bytes, 0, kept);
                    frames = packet;
                } else if (frames >= packet) {
                    codec->fresh = false;
                }
            }
        }
    }
    if (frames > codec->frames) frames = 0;
    if (frames > 0 && !sys::memory::copy_checked(asked.out, reinterpret_cast<std::uint64_t>(pcm.data()),
                                                 frames * frame_bytes)) {
        return -linux_errno::efault;
    }
    asked.frames = frames;
    if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&asked), sizeof asked)) {
        return -linux_errno::efault;
    }
    return 0;
}

long flush(std::uint64_t id) {
    const auto codec = find(id);
    if (codec == nullptr) return -linux_errno::ebadf;
    const std::lock_guard hold(codec->mutex);
    if (codec->gone) return -linux_errno::ebadf;
    toolbox().reset(codec->converter);
    return 0;
}

}  // namespace

long call(std::uint64_t op, std::uint64_t argument) {
    if (op == APKRT_AUDIO_CODEC_FLUSH) return flush(argument);
    if (op == APKRT_AUDIO_CODEC_DESTROY) return destroy(argument);
    if (op != APKRT_AUDIO_CODEC_CREATE && op != APKRT_AUDIO_CODEC_DECODE) return -linux_errno::einval;
    apkrt_audio_codec asked{};
    if (argument == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&asked), argument, sizeof asked)) {
        return -linux_errno::efault;
    }
    if (asked.reserved != 0) return -linux_errno::einval;
    return op == APKRT_AUDIO_CODEC_CREATE ? create(argument, asked) : decode(argument, asked);
}

}  // namespace apkrt::audio_codec
