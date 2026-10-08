/* graphics.hardware_buffers (ADR-0058 a)): graphics/host's hardware buffers (hardware_buffer.m), the IOSurfaces of
   the guest's AHardwareBuffers, through the functions minato_graphics_set_hardware_buffers gives the runtime, without
   the runtime: the descriptions they support, the stride of their aligned rows, a pattern written under one lock and
   read back under another for each format of a), read-only and nested locks by IOSurface's seed, layers and BLOBs,
   the size cap, the free, and many threads at once. Exits 1 after the cases when one failed. */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#include <mach/kern_return.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"

// AHardwareBuffer's formats and usages (the NDK's hardware_buffer.h).
enum {
    RGBA8 = 1,
    RGBX8 = 2,
    RGB8 = 3,
    RGB565 = 4,
    RGBA16F = 0x16,
    BLOB = 0x21,
    YUV420 = 0x23,
    RGB10A2 = 0x2b,
    D16 = 0x30,
    D24S8 = 0x32,
    S8 = 0x35,
    P010 = 0x36,
    R8 = 0x38,
    R16_UINT = 0x39,
    BGR10A2 = 0x43,
};
static const uint64_t CPU_READ_RARELY = 2, CPU_READ_OFTEN = 3, CPU_WRITE_RARELY = UINT64_C(2) << 4,
                      CPU_WRITE_OFTEN = UINT64_C(3) << 4, GPU_SAMPLED_IMAGE = UINT64_C(1) << 8,
                      GPU_FRAMEBUFFER = UINT64_C(1) << 9, COMPOSER_OVERLAY = UINT64_C(1) << 11,
                      PROTECTED_CONTENT = UINT64_C(1) << 14, VIDEO_ENCODE = UINT64_C(1) << 16,
                      SENSOR_DIRECT_DATA = UINT64_C(1) << 23, GPU_DATA_BUFFER = UINT64_C(1) << 24,
                      GPU_CUBE_MAP = UINT64_C(1) << 25, GPU_MIPMAP_COMPLETE = UINT64_C(1) << 26,
                      VENDOR_0 = UINT64_C(1) << 28, FRONT_BUFFER = UINT64_C(1) << 32, VENDOR_19 = UINT64_C(1) << 63;
static const uint64_t CPU = CPU_READ_OFTEN | CPU_WRITE_OFTEN;

// Linux's errno values, which the guest gets.
enum { linux_enomem = 12, linux_einval = 22 };

/// a)'s formats but BLOB, with their bytes per pixel and the Metal format b) wraps them as, whose linear alignment the
/// rows keep.
struct format {
    const char *name;
    uint32_t code;
    uint32_t bytes;
    MTLPixelFormat metal;
};
static const struct format FORMATS[] = {
    {"R8G8B8A8_UNORM", RGBA8, 4, MTLPixelFormatRGBA8Unorm},
    {"R8G8B8X8_UNORM", RGBX8, 4, MTLPixelFormatRGBA8Unorm},
    {"R5G6B5_UNORM", RGB565, 2, MTLPixelFormatB5G6R5Unorm},
    {"R16G16B16A16_FLOAT", RGBA16F, 8, MTLPixelFormatRGBA16Float},
    {"R10G10B10A2_UNORM", RGB10A2, 4, MTLPixelFormatRGB10A2Unorm},
    {"R8_UNORM", R8, 1, MTLPixelFormatR8Unorm},
};
static const size_t FORMAT_COUNT = sizeof FORMATS / sizeof *FORMATS;

static const struct apkrt_hardware_buffers *table;
static id<MTLDevice> device;
static atomic_int failures;
static atomic_int thread_failures;

static void fail(const char *test, NSString *what) {
    fprintf(stderr, "FAILED  %s: %s\n", test, what.UTF8String);
    atomic_fetch_add(&failures, 1);
}

static struct apkrt_hardware_buffer_description desc(uint32_t width, uint32_t height, uint32_t layers,
                                                     uint32_t format, uint64_t usage) {
    const struct apkrt_hardware_buffer_description description = {
        .width = width, .height = height, .layers = layers, .format = format, .usage = usage};
    return description;
}

