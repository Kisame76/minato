// The guest's video decoders (video_decoders.h, ADR-0060 e2b3). The registry maps the runtime's IDs to decoders under
// one mutex, which no call into the host holds; each decoder has a mutex of its own, held around every call of its
// host's functions, so that they come one at a time and a DESTROY waits for the call still running on another thread.
// IDs count up from 1 and are never used again: an ID after its DESTROY never reaches a newer decoder (-EBADF). The
// limit counts the host's decoders: a DESTROY's entry goes at once, and it counts until the host's decoder is gone. The
// guest's struct is read once; its bytes are copied into host memory before the host sees them, and a decoded frame
// into the guest's buffer after its description is checked, each size with 64-bit products: EFAULT for memory the
// guest does not have, the frame staying for a later DEQUEUE. Memory the runtime cannot get (a QUEUE's copy of up to
// 16 MiB, a decoder's entry) answers -ENOMEM: no exception leaves call(), whose caller is the syscall entry's
// assembly, and a host decoder made before the failure is destroyed.
#include "video_decoders.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace apkrt::video_decoders {
namespace {

namespace linux_errno = sys::linux_errno;

static_assert(sizeof(apkrt_video_decoder) == 64);
static_assert(offsetof(apkrt_video_decoder, flags) == 12 && offsetof(apkrt_video_decoder, width) == 16);
static_assert(offsetof(apkrt_video_decoder, size) == 32 && offsetof(apkrt_video_decoder, time_us) == 40);
static_assert(offsetof(apkrt_video_decoder, stride) == 48 && offsetof(apkrt_video_decoder, crop_left) == 56);

/// A frame's rows: at most four bytes a pixel of the largest width, more than NV12's 1 or P010's 2 need.
constexpr std::uint64_t max_stride = std::uint64_t{APKRT_VIDEO_DECODER_MAX_DIMENSION} * 4;

/// An answer of the host's functions as an errno: 0, or -1 to -4095, anything else -EIO.
long host_answer(int answer) {
    return answer == 0 || (answer < 0 && answer >= -4095) ? answer : -linux_errno::eio;
}

struct Decoder {
    apkrt_video_decoders table{};
    std::uint64_t handle = 0;
    std::mutex mutex;  // around each call of the host's functions
    bool gone = false;  // destroyed: no more calls
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Decoder>> decoders;
    std::uint32_t creating = 0;    // CREATEs in the host, counted against the limit
    std::uint32_t destroying = 0;  // DESTROYs whose entry went, until the host's decoder has gone too, counted as well
    std::uint64_t next_id = 1;
    apkrt_video_decoders table{};  // all NULL: none
};

Registry &registry() {
    static auto *one = new Registry;  // as long as the process
    return *one;
}

std::shared_ptr<Decoder> find(std::uint64_t id) {
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    const auto found = all.decoders.find(id);
    return found == all.decoders.end() ? nullptr : found->second;
}

/// Copies `size` of the guest's bytes at `address` into `bytes`; false where the guest does not have them.
bool take(std::uint64_t address, std::uint64_t size, std::vector<unsigned char> &bytes) {
    if (size == 0) return true;
    if (address == 0) return false;
    bytes.resize(size);
    return sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(bytes.data()), address, size);
}

/// Writes `size` bytes of `value` to the guest's struct at `address` + `offset`; false where the guest cannot write.
bool put(std::uint64_t address, std::size_t offset, const void *value, std::size_t size) {
    return sys::memory::copy_checked(address + offset, reinterpret_cast<std::uint64_t>(value), size);
}

long destroy(std::uint64_t id) {
    std::shared_ptr<Decoder> decoder;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        const auto found = all.decoders.find(id);
        if (found == all.decoders.end()) return -linux_errno::ebadf;
        decoder = std::move(found->second);
        all.decoders.erase(found);
        all.destroying++;
    }
    {
        const std::lock_guard hold(decoder->mutex);  // after the call still running on it
        decoder->gone = true;
        decoder->table.destroy(decoder->table.context, decoder->handle);
    }
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    all.destroying--;
    return 0;
}

