/* ADR-0058 a): ahb-test, libnativewindow's AHardwareBuffer on graphics/host's hardware buffers (hardware_buffer.m),
   with Android 15's answers from the emulator, where it runs as a shell program (tools/android-reference.sh):
   - isSupported and allocate for a)'s formats and for descriptions Android refuses;
   - describe, getId, acquire and release; a pattern written under one lock and read back under another for each
     format, lockAndGetInfo, lockPlanes, a lock with a rectangle, and the locks Android refuses;
   - a BLOB, and the send and the receive over a socket pair;
   - the YUV formats (ADR-0060 e2a): isSupported for their usages and shapes, the locks, and lockPlanes' three planes
     with a pattern through them.
   No value of the emulator's gralloc alone is printed: a stride as "at least the width", an address as "the same".
   Left out, as the emulator's gralloc answers them for itself: R8 and more than one layer, which it does not make
   (graphics.hardware_buffers has them), and a buffer of 1 GiB, at which the emulator crashed; and a lock without an
   out, at which Android crashes. A second lock while locked is gralloc's too: the emulator's answers -38, where
   IOSurface nests (graphics.hardware_buffers). */
#include <android/hardware_buffer.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const uint64_t CPU = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

struct format {
    const char *name;
    uint32_t code;
    uint32_t bytes;
};
static const struct format FORMATS[] = {
    {"R8G8B8A8_UNORM", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 4},
    {"R8G8B8X8_UNORM", AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 4},
    {"R5G6B5_UNORM", AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, 2},
    {"R16G16B16A16_FLOAT", AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, 8},
    {"R10G10B10A2_UNORM", AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM, 4},
};

static AHardwareBuffer_Desc desc(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint64_t usage) {
    AHardwareBuffer_Desc description;
    memset(&description, 0, sizeof description);
    description.width = width;
    description.height = height;
    description.layers = layers;
    description.format = format;
    description.usage = usage;
    return description;
}

static uint8_t pattern(uint32_t format, uint32_t row, uint32_t column) {
    return (uint8_t)(format * 31 + row * 7 + column * 13);
}

/* Writes the pattern under a write lock and reads it back under a read lock: 1 when every byte came back. */
static int round_trip(AHardwareBuffer *buffer, uint32_t format, uint32_t rows, uint32_t row_bytes, uint32_t stride) {
    void *address = NULL;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &address) != 0) return 0;
    for (uint32_t row = 0; row < rows; row++) {
        for (uint32_t column = 0; column < row_bytes; column++) {
            ((uint8_t *)address)[(size_t)row * stride + column] = pattern(format, row, column);
        }
    }
    int32_t fence = 0;
    if (AHardwareBuffer_unlock(buffer, &fence) != 0 || fence != -1) return 0;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &address) != 0) return 0;
    int same = 1;
    for (uint32_t row = 0; row < rows && same; row++) {
        for (uint32_t column = 0; column < row_bytes; column++) {
            if (((const uint8_t *)address)[(size_t)row * stride + column] != pattern(format, row, column)) {
                same = 0;
                break;
            }
        }
    }
    return AHardwareBuffer_unlock(buffer, NULL) == 0 && same;
}