static int supported(struct apkrt_hardware_buffer_description description) {
    return table->supported(table->context, &description);
}

static int allocate(struct apkrt_hardware_buffer_description description, uint64_t *handle, uint32_t *stride) {
    return table->allocate(table->context, &description, handle, stride);
}

struct lock {
    int result;
    uint8_t *address;
    uint32_t bytes_per_pixel, bytes_per_row;
};

static struct lock lock(uint64_t handle, uint64_t usage) {
    struct lock answer = {0};
    void *address = NULL;
    answer.result =
        table->lock(table->context, handle, usage, &address, &answer.bytes_per_pixel, &answer.bytes_per_row);
    answer.address = address;
    return answer;
}

static int unlock(uint64_t handle) {
    return table->unlock(table->context, handle);
}

static void release(uint64_t handle) {
    table->free(table->context, handle);
}

/// The rows as the lead's rule has them: IOSurface's alignment of width x bytes per pixel, rounded up to Metal's
/// minimum linear texture alignment for the format.
static size_t aligned_row(const struct format *format, uint32_t width) {
    const size_t alignment = (size_t)[device minimumLinearTextureAlignmentForPixelFormat:format->metal];
    const size_t row = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)width * format->bytes);
    return (row + alignment - 1) / alignment * alignment;
}

static uint8_t pattern(uint32_t format, size_t row, size_t column) {
    return (uint8_t)(format * 31 + row * 7 + column * 13);
}

static void fill(uint8_t *address, size_t bytes_per_row, size_t rows, size_t row_bytes, uint32_t format) {
    for (size_t row = 0; row < rows; row++) {
        for (size_t column = 0; column < row_bytes; column++) {
            address[row * bytes_per_row + column] = pattern(format, row, column);
        }
    }
}

/// The first byte that differs from the pattern as "row r, byte b", or nil.
static NSString *differs(const uint8_t *address, size_t bytes_per_row, size_t rows, size_t row_bytes, uint32_t format) {
    for (size_t row = 0; row < rows; row++) {
        for (size_t column = 0; column < row_bytes; column++) {
            if (address[row * bytes_per_row + column] != pattern(format, row, column)) {
                return [NSString stringWithFormat:@"row %zu, byte %zu", row, column];
            }
        }
    }
    return nil;
}

static void eachFormatOfAIsSupportedForTheCPUAndTheGPU(void) {
    const char *const test = "each format of a) is supported for the CPU and the GPU";
    const uint64_t usages[] = {CPU,
                               CPU_READ_RARELY,
                               CPU_WRITE_RARELY,
                               GPU_SAMPLED_IMAGE,
                               GPU_SAMPLED_IMAGE | GPU_FRAMEBUFFER | CPU_WRITE_OFTEN,
                               GPU_SAMPLED_IMAGE | COMPOSER_OVERLAY};
    for (size_t index = 0; index < FORMAT_COUNT; index++) {
        for (size_t usage = 0; usage < sizeof usages / sizeof *usages; usage++) {
            const int answer = supported(desc(64, 48, 1, FORMATS[index].code, usages[usage]));
            if (answer != 1) {
                fail(test, [NSString stringWithFormat:@"%s with usage %#llx: %d", FORMATS[index].name,
                                                      (unsigned long long)usages[usage], answer]);
            }
        }
    }
    const struct apkrt_hardware_buffer_description more[] = {
        desc(4096, 1, 1, BLOB, CPU), desc(1, 1, 1, BLOB, CPU_READ_RARELY), desc(64, 48, 4, RGBA8, CPU)};
    for (size_t index = 0; index < sizeof more / sizeof *more; index++) {
        if (supported(more[index]) != 1) {
            fail(test, [NSString stringWithFormat:@"format %#x, %u x %u x %u: not supported", more[index].format,
                                                  more[index].width, more[index].height, more[index].layers]);
        }
    }
}

