/* graphics/host's hardware buffers (ADR-0058 a)), which minato_graphics_set_providers gives the runtime
   (apkrt_set_hardware_buffers, whose contract they keep): the memory of the guest's AHardwareBuffers, one IOSurface
   each in Minato's process, which the guest reads and writes at a lock's address without a copy, and which b)'s EGL
   images and e1)'s Vulkan imports wrap. Their functions run as host code on the guest's thread inside its syscall.
   - Formats, with their bytes per pixel: R8G8B8A8 and R8G8B8X8 4, R5G6B5 2, R16G16B16A16_FLOAT 8, R10G10B10A2 4, R8
     1, and BLOB, its width in bytes. The format is only a tag here: b)'s Metal pixel format decides the order of the
     channels and RGBX's alpha. R8G8B8, depth and stencil formats are not supported.
   - YUV (ADR-0060 e2a): Y8Cb8Cr8_420 is one '420v' surface of two planes, Y and interleaved CbCr (NV12), each
     wrapped by Metal (R8 and RG8) and the whole by CoreVideo, for the decoders' copies; YCbCr_P010 the same with 2
     bytes a sample, 'x420' (R16 and RG16). The chroma plane lies right below the luma, (height + 1) / 2 rows of
     (width + 1) / 2 pairs, with the luma's rows, which the guest's lockPlanes counts on: a lock answers the luma's
     address, a sample's bytes and its rows. Only one layer; VIDEO_ENCODE is taken for them, as Android 15 takes it.
   - Usages: the CPU's read and write bits, GPU_SAMPLED_IMAGE, GPU_FRAMEBUFFER and COMPOSER_OVERLAY (and VIDEO_ENCODE
     for YUV), and no other bit
     (protected content, video encode, sensor data, GPU data buffers, cube maps, mipmaps, the front buffer, vendor
     bits). A BLOB is one row and one layer without the GPU's image bits. More than one layer goes without them in
     a), since b) wraps a surface as one 2D texture.
   - Rows: IOSurface's alignment of width x bytes per pixel (IOSurfaceAlignProperty), rounded up to Metal's minimum
     linear texture alignment for the format (for YUV, R8's and RG8's), so that b) can wrap the surface; the stride
     is a row's pixels, and the layers lie one below the other. A BLOB's bytes lie in one range, in rows of at most
     IOSurface's largest row; its stride and its bytes per row are its width.
   - A size above APKRT_HARDWARE_BUFFER_MAX_BYTES as the rows align it, each product checked, answers -ENOMEM, as a
     surface that IOSurface does not make does.
   - Locks nest. The first takes IOSurface's lock read-only unless it asks to write, a write inside read-only locks
     adds a read-write one, and the last unlock lets go of both, so IOSurface's seed moves only for a write.
   - A free refuses every later lock. A buffer freed while locked, which the runtime's contract rules out, keeps its
     surface mapped until its last unlock, so the guest's address never points into freed memory.
   ARC. The handles live under one mutex and each buffer's locks under its own; no IOSurface call holds the first. */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>

#include <pthread.h>
#include <stdio.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"

/* Linux's errno values, which the guest gets. */
enum { linux_enomem = 12, linux_einval = 22 };

/* AHardwareBuffer's formats and usages (the NDK's hardware_buffer.h). */
enum {
    format_rgba8 = 1,
    format_rgbx8 = 2,
    format_rgb565 = 4,
    format_rgba16f = 0x16,
    format_blob = 0x21,
    format_yuv420 = 0x23,
    format_p010 = 0x36,
    format_rgb10a2 = 0x2b,
    format_r8 = 0x38,
};
static const uint64_t usage_cpu_read = 0xf, usage_cpu_write = UINT64_C(0xf) << 4,
                      usage_gpu_images = (UINT64_C(1) << 8) | (UINT64_C(1) << 9),  // sampled image, framebuffer
                      usage_composer_overlay = UINT64_C(1) << 11;
static const uint64_t usage_served = usage_cpu_read | usage_cpu_write | usage_gpu_images | usage_composer_overlay,
                      usage_video_encode = UINT64_C(1) << 16;

