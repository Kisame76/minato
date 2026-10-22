// libnativewindow.so's AHardwareBuffer (ADR-0058 a)): the runtime keeps the process's buffers by an ID of its own,
// with their description, references and locks, and graphics/host's hardware buffers hold their memory, IOSurfaces
// in Minato's one process (hardware_buffer syscall, guest_interface.h). An AHardwareBuffer* is its ID shifted by 4,
// never dereferenced, so the library keeps no state: the same buffer is the same pointer on every thread, and the
// runtime's references alone decide when it goes.
// - Android's rules for a description are checked here, with Android 15's answers (graphics/tests/ahb_test.out);
//   the runtime re-checks what keeps the host safe. A valid description the host cannot make answers the
//   allocator's UNSUPPORTED, 7, as Android 15 does for a format its gralloc lacks.
// - A lock waits for and closes its fence, as Android does; an unlock answers no fence (-1), and 0 without a lock,
//   as the emulator's gralloc does. A second lock while locked nests, as IOSurface's do, where the emulator's
//   gralloc answers -38.
// - lockPlanes gives one plane, and for Y8Cb8Cr8_420 and YCbCr_P010 three (ADR-0060 e2a): Y at the lock's address
//   and rows, then Cb and Cr interleaved (NV12 and P010, pixel stride twice a sample's bytes) with the same rows
//   right below Y's height, as graphics/host lays them out.
// - The send and the receive pass the ID over the socket, which works within the one process there is: the send
//   takes a reference for the receiver, as Android's file descriptor in flight keeps the buffer, and the receive
//   takes it over. A buffer sent and never received lives until the process ends.
#include <android/hardware_buffer.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

namespace {

// Android's status codes (utils/Errors.h).
constexpr int kBadValue = -EINVAL;
constexpr int kInvalidOperation = -ENOSYS;
// Android 15's allocator's UNSUPPORTED (Error3 7), which AHardwareBuffer_allocate answers as it is, positive, for a
// valid description it cannot make.
constexpr int kUnsupported = 7;

constexpr uint64_t kCpuMask = AHARDWAREBUFFER_USAGE_CPU_READ_MASK | AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK;
constexpr uint64_t kCubeAndMips = AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP | AHARDWAREBUFFER_USAGE_GPU_MIPMAP_COMPLETE;
constexpr uint64_t kBlobRefused = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                                  kCubeAndMips | AHARDWAREBUFFER_USAGE_VIDEO_ENCODE;

// What the send writes and the receive reads: "MNAH", then the ID.
constexpr uint32_t kMessageMagic = 0x48414e4d;
struct Message {
    uint32_t magic, reserved;
    uint64_t id;
};

uint64_t id_of(const AHardwareBuffer* buffer) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(buffer);
    return (value & 0xf) == 0 ? value >> 4 : 0;
}

AHardwareBuffer* buffer_of(uint64_t id) {
    return reinterpret_cast<AHardwareBuffer*>(static_cast<uintptr_t>(id) << 4);
}

// The runtime's answer: 0 or a positive value, else -errno.
long call(apkrt_hardware_buffer_op op, uintptr_t argument) {
    long result;
    do {
        result = syscall(APKRT_NR_HARDWARE_BUFFER, op, argument);
    } while (result == -1 && errno == EINTR);
    return result == -1 ? -errno : result;
}

bool is_yuv(uint32_t format) {
    return format == AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 || format == AHARDWAREBUFFER_FORMAT_YCbCr_P010;
}

// Android's rules for a description, as AHardwareBuffer_allocate and isSupported apply them.
bool valid(const AHardwareBuffer_Desc* desc) {
    if (desc->width == 0 || desc->height == 0 || desc->layers == 0) return false;
    if (desc->rfu0 != 0 || desc->rfu1 != 0) return false;
    if (desc->format == AHARDWAREBUFFER_FORMAT_BLOB) {
        if (desc->height != 1 || desc->layers != 1 || (desc->usage & kBlobRefused) != 0) return false;
    } else if (is_yuv(desc->format)) {
        if (desc->layers != 1 || (desc->usage & kCubeAndMips) != 0) return false;
    } else if ((desc->usage & AHARDWAREBUFFER_USAGE_SENSOR_DIRECT_DATA) != 0) {
        return false;
    }
    if ((desc->usage & kCpuMask) != 0 && (desc->usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) != 0) return false;
    if ((desc->usage & AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP) != 0 &&
        (desc->width != desc->height || desc->layers % 6 != 0)) {
        return false;
    }
    return true;
}

