// The host's audio encoding (audio_encode.h, MediaRecorder's audio). One syscall carries every operation, x0 the op
// and x1 its argument, as audio_codec does. A session encodes an AAC-LC track into an MPEG-4 file on AudioToolbox
// (AudioFileInitializeWithCallbacks + ExtAudioFile), a chunk of PCM at a time. The file is never kept whole: the
// writer streams the mdat payload forward and the host hands those bytes straight to the guest as records (an offset
// and a run of bytes the guest pwrites to its output), freeing them at once; only the small reserved header at the
// front, which the writer patches and re-reads at the end, is held until the track closes. So the host's memory is
// bounded by the chunk, not by the recording's length. AudioToolbox is opened by its path, not linked, as
// audio_decode opens it.
#include "audio_encode.h"

#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

#include <AudioToolbox/AudioConverter.h>
#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/ExtendedAudioFile.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace apkrt::audio_encode {
namespace {

namespace linux_errno = sys::linux_errno;

static_assert(sizeof(apkrt_audio_encode_create) == 24 && offsetof(apkrt_audio_encode_create, id) == 16);
static_assert(sizeof(apkrt_audio_encode_io) == 56 && offsetof(apkrt_audio_encode_io, pending) == 48);
static_assert(sizeof(apkrt_audio_encode_record) == 16);

// The retained front (the header and the writer's reserve the moov patches at the end) must fit this; a moov larger
// than it (a recording of many hours) is refused rather than grown without bound (and never crashes a callback).
constexpr std::uint64_t front_cap = 16ull << 20;

/// AudioToolbox's functions, found once.
struct Toolbox {
    decltype(&AudioFileInitializeWithCallbacks) init = nullptr;
    decltype(&AudioFileClose) close = nullptr;
    decltype(&ExtAudioFileWrapAudioFileID) wrap = nullptr;
    decltype(&ExtAudioFileGetProperty) get = nullptr;
    decltype(&ExtAudioFileSetProperty) set = nullptr;
    decltype(&ExtAudioFileWrite) write = nullptr;
    decltype(&ExtAudioFileDispose) dispose = nullptr;
    decltype(&AudioConverterSetProperty) converter_set = nullptr;
    bool found() const { return init && close && wrap && get && set && write && dispose && converter_set; }
};

const Toolbox &toolbox() {
    static Toolbox found;
    static std::once_flag once;
    std::call_once(once, [] {
        void *library = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY);
        if (library == nullptr) return;
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
        FIND(init, AudioFileInitializeWithCallbacks);
        FIND(close, AudioFileClose);
        FIND(wrap, ExtAudioFileWrapAudioFileID);
        FIND(get, ExtAudioFileGetProperty);
        FIND(set, ExtAudioFileSetProperty);
        FIND(write, ExtAudioFileWrite);
        FIND(dispose, ExtAudioFileDispose);
        FIND(converter_set, AudioConverterSetProperty);
#undef FIND
    });
    return found;
}

/// A record the guest will pwrite: the bytes and the file offset they go to.
using Record = std::pair<std::uint64_t, std::vector<unsigned char>>;

struct Session {
    std::uint64_t id = 0;  // 0 while the slot is free
    bool finishing = false;
    bool finalized = false;
    std::uint32_t channels = 0;
    ExtAudioFileRef encoder = nullptr;
    AudioFileID file = nullptr;
    std::vector<unsigned char> front;       // retained [0, size): the header and reserve, patched at the end
    std::uint64_t mdat_start = ~0ull;        // the first forward-gap offset; at or above it the writes stream and drain
    std::uint64_t logical_size = 0;          // the file's size the writer sees
    std::deque<Record> pending;              // the mdat (and, after finalize, the front) records not yet drained
    bool bad = false;                        // an allocation the callbacks could not make, or the front past its cap
};

// Immortal: a guest thread may still be inside a syscall here while exit() runs the static destructors, so the lock
// and the session table are never torn down (as audio.cpp keeps its slots). close_session frees a session's own
// buffers at its end; exit leaks whatever is still open, which the process is discarding anyway.
[[clang::no_destroy]] constinit std::mutex table_lock;
[[clang::no_destroy]] Session sessions[APKRT_AUDIO_ENCODE_MAX_SESSIONS];
std::uint64_t next_id = 1;