static void supported_and_refused(void) {
    for (size_t index = 0; index < sizeof FORMATS / sizeof *FORMATS; index++) {
        AHardwareBuffer_Desc cpu = desc(64, 48, 1, FORMATS[index].code, CPU);
        AHardwareBuffer_Desc gpu =
            desc(64, 48, 1, FORMATS[index].code, CPU | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE);
        printf("isSupported %s: CPU %d, CPU and GPU sampled %d\n", FORMATS[index].name,
               AHardwareBuffer_isSupported(&cpu),
               AHardwareBuffer_isSupported(&gpu));
    }
    AHardwareBuffer_Desc blob = desc(4096, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB, CPU);
    printf("isSupported BLOB: %d\n", AHardwareBuffer_isSupported(&blob));
    struct {
        const char *what;
        AHardwareBuffer_Desc description;
    } refused[] = {
        {"width 0", desc(0, 48, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, CPU)},
        {"height 0", desc(64, 0, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, CPU)},
        {"layers 0", desc(64, 48, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, CPU)},
        {"rfu0 1", desc(64, 48, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, CPU)},
        {"format 0x99", desc(64, 48, 1, 0x99, CPU)},
        {"BLOB of height 2", desc(4096, 2, 1, AHARDWAREBUFFER_FORMAT_BLOB, CPU)},
        {"BLOB of 2 layers", desc(4096, 1, 2, AHARDWAREBUFFER_FORMAT_BLOB, CPU)},
        {"BLOB sampled", desc(4096, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB, CPU | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)},
        {"protected with the CPU",
         desc(64, 48, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, CPU | AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT)},
        {"sensor data on RGBA",
         desc(64, 48, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, CPU | AHARDWAREBUFFER_USAGE_SENSOR_DIRECT_DATA)},
        {"cube map 64 x 48", desc(64, 48, 6, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP)},
        {"cube map of 5 layers", desc(64, 64, 5, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP)},
    };
    refused[3].description.rfu0 = 1;
    for (size_t index = 0; index < sizeof refused / sizeof *refused; index++) {
        AHardwareBuffer *buffer = NULL;
        const int made = AHardwareBuffer_allocate(&refused[index].description, &buffer);
        printf("%s: isSupported %d, allocate %d%s\n", refused[index].what,
               AHardwareBuffer_isSupported(&refused[index].description), made,
               made == 0 ? "" : (buffer == NULL ? ", no buffer" : ", a buffer"));
        if (made == 0) AHardwareBuffer_release(buffer);
    }
    AHardwareBuffer *none = NULL;
    printf("isSupported NULL: %d, allocate NULL: %d\n", AHardwareBuffer_isSupported(NULL),
           AHardwareBuffer_allocate(NULL, &none));
}

static void one_buffer(void) {
    AHardwareBuffer_Desc asked = desc(64, 48, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                      CPU | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE);
    AHardwareBuffer *buffer = NULL, *other = NULL;
    printf("allocate: %d\n", AHardwareBuffer_allocate(&asked, &buffer));
    if (buffer == NULL) return;
    AHardwareBuffer_Desc got;
    memset(&got, 0xff, sizeof got);
    AHardwareBuffer_describe(buffer, &got);
    printf("describe: %u x %u x %u, format %u, usage %#llx, stride at least the width %d, rfu %u %llu\n", got.width,
           got.height, got.layers, got.format, (unsigned long long)got.usage, got.stride >= got.width, got.rfu0,
           (unsigned long long)got.rfu1);
    uint64_t id = 0, other_id = 0;
    AHardwareBuffer_allocate(&asked, &other);
    printf("getId: %d, not 0 %d, another buffer's differs %d\n", AHardwareBuffer_getId(buffer, &id), id != 0,
           other != NULL && AHardwareBuffer_getId(other, &other_id) == 0 && other_id != id);
    printf("getId with no out: %d\n", AHardwareBuffer_getId(buffer, NULL));
    if (other != NULL) AHardwareBuffer_release(other);

    void *address = NULL, *rect_address = NULL;
    int32_t bytes_per_pixel = 0, bytes_per_stride = 0;
    printf("lockAndGetInfo: %d", AHardwareBuffer_lockAndGetInfo(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL,
                                                                &address, &bytes_per_pixel, &bytes_per_stride));
    printf(", %d bytes a pixel, bytes a stride the stride's %d\n", bytes_per_pixel,
           bytes_per_stride == (int32_t)got.stride * 4);
    printf("unlock: %d\n", AHardwareBuffer_unlock(buffer, NULL));
    printf("unlock without a lock: %d\n", AHardwareBuffer_unlock(buffer, NULL));
    const ARect inside = {10, 10, 20, 20}, outside = {0, 0, 65, 48};
    printf("lock with a rectangle: %d", AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, &inside,
                                                             &rect_address));
    printf(", the buffer's address %d\n", rect_address == address);
    AHardwareBuffer_unlock(buffer, NULL);
    void *refused = NULL;
    printf("lock outside the buffer: %d\n",
           AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, &outside, &refused));
    printf("lock for the GPU: %d\n",
           AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, -1, NULL, &refused));
    AHardwareBuffer_Planes planes;
    memset(&planes, 0, sizeof planes);
    printf("lockPlanes: %d",
           AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &planes));
    printf(", %u plane, pixel stride %u, row stride the stride's %d, the buffer's address %d\n", planes.planeCount,
           planes.planes[0].pixelStride, planes.planes[0].rowStride == got.stride * 4,
           planes.planes[0].data == address);
    AHardwareBuffer_unlock(buffer, NULL);
    AHardwareBuffer_acquire(buffer);
    AHardwareBuffer_release(buffer);
    printf("after acquire and release: describe %u x %u\n", (AHardwareBuffer_describe(buffer, &got), got.width),
           got.height);
    AHardwareBuffer_release(buffer);
}

