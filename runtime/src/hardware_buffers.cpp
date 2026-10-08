// The guest's hardware buffers (hardware_buffers.h, ADR-0058 a)). The registry maps the runtime's IDs to buffers under
// one mutex; the host's functions run under no lock of the runtime's, as a lock may wait for the GPU's work on the
// buffer, and a buffer's own mutex guards only its count of locks. The host's free comes when the last holder lets the
// buffer go: its last release, or a call still in the host at that moment. The guest's struct is read once and its
// answers written back field by field, both checked: EFAULT for memory the guest does not have.
#include "hardware_buffers.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <new>

namespace apkrt::hardware_buffers {
namespace {

namespace linux_errno = sys::linux_errno;

static_assert(sizeof(apkrt_hardware_buffer) == 56);
static_assert(offsetof(apkrt_hardware_buffer, usage) == 24 && offsetof(apkrt_hardware_buffer, stride) == 32);
static_assert(offsetof(apkrt_hardware_buffer, reserved) == 44 && offsetof(apkrt_hardware_buffer, address) == 48);

/// The CPU's read and write bits of a usage (AHARDWAREBUFFER_USAGE_CPU_READ_MASK and _WRITE_MASK), a lock's only ones.
constexpr std::uint64_t cpu_usage = 0xff;

/// At most the bytes a pixel of an AHARDWAREBUFFER_FORMAT_* takes (the NDK's hardware_buffer.h and the VNDK's), for the
/// cap; 0 for a number that is no format. YUV's are rounded up, odd sizes included, and a format the host may keep in
/// a wider one counts the wider (D24S8 in Metal's Depth32Float_Stencil8).
std::uint32_t bytes_per_pixel(std::uint32_t format) {
    switch (format) {
        case 0x20203859:  // Y8 (VNDK)
        case 0x21:  // BLOB, its width in bytes
        case 0x35:  // S8_UINT
        case 0x38:  // R8_UNORM
            return 1;
        case 0x04:  // R5G6B5_UNORM
        case 0x10:  // YCbCr_422_SP (VNDK)
        case 0x11:  // YCrCb_420_SP (VNDK)
        case 0x14:  // YCbCr_422_I (VNDK)
        case 0x20:  // RAW16 (VNDK)
        case 0x24:  // RAW_OPAQUE (VNDK)
        case 0x25:  // RAW10 (VNDK)
        case 0x26:  // RAW12 (VNDK)
        case 0x20363159:  // Y16 (VNDK)
        case 0x32315659:  // YV12 (VNDK), 1.5
        case 0x23:  // Y8Cb8Cr8_420, 1.5
        case 0x30:  // D16_UNORM
        case 0x39:  // R16_UINT
        case 0x3d:  // R12_UINT
        case 0x3e:  // R14_UINT
            return 2;
        case 0x03:  // R8G8B8_UNORM
            return 3;
        case 0x01:  // R8G8B8A8_UNORM
        case 0x02:  // R8G8B8X8_UNORM
        case 0x05:  // B8G8R8A8_UNORM (VNDK)
        case 0x2b:  // R10G10B10A2_UNORM
        case 0x31:  // D24_UNORM
        case 0x33:  // D32_FLOAT
        case 0x36:  // YCbCr_P010, 3
        case 0x3a:  // R16G16_UINT
        case 0x3f:  // R12G12_UINT
        case 0x40:  // R14G14_UINT
        case 0x43:  // B10G10R10A2_UNORM
        case 0x44:  // B10G10R10X2_UNORM
            return 4;
        case 0x3c:  // YCbCr_P210, 4 and an odd width's half pixel
            return 6;
        case 0x16:  // R16G16B16A16_FLOAT
        case 0x22:  // IMPLEMENTATION_DEFINED (VNDK), the host's choice
        case 0x32:  // D24_UNORM_S8_UINT
        case 0x34:  // D32_FLOAT_S8_UINT
        case 0x3b:  // R10G10B10A10_UNORM
        case 0x41:  // R12G12B12A12_UINT
        case 0x42:  // R14G14B14A14_UINT
            return 8;
        default:
            return 0;
    }
}

/// What the runtime checks of a description before the host sees it: 0, -EINVAL, or -ENOMEM above the cap.
long check(const apkrt_hardware_buffer &asked) {
    if (asked.reserved != 0 || asked.width == 0 || asked.height == 0 || asked.layers == 0) return -linux_errno::einval;
    const std::uint32_t pixel = bytes_per_pixel(asked.format);
    if (pixel == 0) return -linux_errno::einval;
    std::uint64_t bytes = 0;
    if (__builtin_mul_overflow(std::uint64_t{asked.width}, std::uint64_t{asked.height}, &bytes) ||
        __builtin_mul_overflow(bytes, std::uint64_t{asked.layers}, &bytes) ||
        __builtin_mul_overflow(bytes, std::uint64_t{pixel}, &bytes) || bytes > APKRT_HARDWARE_BUFFER_MAX_BYTES) {
        return -linux_errno::enomem;
    }
    return 0;
}

/// An answer of the host's functions as an errno: 0, or -1 to -4095, anything else -EIO.
long host_answer(int answer) {
    return answer == 0 || (answer < 0 && answer >= -4095) ? answer : -linux_errno::eio;
}

apkrt_hardware_buffer_description description_of(const apkrt_hardware_buffer &asked) {
    return {asked.width, asked.height, asked.layers, asked.format, asked.usage};
}

/// A buffer of the host's, with the functions that made it.
struct Buffer {
    apkrt_hardware_buffer_description description{};
    std::uint32_t stride = 0;
    std::uint64_t handle = 0;
    apkrt_hardware_buffers table{};
    std::uint64_t references = 1;  // the registry's mutex guards it