static void otherFormatsAndUsagesAreNot(void) {
    const char *const test = "other formats and usages are not supported";
    const uint32_t formats[] = {RGB8, D16, D24S8, S8, YUV420, P010, R16_UINT, BGR10A2, 0x99, 0};
    struct apkrt_hardware_buffer_description refused[32];
    size_t count = 0;
    for (size_t index = 0; index < sizeof formats / sizeof *formats; index++) {
        refused[count++] = desc(64, 48, 1, formats[index], CPU_READ_OFTEN);
    }
    const uint64_t usages[] = {PROTECTED_CONTENT | GPU_SAMPLED_IMAGE,
                               VIDEO_ENCODE,
                               SENSOR_DIRECT_DATA,
                               GPU_DATA_BUFFER,
                               GPU_CUBE_MAP | GPU_SAMPLED_IMAGE,
                               GPU_MIPMAP_COMPLETE | GPU_SAMPLED_IMAGE,
                               FRONT_BUFFER | GPU_FRAMEBUFFER,
                               VENDOR_0 | CPU_READ_OFTEN,
                               VENDOR_19 | CPU_READ_OFTEN,
                               (UINT64_C(1) << 10) | CPU_READ_OFTEN,
                               (UINT64_C(1) << 12) | CPU_READ_OFTEN};
    for (size_t index = 0; index < sizeof usages / sizeof *usages; index++) {
        refused[count++] = desc(64, 48, 1, RGBA8, usages[index]);
    }
    refused[count++] = desc(4096, 1, 1, BLOB, CPU | GPU_SAMPLED_IMAGE);
    refused[count++] = desc(4096, 2, 1, BLOB, CPU);
    refused[count++] = desc(4096, 1, 2, BLOB, CPU);
    refused[count++] = desc(64, 48, 2, RGBA8, CPU | GPU_SAMPLED_IMAGE);
    for (size_t index = 0; index < count; index++) {
        uint64_t handle = 0;
        uint32_t stride = 0;
        const int answer = supported(refused[index]);
        const int made = allocate(refused[index], &handle, &stride);
        if (answer != 0 || made != -linux_einval || handle != 0) {
            fail(test, [NSString stringWithFormat:@"format %#x, usage %#llx, %u x %u x %u: supported %d, allocate %d",
                                                  refused[index].format, (unsigned long long)refused[index].usage,
                                                  refused[index].width, refused[index].height, refused[index].layers,
                                                  answer, made]);
            if (made == 0) {
                release(handle);
            }
        }
    }
}

static void rowsAlignAsIOSurfaceAndMetalAskAndTheStrideCountsThem(void) {
    const char *const test = "rows align as IOSurface and Metal ask, and the stride counts them";
    const uint32_t widths[] = {1, 1290, 4097};
    for (size_t index = 0; index < FORMAT_COUNT; index++) {
        const struct format *format = &FORMATS[index];
        for (size_t width = 0; width < sizeof widths / sizeof *widths; width++) {
            uint64_t handle = 0;
            uint32_t stride = 0;
            const int made = allocate(desc(widths[width], 3, 1, format->code, CPU), &handle, &stride);
            if (made != 0 || handle == 0) {
                fail(test, [NSString stringWithFormat:@"%s, width %u: allocate %d, handle %llu", format->name,
                                                      widths[width], made, (unsigned long long)handle]);
                continue;
            }
            const struct lock locked = lock(handle, CPU_READ_OFTEN);
            const size_t row = aligned_row(format, widths[width]);
            if (locked.result != 0 || locked.address == NULL || locked.bytes_per_pixel != format->bytes ||
                locked.bytes_per_row != row || (size_t)stride * format->bytes != row || stride < widths[width]) {
                fail(test, [NSString stringWithFormat:@"%s, width %u: lock %d, %u bytes a pixel, %u a row (want %zu),"
                                                      " stride %u",
                                                      format->name, widths[width], locked.result,
                                                      locked.bytes_per_pixel, locked.bytes_per_row, row, stride]);
            }
            if (locked.result == 0 && unlock(handle) != 0) {
                fail(test, [NSString stringWithFormat:@"%s, width %u: unlock failed", format->name, widths[width]]);
            }
            release(handle);
        }
    }
}

