// libandroid.so's AHardwareBuffer functions (ADR-0058 a)): forwards to libnativewindow.so, which holds them, as
// window_forwards.cpp forwards the window's. Each calls libnativewindow's function of the same name through its
// version LIBNATIVEWINDOW, since a plain reference would bind to libandroid's own.
#include <android/hardware_buffer.h>

#define FORWARD(result, name, parameters, arguments)                                    \
    extern "C" result minato_forward_##name parameters;                                \
    __asm__(".symver minato_forward_" #name ", " #name "@LIBNATIVEWINDOW");             \
    extern "C" result name parameters { return minato_forward_##name arguments; }

FORWARD(int, AHardwareBuffer_allocate, (const AHardwareBuffer_Desc* desc, AHardwareBuffer** buffer), (desc, buffer))
FORWARD(int, AHardwareBuffer_isSupported, (const AHardwareBuffer_Desc* desc), (desc))
FORWARD(void, AHardwareBuffer_acquire, (AHardwareBuffer* buffer), (buffer))
FORWARD(void, AHardwareBuffer_release, (AHardwareBuffer* buffer), (buffer))
FORWARD(void, AHardwareBuffer_describe, (const AHardwareBuffer* buffer, AHardwareBuffer_Desc* desc), (buffer, desc))
FORWARD(int, AHardwareBuffer_getId, (const AHardwareBuffer* buffer, uint64_t* id), (buffer, id))
FORWARD(int, AHardwareBuffer_lock,
        (AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect, void** address),
        (buffer, usage, fence, rect, address))
FORWARD(int, AHardwareBuffer_lockAndGetInfo,
        (AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect, void** address,
         int32_t* bytes_per_pixel, int32_t* bytes_per_stride),
        (buffer, usage, fence, rect, address, bytes_per_pixel, bytes_per_stride))
FORWARD(int, AHardwareBuffer_lockPlanes,
        (AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect, AHardwareBuffer_Planes* planes),
        (buffer, usage, fence, rect, planes))
FORWARD(int, AHardwareBuffer_unlock, (AHardwareBuffer* buffer, int32_t* fence), (buffer, fence))
FORWARD(int, AHardwareBuffer_sendHandleToUnixSocket, (const AHardwareBuffer* buffer, int socket), (buffer, socket))
FORWARD(int, AHardwareBuffer_recvHandleFromUnixSocket, (int socket, AHardwareBuffer** buffer), (socket, buffer))