    std::mutex mutex;  // for locks alone
    std::uint64_t locks = 0;

    ~Buffer() {
        for (; locks > 0; --locks) table.unlock(table.context, handle);
        table.free(table.context, handle);
    }
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Buffer>> buffers;
    std::uint64_t next_id = 1;
    apkrt_hardware_buffers table{};  // all NULL: none
};

Registry &registry() {
    static auto *one = new Registry;  // as long as the process: no host free at its exit
    return *one;
}

std::shared_ptr<Buffer> find(std::uint64_t id) {
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    const auto found = all.buffers.find(id);
    return found != all.buffers.end() ? found->second : nullptr;
}

long allocate(apkrt_hardware_buffer &asked) {
    if (const long refused = check(asked)) return refused;
    apkrt_hardware_buffers table;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        table = all.table;
    }
    if (table.allocate == nullptr) return -linux_errno::enosys;
    const apkrt_hardware_buffer_description description = description_of(asked);
    std::uint64_t handle = 0;
    std::uint32_t stride = 0;
    if (const int refused = table.allocate(table.context, &description, &handle, &stride)) {
        return host_answer(refused);
    }
    std::shared_ptr<Buffer> buffer;
    try {
        buffer = std::make_shared<Buffer>();
    } catch (const std::bad_alloc &) {
        table.free(table.context, handle);
        return -linux_errno::enomem;
    }
    buffer->description = description;
    buffer->stride = stride;
    buffer->handle = handle;
    buffer->table = table;
    std::uint64_t id;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        id = all.next_id++;
        try {
            all.buffers.emplace(id, buffer);
        } catch (const std::bad_alloc &) {
            return -linux_errno::enomem;  // the buffer goes with the last pointer, here
        }
    }
    asked.id = id;
    asked.stride = stride;
    return 0;
}

long supported(const apkrt_hardware_buffer &asked) {
    if (check(asked) != 0) return 0;
    apkrt_hardware_buffers table;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        table = all.table;
    }
    if (table.supported == nullptr) return 0;
    const apkrt_hardware_buffer_description description = description_of(asked);
    return table.supported(table.context, &description) == 1 ? 1 : 0;  // a host's error is no yes
}

long describe(apkrt_hardware_buffer &asked) {
    if (asked.reserved != 0) return -linux_errno::einval;
    const auto buffer = find(asked.id);
    if (!buffer) return -linux_errno::einval;
    asked.width = buffer->description.width;
    asked.height = buffer->description.height;
    asked.layers = buffer->description.layers;
    asked.format = buffer->description.format;
    asked.usage = buffer->description.usage;
    asked.stride = buffer->stride;
    return 0;
}

long acquire(std::uint64_t id) {
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    const auto found = all.buffers.find(id);
    if (found == all.buffers.end()) return -linux_errno::einval;
    ++found->second->references;
    return 0;
}

long release(std::uint64_t id) {
    std::shared_ptr<Buffer> last;  // freed when this goes, unless a call in the host still holds the buffer
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    const auto found = all.buffers.find(id);
    if (found == all.buffers.end()) return -linux_errno::einval;
    if (--found->second->references == 0) {
        last = std::move(found->second);
        all.buffers.erase(found);
    }
    return 0;
}