static void aPatternWrittenUnderOneLockReadsBackUnderAnother(void) {
    const char *const test = "a pattern written under one lock reads back under another";
    for (size_t index = 0; index <= FORMAT_COUNT; index++) {
        const BOOL blob = index == FORMAT_COUNT;
        const uint32_t code = blob ? (uint32_t)BLOB : FORMATS[index].code;
        const uint32_t width = blob ? 5000u : 77u, height = blob ? 1u : 5u, bytes = blob ? 1u : FORMATS[index].bytes;
        uint64_t handle = 0;
        uint32_t stride = 0;
        if (allocate(desc(width, height, 1, code, CPU), &handle, &stride) != 0) {
            fail(test, [NSString stringWithFormat:@"format %#x: allocate failed", code]);
            continue;
        }
        const struct lock written = lock(handle, CPU_WRITE_OFTEN);
        if (written.result != 0 || written.address == NULL) {
            fail(test, [NSString stringWithFormat:@"format %#x: the write lock answered %d", code, written.result]);
            release(handle);
            continue;
        }
        fill(written.address, written.bytes_per_row, height, (size_t)width * bytes, code);
        unlock(handle);
        const struct lock read = lock(handle, CPU_READ_OFTEN);
        NSString *where = read.result == 0 && read.address != NULL
                              ? differs(read.address, read.bytes_per_row, height, (size_t)width * bytes, code)
                              : @"no read lock";
        if (where != nil) {
            fail(test, [NSString stringWithFormat:@"format %#x: %@", code, where]);
        }
        if (read.result == 0) {
            unlock(handle);
        }
        release(handle);
    }
}

static void aReadOnlyLockKeepsTheSeedAndAWriteLockMovesIt(void) {
    const char *const test = "a read-only lock keeps IOSurface's seed and a write lock moves it";
    uint64_t handle = 0;
    uint32_t stride = 0;
    if (allocate(desc(16, 16, 1, RGBA8, CPU), &handle, &stride) != 0) {
        fail(test, @"allocate failed");
        return;
    }
    IOSurfaceRef surface = (IOSurfaceRef)minato_graphics_hardware_buffer_surface(handle);
    if (surface == NULL) {
        fail(test, @"no surface for the handle");
        release(handle);
        return;
    }
    const uint32_t before = IOSurfaceGetSeed(surface);
    const int read = lock(handle, CPU_READ_RARELY).result;
    unlock(handle);
    const uint32_t after_read = IOSurfaceGetSeed(surface);
    const int written = lock(handle, CPU_WRITE_RARELY).result;
    unlock(handle);
    const uint32_t after_write = IOSurfaceGetSeed(surface);
    if (read != 0 || written != 0 || after_read != before || after_write == after_read) {
        fail(test, [NSString stringWithFormat:@"locks %d and %d, seeds %u, %u, %u", read, written, before, after_read,
                                              after_write]);
    }
    CFRelease(surface);
    release(handle);
}

/// Whether IOSurface holds no lock of the surface's: an unlock of either kind fails.
static BOOL unlocked(IOSurfaceRef surface) {
    return IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL) != KERN_SUCCESS &&
           IOSurfaceUnlock(surface, 0, NULL) != KERN_SUCCESS;
}

/// Whether IOSurface holds a read-write lock of the surface's, which an unlock and a new lock of that kind show and
/// put back.
static BOOL write_locked(IOSurfaceRef surface) {
    if (IOSurfaceUnlock(surface, 0, NULL) != KERN_SUCCESS) {
        return NO;
    }
    IOSurfaceLock(surface, 0, NULL);
    return YES;
}