static void formats(void) {
    for (size_t index = 0; index < sizeof FORMATS / sizeof *FORMATS; index++) {
        AHardwareBuffer_Desc asked = desc(33, 7, 1, FORMATS[index].code, CPU);
        AHardwareBuffer *buffer = NULL;
        const int made = AHardwareBuffer_allocate(&asked, &buffer);
        if (made != 0) {
            printf("%s: allocate %d\n", FORMATS[index].name, made);
            continue;
        }
        AHardwareBuffer_Desc got;
        AHardwareBuffer_describe(buffer, &got);
        void *address = NULL;
        int32_t bytes_per_pixel = 0, bytes_per_stride = 0;
        AHardwareBuffer_lockAndGetInfo(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &address,
                                       &bytes_per_pixel, &bytes_per_stride);
        AHardwareBuffer_unlock(buffer, NULL);
        printf("%s: %d bytes a pixel, the pattern back %d\n", FORMATS[index].name, bytes_per_pixel,
               round_trip(buffer, FORMATS[index].code, 7, 33 * FORMATS[index].bytes,
                          got.stride * FORMATS[index].bytes));
        AHardwareBuffer_release(buffer);
    }
}

static void blob(void) {
    AHardwareBuffer_Desc asked = desc(100000, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB, CPU);
    AHardwareBuffer *buffer = NULL;
    printf("BLOB of 100000: allocate %d", AHardwareBuffer_allocate(&asked, &buffer));
    if (buffer != NULL) {
        AHardwareBuffer_Desc got;
        AHardwareBuffer_describe(buffer, &got);
        void *address = NULL;
        int32_t bytes_per_pixel = 0, bytes_per_stride = 0;
        const int info = AHardwareBuffer_lockAndGetInfo(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL,
                                                        &address, &bytes_per_pixel, &bytes_per_stride);
        AHardwareBuffer_unlock(buffer, NULL);
        printf(", stride %u, lockAndGetInfo %d with %d and %d, the pattern back %d", got.stride, info, bytes_per_pixel,
               bytes_per_stride, round_trip(buffer, AHARDWAREBUFFER_FORMAT_BLOB, 1, 100000, 100000));
        AHardwareBuffer_release(buffer);
    }
    printf("\n");
}

static void socket_pair(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        printf("socketpair failed\n");
        return;
    }
    AHardwareBuffer_Desc asked = desc(16, 16, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, CPU);
    AHardwareBuffer *sent = NULL, *received = NULL;
    AHardwareBuffer_allocate(&asked, &sent);
    printf("send: %d\n", AHardwareBuffer_sendHandleToUnixSocket(sent, pair[0]));
    printf("receive: %d\n", AHardwareBuffer_recvHandleFromUnixSocket(pair[1], &received));
    uint64_t sent_id = 0, received_id = 0;
    AHardwareBuffer_Desc got;
    memset(&got, 0, sizeof got);
    if (received != NULL) {
        AHardwareBuffer_getId(received, &received_id);
        AHardwareBuffer_describe(received, &got);
    }
    AHardwareBuffer_getId(sent, &sent_id);
    printf("received: the same ID %d, %u x %u\n", received != NULL && sent_id == received_id, got.width, got.height);
    AHardwareBuffer_release(sent);
    printf("the received one after the sent one's release: the pattern back %d\n",
           received != NULL && round_trip(received, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 16, 64, got.stride * 4));
    if (received != NULL) AHardwareBuffer_release(received);
    close(pair[0]);
    close(pair[1]);
}