/// The undrained record bytes the session holds.
std::uint64_t pending_bytes(const Session &session) {
    std::uint64_t total = 0;
    for (const Record &record : session.pending) total += sizeof(apkrt_audio_encode_record) + record.second.size();
    return total;
}

Session *find(std::uint64_t id) {
    if (id == 0) return nullptr;
    for (Session &session : sessions)
        if (session.id == id) return &session;
    return nullptr;
}

// MARK: - The AudioFile callbacks (they run inside ExtAudioFileWrite/Dispose, under the table lock)

OSStatus write_cb(void *client, SInt64 position, UInt32 count, const void *buffer, UInt32 *wrote) {
    Session &session = *static_cast<Session *>(client);
    if (position < 0) {
        *wrote = 0;
        return kAudioFilePositionError;
    }
    const std::uint64_t at = static_cast<std::uint64_t>(position);
    session.logical_size = std::max(session.logical_size, at + count);
    const unsigned char *bytes = static_cast<const unsigned char *>(buffer);
    // The first write past the front's end opens the mdat: from there the writer only appends, and never reads back.
    if (session.mdat_start == ~0ull && at > session.front.size()) session.mdat_start = at;
    if (at >= session.mdat_start) {
        try {
            session.pending.emplace_back(at, std::vector<unsigned char>(bytes, bytes + count));
        } catch (const std::exception &) {
            session.bad = true;
            *wrote = 0;
            return kAudioFileUnspecifiedError;
        }
    } else {
        if (at + count > front_cap) {  // a moov larger than the cap: refuse rather than grow without bound
            session.bad = true;
            *wrote = 0;
            return kAudioFilePositionError;
        }
        try {
            if (at + count > session.front.size()) session.front.resize(static_cast<std::size_t>(at + count));
        } catch (const std::exception &) {
            session.bad = true;
            *wrote = 0;
            return kAudioFileUnspecifiedError;
        }
        std::memcpy(session.front.data() + at, bytes, count);
    }
    *wrote = count;
    return noErr;
}

OSStatus read_cb(void *client, SInt64 position, UInt32 count, void *buffer, UInt32 *read) {
    const Session &session = *static_cast<const Session *>(client);
    if (position < 0) {
        *read = 0;
        return kAudioFilePositionError;
    }
    const std::uint64_t at = static_cast<std::uint64_t>(position);
    // The writer re-reads only its reserved front (to patch the header); the mdat is gone to the guest by then.
    if (at >= session.front.size()) {
        *read = 0;
        if (at < session.mdat_start) return noErr;  // within the reserve the writer has not filled yet
        return kAudioFilePositionError;
    }
    const std::uint64_t taken = std::min<std::uint64_t>(count, session.front.size() - at);
    std::memcpy(buffer, session.front.data() + at, taken);
    *read = static_cast<UInt32>(taken);
    return noErr;
}

SInt64 size_cb(void *client) { return static_cast<SInt64>(static_cast<const Session *>(client)->logical_size); }

OSStatus set_size_cb(void *client, SInt64 size) {
    Session &session = *static_cast<Session *>(client);
    if (size < 0) return kAudioFilePositionError;
    session.logical_size = static_cast<std::uint64_t>(size);
    if (static_cast<std::uint64_t>(size) < session.front.size()) {
        try {
            session.front.resize(static_cast<std::size_t>(size));
        } catch (const std::exception &) {
            session.bad = true;
            return kAudioFileUnspecifiedError;
        }
    }
    return noErr;
}

// MARK: - The operations

void close_session(Session &session) {
    const Toolbox &calls = toolbox();
    if (session.encoder != nullptr) calls.dispose(session.encoder);
    if (session.file != nullptr) calls.close(session.file);
    session = Session{};  // frees the front and any pending records, marks the slot free (id 0)
}