apkrt_hardware_buffer request(const AHardwareBuffer_Desc* desc) {
    apkrt_hardware_buffer buffer{};
    buffer.width = desc->width;
    buffer.height = desc->height;
    buffer.layers = desc->layers;
    buffer.format = desc->format;
    buffer.usage = desc->usage;
    return buffer;
}

bool describe(const AHardwareBuffer* handle, apkrt_hardware_buffer* buffer) {
    *buffer = apkrt_hardware_buffer{};
    buffer->id = id_of(handle);
    return buffer->id != 0 && call(APKRT_HARDWARE_BUFFER_DESCRIBE, reinterpret_cast<uintptr_t>(buffer)) == 0;
}

// A lock's checks and the runtime's lock: 0 with the buffer's lock answers, else Android's error. The fence is
// waited on and closed whatever the answer, as the lock owns it.
int lock(AHardwareBuffer* handle, uint64_t usage, int32_t fence, const ARect* rect, apkrt_hardware_buffer* buffer) {
    if (fence >= 0) {
        pollfd waiting{fence, POLLIN, 0};
        while (poll(&waiting, 1, -1) == -1 && errno == EINTR) {
        }
        close(fence);
    }
    if ((usage & ~kCpuMask) != 0 || !describe(handle, buffer)) return kBadValue;
    if (buffer->layers > 1) return kInvalidOperation;
    if (rect != nullptr && (rect->left < 0 || rect->top < 0 || rect->right > static_cast<int32_t>(buffer->width) ||
                            rect->bottom > static_cast<int32_t>(buffer->height))) {
        return kBadValue;
    }
    buffer->usage = usage;
    return static_cast<int>(call(APKRT_HARDWARE_BUFFER_LOCK, reinterpret_cast<uintptr_t>(buffer)));
}

}  // namespace

extern "C" {

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer) {
    if (desc == nullptr || outBuffer == nullptr || !valid(desc)) return kBadValue;
    apkrt_hardware_buffer buffer = request(desc);
    if (call(APKRT_HARDWARE_BUFFER_SUPPORTED, reinterpret_cast<uintptr_t>(&buffer)) == 0) return kUnsupported;
    const long result = call(APKRT_HARDWARE_BUFFER_ALLOCATE, reinterpret_cast<uintptr_t>(&buffer));
    if (result != 0) return static_cast<int>(result);
    *outBuffer = buffer_of(buffer.id);
    return 0;
}

int AHardwareBuffer_isSupported(const AHardwareBuffer_Desc* desc) {
    if (desc == nullptr || !valid(desc)) return 0;
    apkrt_hardware_buffer buffer = request(desc);
    return call(APKRT_HARDWARE_BUFFER_SUPPORTED, reinterpret_cast<uintptr_t>(&buffer)) > 0 ? 1 : 0;
}

void AHardwareBuffer_acquire(AHardwareBuffer* buffer) {
    if (id_of(buffer) != 0) call(APKRT_HARDWARE_BUFFER_ACQUIRE, id_of(buffer));
}

void AHardwareBuffer_release(AHardwareBuffer* buffer) {
    if (id_of(buffer) != 0) call(APKRT_HARDWARE_BUFFER_RELEASE, id_of(buffer));
}

void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc) {
    apkrt_hardware_buffer described;
    if (outDesc == nullptr || !describe(buffer, &described)) return;
    *outDesc = AHardwareBuffer_Desc{};
    outDesc->width = described.width;
    outDesc->height = described.height;
    outDesc->layers = described.layers;
    outDesc->format = described.format;
    outDesc->usage = described.usage;
    outDesc->stride = described.stride;
}

int AHardwareBuffer_getId(const AHardwareBuffer* buffer, uint64_t* outId) {
    if (id_of(buffer) == 0 || outId == nullptr) return kBadValue;
    *outId = id_of(buffer);
    return 0;
}