/* A format of a)'s: its bytes per pixel, the Metal format b) wraps it as, whose linear alignment its rows keep, and the
   tag its surface carries, CoreVideo's code, which nothing reads for the order of the channels. */
struct format {
    uint32_t code;
    uint32_t bytes;
    MTLPixelFormat metal;
    uint32_t tag;
};

static const struct format formats[] = {
    {format_rgba8, 4, MTLPixelFormatRGBA8Unorm, 'RGBA'},
    {format_rgbx8, 4, MTLPixelFormatRGBA8Unorm, 'RGBA'},
    {format_rgb565, 2, MTLPixelFormatB5G6R5Unorm, 'L565'},
    {format_rgba16f, 8, MTLPixelFormatRGBA16Float, 'RGhA'},
    {format_rgb10a2, 4, MTLPixelFormatRGB10A2Unorm, 'l10r'},
    {format_r8, 1, MTLPixelFormatR8Unorm, 'L008'},
    {format_blob, 1, MTLPixelFormatInvalid, 0},
    {format_yuv420, 1, MTLPixelFormatR8Unorm, '420v'},
    {format_p010, 2, MTLPixelFormatR16Unorm, 'x420'},
};

@interface MinatoHardwareBuffer : NSObject {
@public
    IOSurfaceRef surface;
    uint32_t bytes_per_pixel, bytes_per_row;  // as a lock answers them
    pthread_mutex_t mutex;                    // for the fields below
    uint32_t locks;                           // the guest's, still open
    BOOL read_only, read_write;               // IOSurface's locks held for them
    BOOL freed;                               // no lock after it; the buffer leaves the table at its last unlock
}
@end

@implementation MinatoHardwareBuffer
- (instancetype)init {
    self = [super init];
    if (self != nil) {
        pthread_mutex_init(&mutex, NULL);
    }
    return self;
}

- (void)dealloc {
    pthread_mutex_destroy(&mutex);
    if (surface != NULL) {
        CFRelease(surface);
    }
}
@end

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, MinatoHardwareBuffer *> *buffers;
static uint64_t last_handle;  // handles count up from 1 and are never used again

static BOOL is_yuv(const struct format *format) {
    return format->code == format_yuv420 || format->code == format_p010;
}

/* The Metal format of a YUV format's chroma plane, Cb and Cr in one element. */
static MTLPixelFormat chroma_format(const struct format *format) {
    return format->code == format_p010 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
}

static const struct format *find_format(uint32_t code) {
    for (size_t index = 0; index < sizeof formats / sizeof *formats; index++) {
        if (formats[index].code == code) {
            return &formats[index];
        }
    }
    return NULL;
}

/* The format of a description they can make, or NULL. */
static const struct format *served(const struct apkrt_hardware_buffer_description *description) {
    const struct format *format = find_format(description->format);
    const uint64_t usages = format != NULL && is_yuv(format) ? usage_served | usage_video_encode : usage_served;
    if (format == NULL || description->width == 0 || description->height == 0 || description->layers == 0 ||
        (description->usage & ~usages) != 0) {
        return NULL;
    }
    const BOOL images = (description->usage & usage_gpu_images) != 0;
    if (format->code == format_blob && (description->height != 1 || description->layers != 1 || images)) {
        return NULL;
    }
    if (is_yuv(format) && description->layers != 1) {
        return NULL;
    }
    return (description->layers > 1 && images) ? NULL : format;
}

/* Metal's minimum linear texture alignment for a format, 1 without a device or a texture format. */
static size_t metal_alignment(MTLPixelFormat pixel_format) {
    static dispatch_once_t once;
    static id<MTLDevice> device;
    dispatch_once(&once, ^{
        device = MTLCreateSystemDefaultDevice();
    });
    if (device == nil || pixel_format == MTLPixelFormatInvalid) {
        return 1;
    }
    const NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:pixel_format];
    return alignment > 0 ? (size_t)alignment : 1;
}

/* A surface's geometry for a description: its width and height in elements, its bytes per element and per row, and
   what describe and a lock answer. */