long op_create(std::uint64_t argument) {
    apkrt_audio_encode_create asked{};
    if (argument == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&asked), argument, sizeof asked)) {
        return -linux_errno::efault;
    }
    if (asked.reserved != 0 || asked.channels < 1 || asked.channels > 2 || asked.rate < 8000 || asked.rate > 384000) {
        return -linux_errno::einval;
    }
    const Toolbox &calls = toolbox();
    if (!calls.found()) return -linux_errno::enosys;

    const std::lock_guard<std::mutex> hold(table_lock);
    Session *slot = nullptr;
    for (Session &session : sessions)
        if (session.id == 0) {
            slot = &session;
            break;
        }
    if (slot == nullptr) return -linux_errno::ebusy;

    *slot = Session{};
    slot->channels = asked.channels;

    AudioStreamBasicDescription file_format{};
    file_format.mSampleRate = asked.rate;
    file_format.mFormatID = kAudioFormatMPEG4AAC;
    file_format.mFormatFlags = kMPEG4Object_AAC_LC;
    file_format.mChannelsPerFrame = asked.channels;
    file_format.mFramesPerPacket = 1024;
    if (calls.init(slot, read_cb, write_cb, size_cb, set_size_cb, kAudioFileM4AType, &file_format, 0, &slot->file) !=
        noErr) {
        close_session(*slot);
        return -linux_errno::einval;
    }
    if (calls.wrap(slot->file, true, &slot->encoder) != noErr) {
        close_session(*slot);
        return -linux_errno::einval;
    }
    AudioStreamBasicDescription source{};
    source.mSampleRate = asked.rate;
    source.mFormatID = kAudioFormatLinearPCM;
    source.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    source.mChannelsPerFrame = asked.channels;
    source.mBitsPerChannel = 16;
    source.mBytesPerFrame = 2 * asked.channels;
    source.mFramesPerPacket = 1;
    source.mBytesPerPacket = source.mBytesPerFrame;
    if (calls.set(slot->encoder, kExtAudioFileProperty_ClientDataFormat, sizeof source, &source) != noErr) {
        close_session(*slot);
        return -linux_errno::einval;
    }
    if (asked.bitrate > 0) {
        // Best effort: push the asked bit rate into the underlying converter; a rate it refuses keeps the default.
        AudioConverterRef converter = nullptr;
        UInt32 length = sizeof converter;
        if (calls.get(slot->encoder, kExtAudioFileProperty_AudioConverter, &length, &converter) == noErr &&
            converter != nullptr) {
            UInt32 rate = asked.bitrate;
            if (calls.converter_set(converter, kAudioConverterEncodeBitRate, sizeof rate, &rate) == noErr) {
                CFPropertyListRef config = nullptr;  // tell ExtAudioFile the converter changed
                calls.set(slot->encoder, kExtAudioFileProperty_ConverterConfig, sizeof config, &config);
            }
        }
    }

    slot->id = next_id++;
    asked.id = slot->id;
    if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&asked), sizeof asked)) {
        close_session(*slot);
        return -linux_errno::efault;
    }
    return 0;
}

/// Drains whole pending records into the guest's `out`; sets produced and pending. When not even one record fits:
/// returns 0 if this call took input (a WRITE with PCM — the guest drains next), else -ENOSPC. `pending` is set to
/// the next record's size either way.
long drain(Session &session, apkrt_audio_encode_io &io, bool took_input) {
    std::vector<unsigned char> blob;
    std::uint64_t remaining = io.capacity;
    while (!session.pending.empty()) {
        const Record &record = session.pending.front();
        const std::uint64_t need = sizeof(apkrt_audio_encode_record) + record.second.size();
        if (need > remaining) {
            if (blob.empty()) {
                io.produced = 0;
                io.pending = need;  // the next record's size, so the guest can size `out`
                // A WRITE that took PCM must not fail: it returns 0 and the guest drains next. Only a call that took
                // nothing (a drain-only WRITE or a FINISH) reports -ENOSPC.
                return took_input ? 0 : -linux_errno::enospc;
            }
            break;
        }
        apkrt_audio_encode_record header{record.first, record.second.size()};
        try {
            const auto *at = reinterpret_cast<const unsigned char *>(&header);
            blob.insert(blob.end(), at, at + sizeof header);
            blob.insert(blob.end(), record.second.begin(), record.second.end());
        } catch (const std::bad_alloc &) {
            return -linux_errno::enomem;
        }
        remaining -= need;
        session.pending.pop_front();
    }
    if (!blob.empty() &&
        !sys::memory::copy_checked(io.out, reinterpret_cast<std::uint64_t>(blob.data()), blob.size())) {
        return -linux_errno::efault;
    }
    io.produced = blob.size();
    io.pending = pending_bytes(session);
    return 0;
}