static void locksNestAndTheLastUnlockEndsThem(void) {
    const char *const test = "locks nest and the last unlock ends them";
    uint64_t handle = 0;
    uint32_t stride = 0;
    if (allocate(desc(16, 16, 1, RGBA8, CPU), &handle, &stride) != 0) {
        fail(test, @"allocate failed");
        return;
    }
    IOSurfaceRef surface = (IOSurfaceRef)minato_graphics_hardware_buffer_surface(handle);
    if (surface == NULL) {
        fail(test, @"no surface for the handle");
        release(handle);
        return;
    }
    // A read lock, then a write lock inside it: one address, a read-write lock of IOSurface's from the write on (its
    // seed moves as it is taken), held until the last unlock.
    const struct lock reading = lock(handle, CPU_READ_OFTEN);
    const uint32_t seed = IOSurfaceGetSeed(surface);
    const struct lock writing = lock(handle, CPU_WRITE_OFTEN);
    const uint32_t written = IOSurfaceGetSeed(surface);
    if (reading.result != 0 || writing.result != 0 || reading.address != writing.address || written == seed) {
        fail(test, [NSString stringWithFormat:@"read then write: %d and %d, addresses %p and %p, seeds %u and %u",
                                              reading.result, writing.result, (void *)reading.address,
                                              (void *)writing.address, seed, written]);
    }
    if (writing.address != NULL) {
        writing.address[0] = 0x5a;
    }
    const int first = unlock(handle);
    const BOOL held = write_locked(surface);
    const int last = unlock(handle);
    if (first != 0 || !held || last != 0 || !unlocked(surface)) {
        fail(test, [NSString stringWithFormat:@"read then write: unlocks %d and %d, %s after the first", first, last,
                                              held ? "held" : "not held"]);
    }
    const int extra = unlock(handle);
    if (extra != -linux_einval) {
        fail(test, [NSString stringWithFormat:@"an unlock without a lock answered %d", extra]);
    }
    // A write lock, then a read lock inside it.
    const int outer = lock(handle, CPU_WRITE_RARELY).result, inner = lock(handle, CPU_READ_RARELY).result;
    const int inner_end = unlock(handle);
    const BOOL outer_held = write_locked(surface);
    const int outer_end = unlock(handle);
    if (outer != 0 || inner != 0 || inner_end != 0 || !outer_held || outer_end != 0 || !unlocked(surface)) {
        fail(test, [NSString stringWithFormat:@"write then read: %d, %d, %d (%s), %d", outer, inner, inner_end,
                                              outer_held ? "held" : "not held", outer_end]);
    }
    const struct lock again = lock(handle, CPU_READ_OFTEN);
    if (again.result != 0 || again.address == NULL || again.address[0] != 0x5a) {
        fail(test, @"the byte written in the nested lock is gone");
    }
    if (again.result == 0) {
        unlock(handle);
    }
    CFRelease(surface);
    release(handle);
}

static void layersLieOneBelowTheOther(void) {
    const char *const test = "layers lie one below the other";
    uint64_t handle = 0;
    uint32_t stride = 0;
    // Rows of 4096 bytes, so that a surface without the layers' rows is smaller than the pattern by pages.
    const uint32_t width = 1024, height = 8, layers = 3;
    if (allocate(desc(width, height, layers, RGBA8, CPU), &handle, &stride) != 0) {
        fail(test, @"allocate failed");
        return;
    }
    const struct lock written = lock(handle, CPU_WRITE_OFTEN);
    if (written.result != 0 || written.address == NULL) {
        fail(test, @"no write lock");
        release(handle);
        return;
    }
    fill(written.address, written.bytes_per_row, height * layers, width * 4, RGBA8);
    unlock(handle);
    const struct lock read = lock(handle, CPU_READ_OFTEN);
    NSString *where = read.result == 0 ? differs(read.address, read.bytes_per_row, height * layers, width * 4, RGBA8)
                                       : @"no read lock";
    IOSurfaceRef surface = (IOSurfaceRef)minato_graphics_hardware_buffer_surface(handle);
    const size_t size = surface != NULL ? IOSurfaceGetAllocSize(surface) : 0;
    if (where != nil || size < (size_t)read.bytes_per_row * height * layers) {
        fail(test, [NSString stringWithFormat:@"%@, %zu bytes for %u rows of %u", where != nil ? where : @"no mismatch",
                                              size, height * layers, read.bytes_per_row]);
    }
    if (surface != NULL) {
        CFRelease(surface);
    }
    if (read.result == 0) {
        unlock(handle);
    }
    release(handle);
}