struct layout {
    size_t width, height, element, row;
    size_t chroma_rows;  // YUV's, below the luma's height rows; 0 for other formats
    uint32_t stride, bytes_per_row;
};

/* 0 with the layout, or -ENOMEM for a size above the cap, as aligned, or one whose products overflow. */
static int lay_out(const struct apkrt_hardware_buffer_description *description, const struct format *format,
                   struct layout *layout) {
    size_t total = 0;
    if (format->code == format_blob) {
        // One range of its width in bytes: one row while IOSurface takes it, else rows of IOSurface's largest, one
        // after the other with nothing between them.
        const size_t width = description->width, alignment = IOSurfaceGetPropertyAlignment(kIOSurfaceBytesPerRow);
        size_t largest = IOSurfaceGetPropertyMaximum(kIOSurfaceBytesPerRow);
        largest = alignment > 1 ? largest / alignment * alignment : largest;
        const BOOL one_row = largest == 0 || width <= largest;
        layout->width = one_row ? width : largest;
        layout->height = one_row ? 1 : (width + largest - 1) / largest;
        layout->element = 1;
        layout->row = one_row ? IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width) : largest;
        layout->stride = description->width;
        layout->bytes_per_row = description->width;
        if (layout->row < layout->width || __builtin_mul_overflow(layout->row, layout->height, &total)) {
            return -linux_enomem;
        }
    } else {
        size_t bytes = 0, rows = 0, row = 0;
        // Both are powers of two: the larger keeps whole pixels in a row.
        size_t metal = metal_alignment(format->metal);
        if (is_yuv(format) && metal_alignment(chroma_format(format)) > metal) {
            metal = metal_alignment(chroma_format(format));
        }
        const size_t alignment = metal > format->bytes ? metal : format->bytes;
        if (__builtin_mul_overflow((size_t)description->width, (size_t)format->bytes, &bytes) ||
            __builtin_mul_overflow((size_t)description->height, (size_t)description->layers, &rows)) {
            return -linux_enomem;
        }
        const size_t surface_row = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, bytes);
        if (surface_row < bytes || __builtin_add_overflow(surface_row, alignment - 1, &row)) {
            return -linux_enomem;
        }
        row = row / alignment * alignment;
        const size_t chroma_rows = is_yuv(format) ? ((size_t)description->height + 1) / 2 : 0;
        if (row / format->bytes > UINT32_MAX || row > UINT32_MAX ||
            __builtin_mul_overflow(row, rows + chroma_rows, &total)) {
            return -linux_enomem;
        }
        layout->width = description->width;
        layout->height = rows;
        layout->chroma_rows = chroma_rows;
        layout->element = format->bytes;
        layout->row = row;
        layout->stride = (uint32_t)(row / format->bytes);
        layout->bytes_per_row = (uint32_t)row;
    }
    return total <= APKRT_HARDWARE_BUFFER_MAX_BYTES ? 0 : -linux_enomem;
}

static int supported(void *context, const struct apkrt_hardware_buffer_description *description) {
    (void)context;
    return served(description) != NULL;
}