long op_io(std::uint64_t argument, bool finish) {
    apkrt_audio_encode_io io{};
    if (argument == 0 || !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&io), argument, sizeof io)) {
        return -linux_errno::efault;
    }
    const Toolbox &calls = toolbox();
    const std::lock_guard<std::mutex> hold(table_lock);
    Session *session = find(io.id);
    if (session == nullptr) return -linux_errno::ebadf;

    const bool took_input = !finish && io.size > 0;
    if (took_input) {
        // A WRITE with PCM after FINISH began would reach AudioToolbox with a disposed encoder; refuse before any copy.
        if (session->finalized || session->encoder == nullptr) return -linux_errno::einval;
        const std::uint64_t frame_bytes = 2ull * session->channels;
        if (io.size > APKRT_AUDIO_ENCODE_MAX_CHUNK || io.size % frame_bytes != 0) return -linux_errno::einval;
        // Bound the host's memory: refuse (taking nothing) while the undrained records exceed the cap; the guest
        // drains with size-0 WRITEs first. So the host holds at most the cap plus this one chunk.
        if (pending_bytes(*session) > APKRT_AUDIO_ENCODE_MAX_PENDING) return -linux_errno::eagain;
        std::vector<std::int16_t> pcm;
        try {
            pcm.resize(static_cast<std::size_t>(io.size / sizeof(std::int16_t)));
        } catch (const std::bad_alloc &) {
            return -linux_errno::enomem;
        }
        if (!sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(pcm.data()), io.data, io.size)) {
            return -linux_errno::efault;
        }
        const std::uint32_t channels = session->channels;
        const std::size_t total = pcm.size() / channels;
        std::size_t done = 0;
        while (done < total) {
            const UInt32 frames = static_cast<UInt32>(std::min<std::size_t>(4096, total - done));
            AudioBufferList list{};
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = channels;
            list.mBuffers[0].mDataByteSize = frames * 2 * channels;
            list.mBuffers[0].mData = pcm.data() + done * channels;
            if (calls.write(session->encoder, frames, &list) != noErr) {
                close_session(*session);
                return -linux_errno::einval;
            }
            done += frames;
        }
    }

    if (finish && !session->finalized) {
        // Close the track: the writer flushes the last mdat, writes the moov into the front and patches the header.
        calls.dispose(session->encoder);
        session->encoder = nullptr;
        calls.close(session->file);
        session->file = nullptr;
        // Hand the finished front (header + moov) over as the last record BEFORE marking finalized, so a failure
        // here fails the session rather than leaving it "finalized" without the header record.
        if (!session->front.empty()) {
            try {
                session->pending.emplace_back(0, std::move(session->front));
            } catch (const std::bad_alloc &) {
                close_session(*session);
                return -linux_errno::enomem;
            }
            session->front.clear();
        }
        session->finalized = true;
        session->finishing = true;
    }

    if (session->bad) {
        close_session(*session);
        return -linux_errno::enomem;
    }

    const long answer = drain(*session, io, took_input);
    if (answer != 0) {
        // -ENOSPC or -EFAULT: the session stays so the guest can retry with a larger buffer.
        sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&io), sizeof io);
        return answer;
    }
    if (!sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(&io), sizeof io)) {
        return -linux_errno::efault;
    }
    if (session->finishing && session->pending.empty()) close_session(*session);  // all written: the session ends
    return 0;
}

long op_abort(std::uint64_t id) {
    const std::lock_guard<std::mutex> hold(table_lock);
    Session *session = find(id);
    if (session == nullptr) return -linux_errno::ebadf;
    close_session(*session);
    return 0;
}

}  // namespace

long call(std::uint64_t op, std::uint64_t argument) {
    switch (op) {
        case APKRT_AUDIO_ENCODE_CREATE:
            return op_create(argument);
        case APKRT_AUDIO_ENCODE_WRITE:
            return op_io(argument, false);
        case APKRT_AUDIO_ENCODE_FINISH:
            return op_io(argument, true);
        case APKRT_AUDIO_ENCODE_ABORT:
            return op_abort(argument);
        default:
            return -linux_errno::einval;
    }
}

}  // namespace apkrt::audio_encode