long create(std::uint64_t argument, const apkrt_video_decoder &asked) {
    if (asked.type != APKRT_VIDEO_DECODER_AVC && asked.type != APKRT_VIDEO_DECODER_HEVC) return -linux_errno::einval;
    if (asked.width == 0 || asked.height == 0 || asked.width > APKRT_VIDEO_DECODER_MAX_DIMENSION ||
        asked.height > APKRT_VIDEO_DECODER_MAX_DIMENSION || asked.size > APKRT_VIDEO_DECODER_MAX_CODEC_DATA) {
        return -linux_errno::einval;
    }
    std::vector<unsigned char> codec_data;
    if (!take(asked.data, asked.size, codec_data)) return -linux_errno::efault;
    apkrt_video_decoders table{};
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        if (all.table.create == nullptr) return -linux_errno::enosys;
        if (all.decoders.size() + all.creating + all.destroying >= APKRT_VIDEO_DECODER_MAX_DECODERS) {
            return -linux_errno::ebusy;
        }
        all.creating++;
        table = all.table;
    }
    const apkrt_video_decoder_description description{asked.type, asked.width, asked.height,
                                                      codec_data.empty() ? nullptr : codec_data.data(),
                                                      codec_data.size()};
    std::uint64_t handle = 0;
    const long made = host_answer(table.create(table.context, &description, &handle));
    std::uint64_t id = 0;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        if (made == 0) {
            try {
                auto decoder = std::make_shared<Decoder>();
                decoder->table = table;
                decoder->handle = handle;
                all.decoders.emplace(all.next_id, std::move(decoder));
                id = all.next_id++;
            } catch (const std::bad_alloc &) {
                id = 0;  // the host's decoder goes below, counted until it has
            }
        }
        if (made != 0 || id != 0) all.creating--;
    }
    if (made != 0) return made;
    if (id == 0) {
        table.destroy(table.context, handle);
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        all.creating--;
        return -linux_errno::enomem;
    }
    if (!put(argument, offsetof(apkrt_video_decoder, id), &id, sizeof id)) {
        destroy(id);  // the guest cannot learn of it
        return -linux_errno::efault;
    }
    return 0;
}

long queue(const apkrt_video_decoder &asked) {
    const auto decoder = find(asked.id);
    if (decoder == nullptr) return -linux_errno::ebadf;
    if (asked.size > APKRT_VIDEO_DECODER_MAX_UNIT || (asked.flags & ~APKRT_VIDEO_DECODER_END_OF_STREAM) != 0) {
        return -linux_errno::einval;
    }
    std::vector<unsigned char> unit;
    if (!take(asked.data, asked.size, unit)) return -linux_errno::efault;
    const std::lock_guard hold(decoder->mutex);
    if (decoder->gone) return -linux_errno::ebadf;
    return host_answer(decoder->table.queue(decoder->table.context, decoder->handle,
                                            unit.empty() ? nullptr : unit.data(), unit.size(), asked.time_us,
                                            asked.flags));
}

/// The bytes of a frame the host describes, NV12 as guest_interface.h has it; 0 for a description that does not
/// hold: sizes outside the limits, rows narrower than the width, a crop outside the frame, a size other than the
/// rows'.
std::uint64_t frame_bytes(const apkrt_video_frame &frame) {
    if (frame.width == 0 || frame.height == 0 || frame.width > APKRT_VIDEO_DECODER_MAX_DIMENSION ||
        frame.height > APKRT_VIDEO_DECODER_MAX_DIMENSION || frame.stride < frame.width || frame.stride > max_stride ||
        frame.slice_height < frame.height || frame.slice_height > APKRT_VIDEO_DECODER_MAX_DIMENSION ||
        frame.crop_left > frame.crop_right || frame.crop_right >= frame.width || frame.crop_top > frame.crop_bottom ||
        frame.crop_bottom >= frame.height || frame.data == nullptr ||
        (frame.flags & ~APKRT_VIDEO_DECODER_END_OF_STREAM) != 0) {
        return 0;
    }
    // At most 32768 x (8192 + 4096) bytes: no product here wraps 64 bits.
    const std::uint64_t rows = std::uint64_t{frame.slice_height} + (std::uint64_t{frame.slice_height} + 1) / 2;
    const std::uint64_t bytes = std::uint64_t{frame.stride} * rows;
    return frame.size == bytes ? bytes : 0;
}