static int allocate(void *context, const struct apkrt_hardware_buffer_description *description, uint64_t *handle,
                    uint32_t *stride) {
    (void)context;
    const struct format *format = served(description);
    if (format == NULL) {
        return -linux_einval;
    }
    @autoreleasepool {
        struct layout layout = {0};
        const int laid_out = lay_out(description, format, &layout);
        if (laid_out != 0) {
            return laid_out;
        }
        NSMutableDictionary *properties = [NSMutableDictionary dictionaryWithDictionary:@{
            (id)kIOSurfaceWidth: @(layout.width),
            (id)kIOSurfaceHeight: @(layout.height),
            (id)kIOSurfaceBytesPerElement: @(layout.element),
            (id)kIOSurfaceBytesPerRow: @(layout.row),
        }];
        if (format->tag != 0) {
            properties[(id)kIOSurfacePixelFormat] = @(format->tag);
        }
        const size_t luma = layout.row * layout.height, chroma = layout.row * layout.chroma_rows;
        if (layout.chroma_rows != 0) {
            [properties removeObjectsForKeys:@[ (id)kIOSurfaceBytesPerElement, (id)kIOSurfaceBytesPerRow ]];
            properties[(id)kIOSurfaceAllocSize] = @(luma + chroma);
            properties[(id)kIOSurfacePlaneInfo] = @[
                @{
                    (id)kIOSurfacePlaneWidth: @(layout.width),
                    (id)kIOSurfacePlaneHeight: @(layout.height),
                    (id)kIOSurfacePlaneBytesPerElement: @(format->bytes),
                    (id)kIOSurfacePlaneBytesPerRow: @(layout.row),
                    (id)kIOSurfacePlaneOffset: @0,
                    (id)kIOSurfacePlaneSize: @(luma),
                },
                @{
                    (id)kIOSurfacePlaneWidth: @((layout.width + 1) / 2),
                    (id)kIOSurfacePlaneHeight: @(layout.chroma_rows),
                    (id)kIOSurfacePlaneBytesPerElement: @(2 * format->bytes),
                    (id)kIOSurfacePlaneBytesPerRow: @(layout.row),
                    (id)kIOSurfacePlaneOffset: @(luma),
                    (id)kIOSurfacePlaneSize: @(chroma),
                },
            ];
        }
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if (surface == NULL) {
            return -linux_enomem;
        }
        // The geometry must be the one asked for: a BLOB's range, the stride and the guest's rows depend on it, and
        // for YUV the chroma's place below the luma.
        const BOOL planes_as_asked =
            layout.chroma_rows == 0
                ? IOSurfaceGetBytesPerRow(surface) == layout.row && IOSurfaceGetHeight(surface) == layout.height
                : IOSurfaceGetPlaneCount(surface) == 2 && IOSurfaceGetBytesPerRowOfPlane(surface, 0) == layout.row &&
                      IOSurfaceGetBytesPerRowOfPlane(surface, 1) == layout.row &&
                      (uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface, 0) ==
                          (uint8_t *)IOSurfaceGetBaseAddress(surface) &&
                      (uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface, 1) ==
                          (uint8_t *)IOSurfaceGetBaseAddress(surface) + luma;
        if (!planes_as_asked || IOSurfaceGetAllocSize(surface) < luma + chroma) {
            CFRelease(surface);
            return -linux_enomem;
        }
        MinatoHardwareBuffer *buffer = [MinatoHardwareBuffer new];
        buffer->surface = surface;
        buffer->bytes_per_pixel = format->bytes;
        buffer->bytes_per_row = layout.bytes_per_row;
        pthread_mutex_lock(&state_lock);
        if (buffers == nil) {
            buffers = [NSMutableDictionary dictionary];
        }
        const uint64_t made = ++last_handle;
        buffers[@(made)] = buffer;
        pthread_mutex_unlock(&state_lock);
        *handle = made;
        *stride = layout.stride;
        return 0;
    }
}

/* The buffer of a handle, or nil, inside its caller's autorelease pool: a guest thread has none of its own. */
static MinatoHardwareBuffer *find_buffer(uint64_t handle) {
    pthread_mutex_lock(&state_lock);
    MinatoHardwareBuffer *buffer = buffers[@(handle)];
    pthread_mutex_unlock(&state_lock);
    return buffer;
}

/* Takes a freed buffer out of the table, inside its caller's autorelease pool. */
static void forget(uint64_t handle) {
    pthread_mutex_lock(&state_lock);
    [buffers removeObjectForKey:@(handle)];
    pthread_mutex_unlock(&state_lock);
}

/* Lets go of IOSurface's locks held for the buffer's, the read-write one first, with the buffer's mutex held. */
static void let_go(MinatoHardwareBuffer *buffer) {
    if (buffer->read_write) {
        IOSurfaceUnlock(buffer->surface, 0, NULL);
        buffer->read_write = NO;
    }
    if (buffer->read_only) {
        IOSurfaceUnlock(buffer->surface, kIOSurfaceLockReadOnly, NULL);
        buffer->read_only = NO;
    }
}