/* The planes' pattern: Y's samples, then Cb's and Cr's, written under one lockPlanes and read back under another,
   at the planes' strides: 1 when every sample came back. */
static int planes_round_trip(AHardwareBuffer *buffer, uint32_t width, uint32_t height) {
    for (int pass = 0; pass < 2; pass++) {
        AHardwareBuffer_Planes planes;
        memset(&planes, 0, sizeof planes);
        const uint64_t usage = pass == 0 ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN : AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        if (AHardwareBuffer_lockPlanes(buffer, usage, -1, NULL, &planes) != 0 || planes.planeCount != 3) return 0;
        int same = 1;
        for (uint32_t plane = 0; plane < 3; plane++) {
            const uint32_t rows = plane == 0 ? height : height / 2, columns = plane == 0 ? width : width / 2;
            uint8_t *data = planes.planes[plane].data;
            const size_t row_stride = planes.planes[plane].rowStride, pixel_stride = planes.planes[plane].pixelStride;
            for (uint32_t row = 0; row < rows; row++) {
                for (uint32_t column = 0; column < columns; column++) {
                    uint8_t *sample = data + row * row_stride + column * pixel_stride;
                    if (pass == 0) {
                        *sample = pattern(plane + 1, row, column);
                    } else if (*sample != pattern(plane + 1, row, column)) {
                        same = 0;
                    }
                }
            }
        }
        if (AHardwareBuffer_unlock(buffer, NULL) != 0 || !same) return 0;
    }
    return 1;
}

/* ADR-0060 e2a: the YUV formats. A plane's strides and the order of Cb and Cr are gralloc's, so only what Android
   promises is printed: three planes, Y's pixel stride 1, Cb's and Cr's alike, 1 or 2, and interleaved at 2. */