long lock(apkrt_hardware_buffer &asked) {
    if (asked.reserved != 0 || (asked.usage & ~cpu_usage) != 0) return -linux_errno::einval;
    const auto buffer = find(asked.id);
    if (!buffer) return -linux_errno::einval;
    void *address = nullptr;
    std::uint32_t pixel = 0, row = 0;
    if (const int refused = buffer->table.lock(buffer->table.context, buffer->handle, asked.usage, &address, &pixel,
                                               &row)) {
        return host_answer(refused);
    }
    {
        const std::lock_guard hold(buffer->mutex);
        ++buffer->locks;
    }
    asked.address = reinterpret_cast<std::uint64_t>(address);
    asked.bytes_per_pixel = pixel;
    asked.bytes_per_stride = row;
    return 0;
}

long unlock(std::uint64_t id) {
    const auto buffer = find(id);
    if (!buffer) return -linux_errno::einval;
    {
        const std::lock_guard hold(buffer->mutex);
        if (buffer->locks == 0) return -linux_errno::einval;
        --buffer->locks;
    }
    if (const int refused = buffer->table.unlock(buffer->table.context, buffer->handle)) {
        const std::lock_guard hold(buffer->mutex);
        ++buffer->locks;  // still locked, for a later unlock or the free
        return host_answer(refused);
    }
    return 0;
}

/// Writes `size` bytes of `value` to the guest's struct at `address` + `offset`; false where the guest cannot write.
bool put(std::uint64_t address, std::size_t offset, const void *value, std::size_t size) {
    return sys::memory::copy_checked(address + offset, reinterpret_cast<std::uint64_t>(value), size);
}

}  // namespace

long call(std::uint64_t op, std::uint64_t argument) {
    switch (op) {
        case APKRT_HARDWARE_BUFFER_ACQUIRE: return acquire(argument);
        case APKRT_HARDWARE_BUFFER_RELEASE: return release(argument);
        case APKRT_HARDWARE_BUFFER_UNLOCK: return unlock(argument);
        case APKRT_HARDWARE_BUFFER_ALLOCATE:
        case APKRT_HARDWARE_BUFFER_SUPPORTED:
        case APKRT_HARDWARE_BUFFER_DESCRIBE:
        case APKRT_HARDWARE_BUFFER_LOCK: break;
        default: return -linux_errno::einval;
    }
    apkrt_hardware_buffer asked;  // the guest's struct, read once
    if (argument == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&asked), argument, sizeof asked)) {
        return -linux_errno::efault;
    }
    switch (op) {
        case APKRT_HARDWARE_BUFFER_SUPPORTED: return supported(asked);
        case APKRT_HARDWARE_BUFFER_ALLOCATE: {
            if (const long refused = allocate(asked)) return refused;
            if (!put(argument, offsetof(apkrt_hardware_buffer, id), &asked.id, sizeof asked.id) ||
                !put(argument, offsetof(apkrt_hardware_buffer, stride), &asked.stride, sizeof asked.stride)) {
                release(asked.id);  // the guest cannot learn of it
                return -linux_errno::efault;
            }
            return 0;
        }
        case APKRT_HARDWARE_BUFFER_DESCRIBE: {
            if (const long refused = describe(asked)) return refused;
            const bool written = put(argument, offsetof(apkrt_hardware_buffer, width), &asked.width, 16) &&
                                 put(argument, offsetof(apkrt_hardware_buffer, usage), &asked.usage, 8) &&
                                 put(argument, offsetof(apkrt_hardware_buffer, stride), &asked.stride, 4);
            return written ? 0 : -linux_errno::efault;
        }
        default: {  // LOCK
            if (const long refused = lock(asked)) return refused;
            if (!put(argument, offsetof(apkrt_hardware_buffer, bytes_per_pixel), &asked.bytes_per_pixel, 8) ||
                !put(argument, offsetof(apkrt_hardware_buffer, address), &asked.address, sizeof asked.address)) {
                unlock(asked.id);  // the guest cannot learn of it
                return -linux_errno::efault;
            }
            return 0;
        }
    }
}

}  // namespace apkrt::hardware_buffers

extern "C" int apkrt_set_hardware_buffers(const apkrt_hardware_buffers *set) {
    using namespace apkrt::hardware_buffers;
    if (set != nullptr && (set->supported == nullptr || set->allocate == nullptr || set->lock == nullptr ||
                           set->unlock == nullptr || set->free == nullptr)) {
        return -1;
    }
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    all.table = set != nullptr ? *set : apkrt_hardware_buffers{};
    return 0;
}
