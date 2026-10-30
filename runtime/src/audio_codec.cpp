// The guest's audio codecs (audio_codec.h, ADR-0060 e2f). The registry maps the runtime's IDs to AudioToolbox's
// converters under one mutex, which no converter call holds; each codec has a mutex of its own, held around every
// converter call, so that they come one at a time and a DESTROY waits for the call still running on another thread.
// IDs count up from 1 and are never used again. An AAC codec's configuration, csd-0's AudioSpecificConfig, goes to
// AudioToolbox inside an MPEG-4 ES descriptor, the magic cookie of an MP4 file's esds, from which AudioToolbox names
// the format, the rate, the channels and the frames a unit gives, of its richest layer (HE-AAC's SBR at the full rate,
// as Android's decoder gives it); only the AAC family is taken. A DECODE hands the
// converter its one unit and copies the frames it gives into the guest's buffer, each size checked first: a unit
// the converter cannot decode gives no frames, and the codec goes on with the next. The guest's struct is read once
// and the unit copied into host memory before AudioToolbox sees it. AudioToolbox is opened by its path, not linked,
// as audio_decode.cpp opens it.
#include "audio_codec.h"

#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

#include <AudioToolbox/AudioConverter.h>
#include <AudioToolbox/AudioFormat.h>
#include <dlfcn.h>

#include <cmath>
#include <cstddef>
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

/// The format `cookie` describes, its richest layer: FormatList's first entry, HE-AAC's at its full rate where
/// FormatInfo names the core's; false where AudioToolbox cannot read it.
bool describe(const Toolbox &calls, const std::vector<unsigned char> &cookie, AudioStreamBasicDescription &source) {
    AudioFormatInfo info{};
    info.mASBD.mFormatID = kAudioFormatMPEG4AAC;  // the cookie read as this format's
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

long create(std::uint64_t argument, apkrt_audio_codec &asked) {
    if (asked.type != APKRT_AUDIO_CODEC_AAC || asked.size == 0 || asked.size > APKRT_AUDIO_CODEC_MAX_CONFIG) {
        return -linux_errno::einval;
    }
    std::vector<unsigned char> config;
    if (!take(asked.data, asked.size, config)) return -linux_errno::efault;
    const Toolbox &calls = toolbox();
    if (!calls.found()) return -linux_errno::enosys;
    const std::vector<unsigned char> cookie = es_descriptor(config);
    AudioStreamBasicDescription source{};
    if (!describe(calls, cookie, source) || !aac(source.mFormatID) || source.mChannelsPerFrame < 1 ||
        source.mChannelsPerFrame > 8 || !(source.mSampleRate >= 1 && source.mSampleRate <= 96000) ||
        source.mFramesPerPacket < 1 || source.mFramesPerPacket > 4096) {
        return -linux_errno::einval;
    }
    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = source.mSampleRate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    pcm.mChannelsPerFrame = source.mChannelsPerFrame;
    pcm.mBitsPerChannel = 16;
    pcm.mBytesPerFrame = 2 * pcm.mChannelsPerFrame;
    pcm.mFramesPerPacket = 1;
    pcm.mBytesPerPacket = pcm.mBytesPerFrame;
    AudioConverterRef converter = nullptr;
    if (calls.make(&source, &pcm, &converter) != noErr) return -linux_errno::einval;
    if (calls.set(converter, kAudioConverterDecompressionMagicCookie, static_cast<UInt32>(cookie.size()),
                  cookie.data()) != noErr) {
        calls.dispose(converter);
        return -linux_errno::einval;
    }
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
                codec->frames = source.mFramesPerPacket;
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
    asked.frames = source.mFramesPerPacket;
    if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&asked), sizeof asked)) {
        destroy(id);  // the guest cannot learn of it
        return -linux_errno::efault;
    }
    return 0;
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
    const std::uint64_t frame_bytes = 2 * std::uint64_t{codec->channels};
    if (asked.capacity < codec->frames * frame_bytes) {
        asked.frames = codec->frames;
        if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&asked), sizeof asked)) {
            return -linux_errno::efault;
        }
        return -linux_errno::enospc;
    }
    std::vector<unsigned char> unit;
    std::vector<std::int16_t> pcm;
    try {
        if (!take(asked.data, asked.size, unit)) return -linux_errno::efault;
        pcm.resize(std::size_t{codec->frames} * codec->channels);
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    UInt32 frames = codec->frames;
    {
        const std::lock_guard hold(codec->mutex);
        if (codec->gone) return -linux_errno::ebadf;
        Feed fed{&unit, false, {}};
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = codec->channels;
        list.mBuffers[0].mDataByteSize = static_cast<UInt32>(pcm.size() * sizeof(std::int16_t));
        list.mBuffers[0].mData = pcm.data();
        toolbox().fill(codec->converter, feed, &fed, &frames, &list, nullptr);  // frames: what came out
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