int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect,
                         void** outVirtualAddress) {
    if (outVirtualAddress == nullptr) {
        if (fence >= 0) close(fence);
        return kBadValue;
    }
    apkrt_hardware_buffer locked;
    const int result = lock(buffer, usage, fence, rect, &locked);
    if (result == 0) *outVirtualAddress = reinterpret_cast<void*>(static_cast<uintptr_t>(locked.address));
    return result;
}

int AHardwareBuffer_lockAndGetInfo(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect,
                                   void** outVirtualAddress, int32_t* outBytesPerPixel, int32_t* outBytesPerStride) {
    if (outVirtualAddress == nullptr || outBytesPerPixel == nullptr || outBytesPerStride == nullptr) {
        if (fence >= 0) close(fence);
        return kBadValue;
    }
    apkrt_hardware_buffer locked;
    const int result = lock(buffer, usage, fence, rect, &locked);
    if (result != 0) return result;
    *outVirtualAddress = reinterpret_cast<void*>(static_cast<uintptr_t>(locked.address));
    *outBytesPerPixel = static_cast<int32_t>(locked.bytes_per_pixel);
    *outBytesPerStride = static_cast<int32_t>(locked.bytes_per_stride);
    return 0;
}

int AHardwareBuffer_lockPlanes(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect,
                               AHardwareBuffer_Planes* outPlanes) {
    if (outPlanes == nullptr) {
        if (fence >= 0) close(fence);
        return kBadValue;
    }
    apkrt_hardware_buffer locked;
    const int result = lock(buffer, usage, fence, rect, &locked);
    if (result != 0) return result;
    *outPlanes = AHardwareBuffer_Planes{};
    uint8_t* const address = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(locked.address));
    outPlanes->planes[0].data = address;
    outPlanes->planes[0].pixelStride = locked.bytes_per_pixel;
    outPlanes->planes[0].rowStride = locked.bytes_per_stride;
    if (!is_yuv(locked.format)) {
        outPlanes->planeCount = 1;
        return 0;
    }
    uint8_t* const chroma = address + static_cast<size_t>(locked.bytes_per_stride) * locked.height;
    const uint32_t sample = locked.bytes_per_pixel;
    outPlanes->planeCount = 3;
    outPlanes->planes[1] = {chroma, 2 * sample, locked.bytes_per_stride};
    outPlanes->planes[2] = {chroma + sample, 2 * sample, locked.bytes_per_stride};
    return 0;
}

int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* fence) {
    if (id_of(buffer) == 0) return kBadValue;
    const long result = call(APKRT_HARDWARE_BUFFER_UNLOCK, id_of(buffer));
    // An unlock without a lock answers 0, as Android 15's gralloc does; an unknown buffer stays an error.
    apkrt_hardware_buffer described;
    if (result != 0 && !(result == -EINVAL && describe(buffer, &described))) return static_cast<int>(result);
    if (fence != nullptr) *fence = -1;
    return 0;
}

int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer* buffer, int socketFd) {
    const uint64_t id = id_of(buffer);
    if (id == 0) return kBadValue;
    if (call(APKRT_HARDWARE_BUFFER_ACQUIRE, id) != 0) return kBadValue;
    const Message message{kMessageMagic, 0, id};
    ssize_t sent;
    do {
        sent = send(socketFd, &message, sizeof message, MSG_NOSIGNAL);
    } while (sent == -1 && errno == EINTR);
    if (sent == static_cast<ssize_t>(sizeof message)) return 0;
    const int error = sent == -1 ? -errno : -EIO;
    call(APKRT_HARDWARE_BUFFER_RELEASE, id);
    return error;
}

int AHardwareBuffer_recvHandleFromUnixSocket(int socketFd, AHardwareBuffer** outBuffer) {
    if (outBuffer == nullptr) return kBadValue;
    Message message{};
    ssize_t received;
    do {
        received = recv(socketFd, &message, sizeof message, MSG_WAITALL);
    } while (received == -1 && errno == EINTR);
    if (received == -1) return -errno;
    if (received != static_cast<ssize_t>(sizeof message) || message.magic != kMessageMagic || message.id == 0) {
        return kBadValue;
    }
    *outBuffer = buffer_of(message.id);
    return 0;
}

}  // extern "C"