static void aBlobIsItsWidthInBytesInOneRange(void) {
    const char *const test = "a BLOB is its width in bytes, in one range";
    // 100000 bytes are more than the 32768 that IOSurface gives as its largest row on the Mac.
    const uint32_t widths[] = {1000, 100000};
    for (size_t index = 0; index < sizeof widths / sizeof *widths; index++) {
        uint64_t handle = 0;
        uint32_t stride = 0;
        const int made = allocate(desc(widths[index], 1, 1, BLOB, CPU), &handle, &stride);
        if (made != 0 || stride != widths[index]) {
            fail(test, [NSString stringWithFormat:@"width %u: allocate %d, stride %u", widths[index], made, stride]);
            if (made == 0) {
                release(handle);
            }
            continue;
        }
        const struct lock written = lock(handle, CPU_WRITE_OFTEN);
        if (written.result != 0 || written.bytes_per_pixel != 1 || written.bytes_per_row != widths[index]) {
            fail(test, [NSString stringWithFormat:@"width %u: lock %d, %u bytes a pixel, %u a row", widths[index],
                                                  written.result, written.bytes_per_pixel, written.bytes_per_row]);
            if (written.result == 0) {
                unlock(handle);
            }
            release(handle);
            continue;
        }
        fill(written.address, widths[index], 1, widths[index], BLOB);
        unlock(handle);
        const struct lock read = lock(handle, CPU_READ_OFTEN);
        NSString *where =
            read.result == 0 ? differs(read.address, widths[index], 1, widths[index], BLOB) : @"no read lock";
        if (where != nil) {
            fail(test, [NSString stringWithFormat:@"width %u: %@", widths[index], where]);
        }
        if (read.result == 0) {
            unlock(handle);
        }
        release(handle);
    }
}

static void sizesAboveTheCapAreRefused(void) {
    const char *const test = "sizes above the cap are refused";
    // 16383 pixels of 4 bytes are 65532 bytes, which align to 65536: 16385 rows stay within the cap unaligned and go
    // above it aligned, which the host checks itself.
    const struct apkrt_hardware_buffer_description large[] = {
        desc(16383, 16385, 1, RGBA8, CPU),          desc(16384, 16385, 1, RGBA8, CPU),
        desc(65536, 65536, 4, RGBA16F, CPU),        desc(UINT32_MAX, UINT32_MAX, UINT32_MAX, RGBA16F, CPU),
        desc(UINT32_MAX, 1, 1, BLOB, CPU),          desc(16384, 16384, 5, R8, CPU),
    };
    for (size_t index = 0; index < sizeof large / sizeof *large; index++) {
        uint64_t handle = 0;
        uint32_t stride = 0;
        const int made = allocate(large[index], &handle, &stride);
        if (made != -linux_enomem || handle != 0) {
            fail(test, [NSString stringWithFormat:@"format %#x, %u x %u x %u: allocate %d", large[index].format,
                                                  large[index].width, large[index].height, large[index].layers, made]);
            if (made == 0) {
                release(handle);
            }
        }
    }
}