static void yuv(void) {
    const struct format yuvs[] = {
        {"Y8Cb8Cr8_420", AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, 1},
        {"YCbCr_P010", AHARDWAREBUFFER_FORMAT_YCbCr_P010, 2},
    };
    for (size_t index = 0; index < sizeof yuvs / sizeof *yuvs; index++) {
        AHardwareBuffer_Desc cpu = desc(64, 48, 1, yuvs[index].code, CPU),
                             sampled = desc(64, 48, 1, yuvs[index].code, CPU | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE),
                             gpu = desc(64, 48, 1, yuvs[index].code, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE),
                             target = desc(64, 48, 1, yuvs[index].code, CPU | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER),
                             encode = desc(64, 48, 1, yuvs[index].code, CPU | AHARDWAREBUFFER_USAGE_VIDEO_ENCODE);
        printf("isSupported %s: CPU %d, CPU and GPU sampled %d, GPU sampled %d, CPU and framebuffer %d, "
               "CPU and video encode %d\n",
               yuvs[index].name, AHardwareBuffer_isSupported(&cpu), AHardwareBuffer_isSupported(&sampled),
               AHardwareBuffer_isSupported(&gpu), AHardwareBuffer_isSupported(&target),
               AHardwareBuffer_isSupported(&encode));
    }
    struct {
        const char *what;
        AHardwareBuffer_Desc description;
    } shapes[] = {
        {"Y8Cb8Cr8_420 63 x 47", desc(63, 47, 1, AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, CPU)},
        {"Y8Cb8Cr8_420 of 2 layers", desc(64, 48, 2, AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, CPU)},
        {"Y8Cb8Cr8_420 mipmapped",
         desc(64, 48, 1, AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, CPU | AHARDWAREBUFFER_USAGE_GPU_MIPMAP_COMPLETE)},
    };
    for (size_t index = 0; index < sizeof shapes / sizeof *shapes; index++) {
        AHardwareBuffer *buffer = NULL;
        const int made = AHardwareBuffer_allocate(&shapes[index].description, &buffer);
        printf("%s: isSupported %d, allocate %d\n", shapes[index].what,
               AHardwareBuffer_isSupported(&shapes[index].description), made);
        if (made == 0) AHardwareBuffer_release(buffer);
    }

    AHardwareBuffer_Desc asked = desc(64, 48, 1, AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, CPU);
    AHardwareBuffer *buffer = NULL;
    printf("Y8Cb8Cr8_420 64 x 48: allocate %d\n", AHardwareBuffer_allocate(&asked, &buffer));
    if (buffer == NULL) return;
    AHardwareBuffer_Desc got;
    AHardwareBuffer_describe(buffer, &got);
    printf("describe: %u x %u x %u, format %#x, stride at least the width %d\n", got.width, got.height, got.layers,
           got.format, got.stride >= got.width);
    void *address = NULL;
    const int locked = AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &address);
    printf("lock: %d\n", locked);
    if (locked == 0) AHardwareBuffer_unlock(buffer, NULL);
    void *info_address = NULL;
    int32_t bytes_per_pixel = 0, bytes_per_stride = 0;
    const int info = AHardwareBuffer_lockAndGetInfo(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL,
                                                    &info_address, &bytes_per_pixel, &bytes_per_stride);
    printf("lockAndGetInfo: %d\n", info);
    if (info == 0) AHardwareBuffer_unlock(buffer, NULL);
    AHardwareBuffer_Planes planes;
    memset(&planes, 0, sizeof planes);
    const int planar = AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &planes);
    printf("lockPlanes: %d, %u planes", planar, planes.planeCount);
    if (planar == 0 && planes.planeCount == 3) {
        const AHardwareBuffer_Plane *y = &planes.planes[0], *cb = &planes.planes[1], *cr = &planes.planes[2];
        const ptrdiff_t apart = (const uint8_t *)cr->data - (const uint8_t *)cb->data;
        printf(", Y's pixel stride %u, Y's rows at least the width %d; Cb's and Cr's: the same pixel stride %d, 1 or 2 "
               "%d, the same row stride %d, rows at least half the width %d, interleaved at 2 %d, after Y %d",
               y->pixelStride, y->rowStride >= 64, cb->pixelStride == cr->pixelStride,
               cb->pixelStride == 1 || cb->pixelStride == 2, cb->rowStride == cr->rowStride,
               cb->rowStride >= 32 * cb->pixelStride, cb->pixelStride != 2 || apart == 1 || apart == -1,
               (const uint8_t *)cb->data >= (const uint8_t *)y->data + (size_t)y->rowStride * 48 &&
                   (const uint8_t *)cr->data >= (const uint8_t *)y->data + (size_t)y->rowStride * 48);
    }
    printf("\n");
    if (planar == 0) AHardwareBuffer_unlock(buffer, NULL);
    printf("Y8Cb8Cr8_420: the planes' pattern back %d\n", planes_round_trip(buffer, 64, 48));
    AHardwareBuffer_release(buffer);

    // P010: 16 bits a sample, the 10 bits at the top, Cb and Cr interleaved (hardware_buffer.h).
    AHardwareBuffer_Desc ten = desc(64, 48, 1, AHARDWAREBUFFER_FORMAT_YCbCr_P010, CPU);
    AHardwareBuffer *deep = NULL;
    printf("YCbCr_P010 64 x 48: allocate %d\n", AHardwareBuffer_allocate(&ten, &deep));
    if (deep == NULL) return;
    memset(&planes, 0, sizeof planes);
    const int deep_planar = AHardwareBuffer_lockPlanes(deep, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &planes);
    printf("lockPlanes: %d, %u planes", deep_planar, planes.planeCount);
    if (deep_planar == 0 && planes.planeCount == 3) {
        const AHardwareBuffer_Plane *y = &planes.planes[0], *cb = &planes.planes[1], *cr = &planes.planes[2];
        const ptrdiff_t apart = (const uint8_t *)cr->data - (const uint8_t *)cb->data;
        printf(", pixel strides %u %u %u, Y's rows at least the width %d, the same chroma rows %d, Cb and Cr 2 apart "
               "%d",
               y->pixelStride, cb->pixelStride, cr->pixelStride, y->rowStride >= 128, cb->rowStride == cr->rowStride,
               apart == 2 || apart == -2);
    }
    printf("\n");
    if (deep_planar == 0) AHardwareBuffer_unlock(deep, NULL);
    AHardwareBuffer_release(deep);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    supported_and_refused();
    one_buffer();
    formats();
    blob();
    socket_pair();
    yuv();
    return 0;
}