/// The frame's answers to the guest's struct: its rows, sizes, crop, time and flags.
bool put_frame(std::uint64_t argument, const apkrt_video_frame &frame, std::uint64_t size) {
    const std::uint32_t sizes[2] = {frame.width, frame.height};
    const std::uint32_t rows[2] = {frame.stride, frame.slice_height};
    const std::uint16_t crop[4] = {frame.crop_left, frame.crop_top, frame.crop_right, frame.crop_bottom};
    return put(argument, offsetof(apkrt_video_decoder, flags), &frame.flags, sizeof frame.flags) &&
           put(argument, offsetof(apkrt_video_decoder, width), sizes, sizeof sizes) &&
           put(argument, offsetof(apkrt_video_decoder, size), &size, sizeof size) &&
           put(argument, offsetof(apkrt_video_decoder, time_us), &frame.time_us, sizeof frame.time_us) &&
           put(argument, offsetof(apkrt_video_decoder, stride), rows, sizeof rows) &&
           put(argument, offsetof(apkrt_video_decoder, crop_left), crop, sizeof crop);
}

long dequeue(std::uint64_t argument, const apkrt_video_decoder &asked) {
    const auto decoder = find(asked.id);
    if (decoder == nullptr) return -linux_errno::ebadf;
    const std::lock_guard hold(decoder->mutex);
    if (decoder->gone) return -linux_errno::ebadf;
    apkrt_video_frame frame{};
    const long peeked = host_answer(decoder->table.peek(decoder->table.context, decoder->handle, &frame));
    if (peeked != 0) return peeked;
    if (frame.flags == APKRT_VIDEO_DECODER_END_OF_STREAM && frame.size == 0) {
        const apkrt_video_frame end{nullptr, 0, 0, 0, 0, 0, 0, 0, 0, 0, frame.time_us, frame.flags};
        if (!put_frame(argument, end, 0)) return -linux_errno::efault;
        decoder->table.pop(decoder->table.context, decoder->handle);
        return 0;
    }
    const std::uint64_t bytes = frame_bytes(frame);
    if (bytes == 0) return -linux_errno::eio;  // the host's frame stays its own
    if (asked.size < bytes) {
        return put_frame(argument, frame, bytes) ? -linux_errno::enospc : -linux_errno::efault;
    }
    if (asked.data == 0 ||
        !sys::memory::copy_checked(asked.data, reinterpret_cast<std::uint64_t>(frame.data), bytes) ||
        !put_frame(argument, frame, bytes)) {
        return -linux_errno::efault;
    }
    decoder->table.pop(decoder->table.context, decoder->handle);
    return 0;
}

long flush(std::uint64_t id) {
    const auto decoder = find(id);
    if (decoder == nullptr) return -linux_errno::ebadf;
    const std::lock_guard hold(decoder->mutex);
    if (decoder->gone) return -linux_errno::ebadf;
    return host_answer(decoder->table.flush(decoder->table.context, decoder->handle));
}

}  // namespace

long call(std::uint64_t op, std::uint64_t argument) try {
    switch (op) {
        case APKRT_VIDEO_DECODER_FLUSH: return flush(argument);
        case APKRT_VIDEO_DECODER_DESTROY: return destroy(argument);
        case APKRT_VIDEO_DECODER_CREATE:
        case APKRT_VIDEO_DECODER_QUEUE:
        case APKRT_VIDEO_DECODER_DEQUEUE: break;
        default: return -linux_errno::einval;
    }
    apkrt_video_decoder asked;  // the guest's struct, read once
    if (argument == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&asked), argument, sizeof asked)) {
        return -linux_errno::efault;
    }
    switch (op) {
        case APKRT_VIDEO_DECODER_CREATE: return create(argument, asked);
        case APKRT_VIDEO_DECODER_QUEUE: return queue(asked);
        default: return dequeue(argument, asked);
    }
} catch (const std::bad_alloc &) {
    return -linux_errno::enomem;
}

}  // namespace apkrt::video_decoders

extern "C" int apkrt_set_video_decoders(const apkrt_video_decoders *set) {
    using namespace apkrt::video_decoders;
    if (set != nullptr && (set->create == nullptr || set->queue == nullptr || set->peek == nullptr ||
                           set->pop == nullptr || set->flush == nullptr || set->destroy == nullptr)) {
        return -1;
    }
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    all.table = set != nullptr ? *set : apkrt_video_decoders{};
    return 0;
}