static void theFreeLetsGoOfTheSurfaceAndHandlesStayUnique(void) {
    const char *const test = "the free lets go of the surface, and handles stay unique";
    uint64_t first = 0, second = 0;
    uint32_t stride = 0;
    if (allocate(desc(8, 8, 1, RGBA8, CPU), &first, &stride) != 0) {
        fail(test, @"allocate failed");
        return;
    }
    IOSurfaceRef surface = (IOSurfaceRef)minato_graphics_hardware_buffer_surface(first);
    release(first);
    void *gone = minato_graphics_hardware_buffer_surface(first);
    const struct lock locked = lock(first, CPU_READ_OFTEN);
    const long references = surface != NULL ? CFGetRetainCount(surface) : 0;
    if (surface == NULL || references != 1 || gone != NULL || locked.result != -linux_einval) {
        fail(test, [NSString stringWithFormat:@"surface %p with %ld references after the free, then %p, lock %d",
                                              (void *)surface, references, gone, locked.result]);
    }
    if (gone != NULL) {
        CFRelease(gone);
    }
    if (surface != NULL) {
        CFRelease(surface);
    }
    if (allocate(desc(8, 8, 1, RGBA8, CPU), &second, &stride) != 0 || second == first || second == 0) {
        fail(test, [NSString stringWithFormat:@"handles %llu, then %llu", (unsigned long long)first,
                                              (unsigned long long)second]);
    }
    release(second);
}

static void aFreeWhileLockedKeepsTheMemoryAndRefusesNewLocks(void) {
    const char *const test = "a free while locked keeps the memory and refuses new locks";
    uint64_t handle = 0;
    uint32_t stride = 0;
    if (allocate(desc(1024, 64, 1, RGBA8, CPU), &handle, &stride) != 0) {
        fail(test, @"allocate failed");
        return;
    }
    const struct lock open = lock(handle, CPU_WRITE_OFTEN);
    if (open.result != 0) {
        fail(test, @"no lock");
        release(handle);
        return;
    }
    release(handle);
    // The guest still holds the address: its writes must land in memory that stays.
    fill(open.address, open.bytes_per_row, 64, 1024 * 4, RGBA8);
    NSString *where = differs(open.address, open.bytes_per_row, 64, 1024 * 4, RGBA8);
    const int locked = lock(handle, CPU_READ_OFTEN).result;
    void *surface = minato_graphics_hardware_buffer_surface(handle);
    const int ended = unlock(handle);
    void *after = minato_graphics_hardware_buffer_surface(handle);
    if (where != nil || locked != -linux_einval || surface != NULL || ended != 0 || after != NULL) {
        fail(test, [NSString stringWithFormat:@"%@, lock %d, surface %p, unlock %d, then %p",
                                              where != nil ? where : @"memory kept", locked, surface, ended, after]);
    }
    if (surface != NULL) {
        CFRelease(surface);
    }
    if (after != NULL) {
        CFRelease(after);
    }
}

static void locksRacingTheFreeNeverTouchAFreedSurface(void) {
    const char *const test = "locks racing the free never touch a freed surface";
    atomic_store(&thread_failures, 0);
    for (int round = 0; round < 200; round++) {
        uint64_t handle = 0;
        uint32_t stride = 0;
        if (allocate(desc(256, 16, 1, RGBA8, CPU), &handle, &stride) != 0) {
            atomic_fetch_add(&thread_failures, 1);
            continue;
        }
        dispatch_apply(4, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t thread) {
            if (thread == 0) {
                release(handle);
                return;
            }
            const struct lock answer = lock(handle, CPU_WRITE_OFTEN);
            if (answer.result == 0) {
                fill(answer.address, answer.bytes_per_row, 16, 256 * 4, RGBA8);
                if (unlock(handle) != 0) {
                    atomic_fetch_add(&thread_failures, 1);
                }
            } else if (answer.result != -linux_einval) {
                atomic_fetch_add(&thread_failures, 1);
            }
        });
    }
    if (atomic_load(&thread_failures) != 0) {
        fail(test, [NSString stringWithFormat:@"%d answers wrong", atomic_load(&thread_failures)]);
    }
}

