// The host's audio decoding (audio_decode.h, the SoundPool plan's step 3). The guest's struct is read once and its
// file's bytes copied into host memory before AudioToolbox sees them; the frames go to the guest's buffer a chunk at a
// time, each checked, so a large buffer costs the host nothing. Only Android's formats are taken: the bytes' own
// signature picks the container (fLaC, an MP4's ftyp, an ID3 tag or an MPEG audio frame, an ADTS frame),
// and the file's data format must then be MP3, AAC (any of AudioToolbox's AAC types) or FLAC, as Android 15's
// SoundPool decodes them [observed: soundpool-codec-test]; AIFF, CAF, ALAC and AC-3 in MP4 and the rest, which
// ExtAudioFile also reads, are refused, as Android's SoundPool fails them. Opus in MP4, which Android's decodes, fails
// here too: ExtAudioFile takes no client format for it (a named difference until the plan's step 4). A read error
// after frames came ends the decode with them, as Sound.cpp's short decode does. AudioToolbox is opened by its path,
// not linked, as audio.cpp finds its AudioUnit getter: the app does not link it.
#include "audio_decode.h"

#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/ExtendedAudioFile.h>
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace apkrt::audio_decode {
namespace {

namespace linux_errno = sys::linux_errno;

static_assert(sizeof(apkrt_audio_decode) == 48 && offsetof(apkrt_audio_decode, frames) == 32);

/// AudioToolbox's functions, found once.
struct Toolbox {
    decltype(&AudioFileOpenWithCallbacks) open = nullptr;
    decltype(&AudioFileClose) close = nullptr;
    decltype(&AudioFileGetProperty) property = nullptr;
    decltype(&ExtAudioFileWrapAudioFileID) wrap = nullptr;
    decltype(&ExtAudioFileGetProperty) get = nullptr;
    decltype(&ExtAudioFileSetProperty) set = nullptr;
    decltype(&ExtAudioFileRead) read = nullptr;
    decltype(&ExtAudioFileDispose) dispose = nullptr;
    bool found() const { return open && close && property && wrap && get && set && read && dispose; }
};

const Toolbox &toolbox() {
    static Toolbox found;
    static std::once_flag once;
    std::call_once(once, [] {
        void *library = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY);
        if (library == nullptr) return;
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
        FIND(open, AudioFileOpenWithCallbacks);
        FIND(close, AudioFileClose);
        FIND(property, AudioFileGetProperty);
        FIND(wrap, ExtAudioFileWrapAudioFileID);
        FIND(get, ExtAudioFileGetProperty);
        FIND(set, ExtAudioFileSetProperty);
        FIND(read, ExtAudioFileRead);
        FIND(dispose, ExtAudioFileDispose);
#undef FIND
    });
    return found;
}

/// The container the bytes' signature names, as AudioFile's type, or 0 for none of Android's.
AudioFileTypeID container(const std::vector<unsigned char> &bytes) {
    const std::size_t size = bytes.size();
    const unsigned char *at = bytes.data();
    if (size >= 4 && std::memcmp(at, "fLaC", 4) == 0) return kAudioFileFLACType;
    if (size >= 8 && std::memcmp(at + 4, "ftyp", 4) == 0) return kAudioFileMPEG4Type;
    if (size >= 3 && std::memcmp(at, "ID3", 3) == 0) return kAudioFileMP3Type;
    if (size >= 2 && at[0] == 0xFF && (at[1] & 0xF6) == 0xF0) return kAudioFileAAC_ADTSType;  // sync, layer 0
    if (size >= 2 && at[0] == 0xFF && (at[1] & 0xE0) == 0xE0) return kAudioFileMP3Type;      // sync, any layer
    return 0;
}

/// Whether AudioToolbox's data format in that container is one of Android's.
bool android_format(AudioFileTypeID type, AudioFormatID format) {
    switch (format) {
        case kAudioFormatMPEGLayer3:
            return type == kAudioFileMP3Type || type == kAudioFileMPEG4Type;
        case kAudioFormatMPEG4AAC:
        case kAudioFormatMPEG4AAC_HE:
        case kAudioFormatMPEG4AAC_HE_V2:
        case kAudioFormatMPEG4AAC_LD:
        case kAudioFormatMPEG4AAC_ELD:
            return type == kAudioFileMPEG4Type || type == kAudioFileAAC_ADTSType;
        case kAudioFormatFLAC:
            return type == kAudioFileFLACType;
        default:
            return false;
    }
}

OSStatus read_bytes(void *client, SInt64 position, UInt32 count, void *buffer, UInt32 *read) {
    const auto &bytes = *static_cast<const std::vector<unsigned char> *>(client);
    if (position < 0 || static_cast<std::uint64_t>(position) > bytes.size()) return kAudioFilePositionError;
    const std::size_t taken = std::min<std::size_t>(count, bytes.size() - static_cast<std::size_t>(position));
    std::memcpy(buffer, bytes.data() + position, taken);
    *read = static_cast<UInt32>(taken);
    return noErr;
}

SInt64 size_of(void *client) {
    return static_cast<SInt64>(static_cast<const std::vector<unsigned char> *>(client)->size());
}

/// Decodes `bytes` into the guest's buffer; the answer, and the frames, rate and channels in `asked`.
long decode(const Toolbox &calls, std::vector<unsigned char> &bytes, apkrt_audio_decode &asked) {
    const AudioFileTypeID type = container(bytes);
    if (type == 0) return -linux_errno::einval;
    AudioFileID file = nullptr;
    if (calls.open(&bytes, read_bytes, nullptr, size_of, nullptr, type, &file) != noErr) return -linux_errno::einval;
    AudioStreamBasicDescription source{};
    UInt32 length = sizeof source;
    ExtAudioFileRef decoder = nullptr;
    if (calls.property(file, kAudioFilePropertyDataFormat, &length, &source) != noErr ||
        !android_format(type, source.mFormatID) || source.mChannelsPerFrame < 1 || source.mSampleRate < 1 ||
        source.mSampleRate > 384000 || calls.wrap(file, false, &decoder) != noErr) {
        calls.close(file);
        return -linux_errno::einval;
    }
    AudioStreamBasicDescription client{};
    client.mSampleRate = source.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    client.mChannelsPerFrame = source.mChannelsPerFrame;
    client.mBitsPerChannel = 16;
    client.mBytesPerFrame = 2 * client.mChannelsPerFrame;
    client.mFramesPerPacket = 1;
    client.mBytesPerPacket = client.mBytesPerFrame;
    long answer = 0;
    if (calls.set(decoder, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client) != noErr) {
        answer = -linux_errno::einval;
    } else {
        const std::uint64_t frame_bytes = client.mBytesPerFrame;
        const std::uint64_t room = asked.capacity / frame_bytes;
        std::vector<std::int16_t> chunk;
        try {
            chunk.resize(static_cast<std::size_t>(4096) * client.mChannelsPerFrame);
        } catch (const std::bad_alloc &) {
            answer = -linux_errno::enomem;
        }
        std::uint64_t written = 0;
        while (answer == 0 && written < room) {
            UInt32 frames = static_cast<UInt32>(std::min<std::uint64_t>(4096, room - written));
            AudioBufferList list{};
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = client.mChannelsPerFrame;
            list.mBuffers[0].mDataByteSize = static_cast<UInt32>(frames * frame_bytes);
            list.mBuffers[0].mData = chunk.data();
            if (calls.read(decoder, &frames, &list) != noErr) {
                if (written == 0) answer = -linux_errno::einval;
                break;
            }
            if (frames == 0) break;
            if (!sys::memory::copy_checked(asked.out + written * frame_bytes,
                                           reinterpret_cast<std::uint64_t>(chunk.data()), frames * frame_bytes)) {
                answer = -linux_errno::efault;
                break;
            }
            written += frames;
        }
        asked.frames = written;
        asked.rate = static_cast<std::uint32_t>(std::lround(source.mSampleRate));
        asked.channels = source.mChannelsPerFrame;
    }
    calls.dispose(decoder);
    calls.close(file);
    return answer;
}

}  // namespace

long call(std::uint64_t argument) {
    apkrt_audio_decode asked{};
    if (argument == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&asked), argument, sizeof asked)) {
        return -linux_errno::efault;
    }
    // A size of 0 names no format; bytes the guest does not have fail their copy, and a buffer it cannot write fails
    // at its first frame's.
    if (asked.size > APKRT_AUDIO_DECODE_MAX_FILE) return -linux_errno::einval;
    const Toolbox &calls = toolbox();
    if (!calls.found()) return -linux_errno::enosys;
    std::vector<unsigned char> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(asked.size));
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    if (!sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(bytes.data()), asked.data, asked.size)) {
        return -linux_errno::efault;
    }
    asked.frames = 0;
    asked.rate = 0;
    asked.channels = 0;
    const long answer = decode(calls, bytes, asked);
    if (answer != 0) return answer;
    if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&asked), sizeof asked)) {
        return -linux_errno::efault;
    }
    return 0;
}

}  // namespace apkrt::audio_decode