static int lock_buffer(void *context, uint64_t handle, uint64_t usage, void **address, uint32_t *bytes_per_pixel,
                       uint32_t *bytes_per_row) {
    (void)context;
    @autoreleasepool {
        MinatoHardwareBuffer *buffer = find_buffer(handle);
        if (buffer == nil) {
            return -linux_einval;
        }
        const BOOL write = (usage & usage_cpu_write) != 0;
        int result = 0;
        pthread_mutex_lock(&buffer->mutex);
        if (buffer->freed || buffer->locks == UINT32_MAX) {
            result = -linux_einval;
        } else if (buffer->locks == 0 || (write && !buffer->read_write)) {
            // IOSurface syncs the GPU's writes to the surface here, from b) on.
            const IOSurfaceLockOptions options = write ? (IOSurfaceLockOptions)0 : kIOSurfaceLockReadOnly;
            if (IOSurfaceLock(buffer->surface, options, NULL) != KERN_SUCCESS) {
                result = -linux_einval;
            } else if (write) {
                buffer->read_write = YES;
            } else {
                buffer->read_only = YES;
            }
        }
        if (result == 0) {
            buffer->locks++;
            *address = IOSurfaceGetBaseAddress(buffer->surface);
            *bytes_per_pixel = buffer->bytes_per_pixel;
            *bytes_per_row = buffer->bytes_per_row;
        }
        pthread_mutex_unlock(&buffer->mutex);
        return result;
    }
}

static int unlock_buffer(void *context, uint64_t handle) {
    (void)context;
    @autoreleasepool {
        MinatoHardwareBuffer *buffer = find_buffer(handle);
        if (buffer == nil) {
            return -linux_einval;
        }
        int result = 0;
        BOOL gone = NO;
        pthread_mutex_lock(&buffer->mutex);
        if (buffer->locks == 0) {
            result = -linux_einval;
        } else if (--buffer->locks == 0) {
            let_go(buffer);
            gone = buffer->freed;
        }
        pthread_mutex_unlock(&buffer->mutex);
        if (gone) {
            forget(handle);
        }
        return result;
    }
}

/* The runtime frees after the unlocks of every lock. A buffer still locked stays in the table, refusing new locks,
   until its last unlock; the surface goes with the buffer's last reference. */
static void free_buffer(void *context, uint64_t handle) {
    (void)context;
    @autoreleasepool {
        MinatoHardwareBuffer *buffer = find_buffer(handle);
        if (buffer == nil) {
            return;
        }
        pthread_mutex_lock(&buffer->mutex);
        const BOOL first = !buffer->freed, open = buffer->locks != 0;
        buffer->freed = YES;
        pthread_mutex_unlock(&buffer->mutex);
        if (!first) {
            return;
        }
        if (open) {
            fprintf(stderr, "minato: a hardware buffer freed while locked stays mapped until its last unlock\n");
        } else {
            forget(handle);
        }
    }
}

// The context is the table itself, by which b)'s images know a handle as theirs (image.m).
static const struct apkrt_hardware_buffers functions = {supported, allocate, lock_buffer, unlock_buffer, free_buffer,
                                                        (void *)&functions};

const struct apkrt_hardware_buffers *minato_graphics_hardware_buffers(void) {
    return &functions;
}

void *minato_graphics_hardware_buffer_surface(uint64_t handle) {
    @autoreleasepool {
        MinatoHardwareBuffer *buffer = find_buffer(handle);
        if (buffer == nil) {
            return NULL;
        }
        pthread_mutex_lock(&buffer->mutex);
        const BOOL freed = buffer->freed;
        pthread_mutex_unlock(&buffer->mutex);
        if (freed) {
            return NULL;
        }
        CFRetain(buffer->surface);
        return buffer->surface;
    }
}

int minato_graphics_set_hardware_buffers(void) {
    @autoreleasepool {
        (void)metal_alignment(MTLPixelFormatRGBA8Unorm);  // the Metal device, made here and not on a guest thread
    }
    return apkrt_set_hardware_buffers(&functions) == 0 ? 0 : -1;
}