static void unknownHandlesAreRefused(void) {
    const char *const test = "unknown handles are refused";
    const uint64_t handles[] = {0, UINT64_MAX, UINT64_C(1) << 40};
    for (size_t index = 0; index < sizeof handles / sizeof *handles; index++) {
        const int locked = lock(handles[index], CPU_READ_OFTEN).result, ended = unlock(handles[index]);
        void *surface = minato_graphics_hardware_buffer_surface(handles[index]);
        release(handles[index]);
        if (locked != -linux_einval || ended != -linux_einval || surface != NULL) {
            fail(test, [NSString stringWithFormat:@"handle %llu: lock %d, unlock %d, surface %p",
                                                  (unsigned long long)handles[index], locked, ended, surface]);
        }
        if (surface != NULL) {
            CFRelease(surface);
        }
    }
}

static void manyThreadsAtOnce(void) {
    const char *const test = "many threads at once";
    uint64_t shared = 0;
    uint32_t stride = 0;
    if (allocate(desc(64, 64, 1, RGBA8, CPU), &shared, &stride) != 0) {
        fail(test, @"allocate failed");
        return;
    }
    const struct lock first = lock(shared, CPU_READ_OFTEN);
    atomic_store(&thread_failures, 0);
    dispatch_apply(8, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t thread) {
        for (int round = 0; round < 100; round++) {
            uint64_t own = 0;
            uint32_t own_stride = 0;
            const uint32_t code = (thread + (size_t)round) % 2 ? (uint32_t)RGB565 : (uint32_t)R8;
            if (allocate(desc(33, 9, 1, code, CPU), &own, &own_stride) != 0) {
                atomic_fetch_add(&thread_failures, 1);
                continue;
            }
            const struct lock written = lock(own, CPU_WRITE_OFTEN);
            const size_t bytes = code == RGB565 ? 2 : 1;
            if (written.result == 0) {
                fill(written.address, written.bytes_per_row, 9, 33 * bytes, code);
            }
            unlock(own);
            const struct lock read = lock(own, CPU_READ_OFTEN);
            if (written.result != 0 || read.result != 0 ||
                differs(read.address, read.bytes_per_row, 9, 33 * bytes, code) != nil) {
                atomic_fetch_add(&thread_failures, 1);
            }
            if (read.result == 0) {
                unlock(own);
            }
            release(own);
            const struct lock again = lock(shared, CPU_READ_RARELY);
            if (again.result != 0 || again.address != first.address) {
                atomic_fetch_add(&thread_failures, 1);
            }
            if (again.result == 0) {
                unlock(shared);
            }
        }
    });
    if (first.result == 0) {
        unlock(shared);
    }
    if (first.result != 0 || atomic_load(&thread_failures) != 0) {
        fail(test, [NSString stringWithFormat:@"first lock %d, %d rounds wrong", first.result,
                                              atomic_load(&thread_failures)]);
    }
    release(shared);
}

int main(void) {
    @autoreleasepool {
        device = MTLCreateSystemDefaultDevice();
        table = minato_graphics_hardware_buffers();
        if (table == NULL || device == nil) {
            fprintf(stderr, "FAILED  no hardware buffers or no Metal device\n");
            return 1;
        }
        eachFormatOfAIsSupportedForTheCPUAndTheGPU();
        otherFormatsAndUsagesAreNot();
        rowsAlignAsIOSurfaceAndMetalAskAndTheStrideCountsThem();
        aPatternWrittenUnderOneLockReadsBackUnderAnother();
        aReadOnlyLockKeepsTheSeedAndAWriteLockMovesIt();
        locksNestAndTheLastUnlockEndsThem();
        layersLieOneBelowTheOther();
        aBlobIsItsWidthInBytesInOneRange();
        sizesAboveTheCapAreRefused();
        theFreeLetsGoOfTheSurfaceAndHandlesStayUnique();
        aFreeWhileLockedKeepsTheMemoryAndRefusesNewLocks();
        locksRacingTheFreeNeverTouchAFreedSurface();
        unknownHandlesAreRefused();
        manyThreadsAtOnce();
        const int failed = atomic_load(&failures);
        printf("graphics.hardware_buffers: 14 cases, %d failures\n", failed);
        return failed == 0 ? 0 : 1;
    }
}
