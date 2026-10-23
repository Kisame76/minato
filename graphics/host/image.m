/* graphics/host's EGL images of the guest's hardware buffers (ADR-0058 b), decision 3), in front of ANGLE's
   eglCreateImageKHR, eglCreateImage, their destroys and eglTerminate; see angle_host.h. libEGL hands an image of
   EGL_NATIVE_BUFFER_ANDROID the buffer's ID (graphics/egl/image.c):
   - on ANGLE's Metal display, the buffer's IOSurface becomes a 2D MTLTexture of the system's default device, ANGLE's,
     without a copy, and that texture an EGL_METAL_TEXTURE_ANGLE image (patch 0010 lets an External texture take it);
     R8G8B8X8 samples its alpha as 1 through the texture's swizzle; a swizzled MTLTexture cannot be a render target,
     so a buffer that is also AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER is RGBA8 with ANGLE's internal format GL_RGB
     (EGL_TEXTURE_INTERNAL_FORMAT_ANGLE), whose draws leave alpha alone, and samples the alpha in memory, a named
     limit;
   - the image keeps a reference of the host's own to the buffer (apkrt_hardware_buffer_acquire_handle) until its
     destroy or its display's terminate, so the guest's last release does not free the memory under it;
   - the errors are ANGLE's, asked for with what gives them: EGL_BAD_CONTEXT for a context, EGL_BAD_PARAMETER for an
     ID without a buffer of graphics/host's, a BLOB or a buffer of more than one layer.
   - on ANGLE's Vulkan display (the ES 3.1 path), a VkImage of ANGLE's device over the IOSurface (angle_vulkan.m)
     becomes an EGL_VULKAN_IMAGE_ANGLE image; the VkImage and its memory go after the image's destroy, once the device
     is idle. R8G8B8X8 is RGBA8 there and samples the alpha in memory, a named limit: with the internal format GL_RGB,
     ANGLE clears the whole of an imported VkImage for its emulated alpha (init2DWeakReference does not take it as
     external).
   With ARC. */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "APKRuntime/apkrt.h"
#include "angle_host.h"
#include "egl_table.h"
#include "minato_graphics.h"

typedef unsigned int egl_boolean;
typedef int32_t egl_int;
typedef intptr_t egl_attrib;
typedef unsigned int egl_enum;
#define EGL_NONE 0x3038
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_METAL_TEXTURE_ANGLE 0x34A7
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE 0x345D
#define EGL_VULKAN_IMAGE_ANGLE 0x34D3
#define EGL_VULKAN_IMAGE_CREATE_INFO_HI_ANGLE 0x34D4
#define EGL_VULKAN_IMAGE_CREATE_INFO_LO_ANGLE 0x34D5
#define GL_RGB 0x1907

typedef void *(*create_image_khr_function)(void *display, void *context, egl_enum target, void *buffer,
                                           const egl_int *attributes);
typedef void *(*create_image_function)(void *display, void *context, egl_enum target, void *buffer,
                                       const egl_attrib *attributes);
typedef egl_boolean (*destroy_image_function)(void *display, void *image);

static create_image_khr_function create_image_khr;
static create_image_function create_image;
static destroy_image_function destroy_image_khr, destroy_image;

/* AHardwareBuffer's formats that an image takes, as graphics/host's buffers serve them (hardware_buffer.m). */
enum {
    format_rgba8 = 1,
    format_rgbx8 = 2,
    format_rgb565 = 4,
    format_rgba16f = 0x16,
    format_rgb10a2 = 0x2b,
    format_r8 = 0x38,
};

static const uint64_t usage_gpu_framebuffer = UINT64_C(1) << 9;

static MTLPixelFormat pixel_format(uint32_t format) {
    switch (format) {
        case format_rgba8:
        case format_rgbx8: return MTLPixelFormatRGBA8Unorm;
        case format_rgb565: return MTLPixelFormatB5G6R5Unorm;
        case format_rgba16f: return MTLPixelFormatRGBA16Float;
        case format_rgb10a2: return MTLPixelFormatRGB10A2Unorm;
        case format_r8: return MTLPixelFormatR8Unorm;
        default: return MTLPixelFormatInvalid;
    }
}

/* An image of a hardware buffer: its display, the buffer's ID, and the texture ANGLE's Metal image holds or the VkImage
   its Vulkan image holds. */
@interface MinatoBufferImage : NSObject {
@public
    void *display;
    uint64_t buffer;
    id<MTLTexture> texture;
    struct minato_vulkan_image *vulkan;
}
@end

@implementation MinatoBufferImage
- (void)dealloc {
    minato_graphics_angle_vulkan_image_free(vulkan);
}
@end

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSValue *, MinatoBufferImage *> *images;

/* The IOSurface and description of the buffer `buffer`, retained, with a handle of the host's own taken; NULL with
   none taken for an ID without a buffer of graphics/host's. */
static IOSurfaceRef surface_of(uint64_t buffer, struct apkrt_hardware_buffer_description *description) {
    uint64_t handle = 0;
    void *context = NULL;
    uint32_t stride = 0;
    if (buffer == 0 || apkrt_hardware_buffer_acquire_handle(buffer, &handle, &context, description, &stride) != 0) {
        return NULL;
    }
    IOSurfaceRef surface = context == minato_graphics_hardware_buffers()->context
                               ? (IOSurfaceRef)minato_graphics_hardware_buffer_surface(handle)
                               : NULL;
    if (surface == NULL || description->layers != 1) {
        if (surface != NULL) {
            CFRelease(surface);
        }
        apkrt_hardware_buffer_release_handle(buffer);
        return NULL;
    }
    return surface;
}

/* The texture of `surface` on ANGLE's device, and the internal format ANGLE is to take it as (0 for the texture's
   own); nil for a format it does not take. */
static id<MTLTexture> texture_of(IOSurfaceRef surface, const struct apkrt_hardware_buffer_description *description,
                                 unsigned *internal_format) {
    const MTLPixelFormat format = pixel_format(description->format);
    // Past 16384 a side Metal ends the process; hardware_buffer.m makes no such image buffer (ADR-0060 e2c2).
    if (format == MTLPixelFormatInvalid || description->width > 16384 || description->height > 16384) {
        return nil;
    }
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:description->width
                                                                                         height:description->height
                                                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModeShared;
    if (description->format == format_rgbx8 && (description->usage & usage_gpu_framebuffer) != 0) {
        *internal_format = GL_RGB;
    } else if (description->format == format_rgbx8) {
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                           MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
}

/* `make` calls ANGLE with a context, a target, a buffer and attributes to add to the app's (pairs ended by
   EGL_NONE). */
typedef void *(^maker)(void *context, egl_enum target, void *buffer, const egl_attrib *added);

/* ANGLE's image of the buffer `buffer`, made by `make`, and its record; with no image, ANGLE's error for the case. */
static void *buffer_image(void *display, void *context, uint64_t buffer, maker make) {
    static const egl_attrib nothing[] = {EGL_NONE};
    const int vulkan = minato_graphics_angle_vulkan_backend();
    if (context != NULL) {
        return make(context, vulkan ? EGL_VULKAN_IMAGE_ANGLE : EGL_METAL_TEXTURE_ANGLE, NULL, nothing);  // BAD_CONTEXT
    }
    struct apkrt_hardware_buffer_description description;
    IOSurfaceRef surface = surface_of(buffer, &description);
    MinatoBufferImage *record = [[MinatoBufferImage alloc] init];
    void *image = NULL;
    if (surface != NULL && vulkan) {
        record->vulkan = minato_graphics_angle_vulkan_image(surface, description.format, description.width,
                                                            description.height);
        if (record->vulkan != NULL) {
            const uintptr_t info = (uintptr_t)minato_graphics_angle_vulkan_image_info(record->vulkan);
            const egl_attrib added[] = {EGL_VULKAN_IMAGE_CREATE_INFO_HI_ANGLE, (egl_attrib)(info >> 32),
                                        EGL_VULKAN_IMAGE_CREATE_INFO_LO_ANGLE, (egl_attrib)(info & 0xffffffffu),
                                        EGL_NONE};
            image = make(NULL, EGL_VULKAN_IMAGE_ANGLE, minato_graphics_angle_vulkan_image_buffer(record->vulkan),
                         added);
        }
    } else if (surface != NULL) {
        unsigned internal_format = 0;
        record->texture = texture_of(surface, &description, &internal_format);
        if (record->texture != nil) {
            const egl_attrib added[] = {internal_format != 0 ? EGL_TEXTURE_INTERNAL_FORMAT_ANGLE : EGL_NONE,
                                        (egl_attrib)internal_format, EGL_NONE};
            image = make(NULL, EGL_METAL_TEXTURE_ANGLE, (__bridge void *)record->texture, added);
        }
    }
    if (surface != NULL) {
        CFRelease(surface);
    }
    if (image == NULL) {
        const int made_one = record->texture != nil || record->vulkan != NULL;
        if (surface != NULL) {
            apkrt_hardware_buffer_release_handle(buffer);
        }
        // ANGLE's error once it had a texture or an image of ours; else EGL_BAD_PARAMETER, as ANGLE has no images of
        // native buffers here.
        return made_one ? NULL : make(NULL, EGL_NATIVE_BUFFER_ANDROID, NULL, nothing);
    }
    record->display = display;
    record->buffer = buffer;
    NSValue *key = [NSValue valueWithPointer:image];
    pthread_mutex_lock(&lock);
    if (images == nil) {
        images = [[NSMutableDictionary alloc] init];
    }
    // A live image has its own pointer; a record still under it is of an image that went past these functions.
    MinatoBufferImage *stale = images[key];
    images[key] = record;
    pthread_mutex_unlock(&lock);
    if (stale != nil) {
        apkrt_hardware_buffer_release_handle(stale->buffer);
    }
    return image;
}

/* The app's attributes with `added` after them, for EGLint and EGLAttrib lists; NULL without memory. */
#define WITH_ADDED(name, type)                                                     \
    static type *name(const type *attributes, const egl_attrib *added) {          \
        size_t length = 0, more = 0;                                               \
        while (attributes != NULL && attributes[length] != EGL_NONE) {             \
            length += 2;                                                           \
        }                                                                          \
        while (added[more] != EGL_NONE) {                                          \
            more += 2;                                                             \
        }                                                                          \
        type *ours = malloc((length + more + 1) * sizeof *ours);                   \
        if (ours != NULL) {                                                        \
            for (size_t index = 0; index < length; index++) {                      \
                ours[index] = attributes[index];                                   \
            }                                                                      \
            for (size_t index = 0; index < more; index++) {                        \
                ours[length + index] = (type)added[index];                         \
            }                                                                      \
            ours[length + more] = EGL_NONE;                                        \
        }                                                                          \
        return ours;                                                               \
    }
WITH_ADDED(with_added_khr, egl_int)
WITH_ADDED(with_added, egl_attrib)
#undef WITH_ADDED

static void *image_created_khr(void *display, void *context, egl_enum target, void *buffer, const egl_int *attributes) {
    if (target != EGL_NATIVE_BUFFER_ANDROID) {
        return create_image_khr(display, context, target, buffer, attributes);
    }
    @autoreleasepool {
        return buffer_image(display, context, (uint64_t)(uintptr_t)buffer,
                            ^(void *asked, egl_enum ours, void *of, const egl_attrib *added) {
                                egl_int *list = with_added_khr(attributes, added);
                                void *const made = list != NULL ? create_image_khr(display, asked, ours, of, list)
                                                                : NULL;
                                free(list);
                                return made;
                            });
    }
}

static void *image_created(void *display, void *context, egl_enum target, void *buffer, const egl_attrib *attributes) {
    if (target != EGL_NATIVE_BUFFER_ANDROID) {
        return create_image(display, context, target, buffer, attributes);
    }
    @autoreleasepool {
        return buffer_image(display, context, (uint64_t)(uintptr_t)buffer,
                            ^(void *asked, egl_enum ours, void *of, const egl_attrib *added) {
                                egl_attrib *list = with_added(attributes, added);
                                void *const made = list != NULL ? create_image(display, asked, ours, of, list)
                                                                : NULL;
                                free(list);
                                return made;
                            });
    }
}

/* Takes the records of an image, or of every image of a display for `image` NULL, and lets their buffers go. */
static void forget(void *display, void *image) {
    NSMutableArray<MinatoBufferImage *> *gone = [[NSMutableArray alloc] init];
    pthread_mutex_lock(&lock);
    for (NSValue *key in images.allKeys) {
        MinatoBufferImage *record = images[key];
        if (record->display == display && (image == NULL || key.pointerValue == image)) {
            [gone addObject:record];
            [images removeObjectForKey:key];
        }
    }
    pthread_mutex_unlock(&lock);
    for (MinatoBufferImage *record in gone) {
        apkrt_hardware_buffer_release_handle(record->buffer);
    }
}

static egl_boolean image_destroyed_khr(void *display, void *image) {
    const egl_boolean result = destroy_image_khr(display, image);
    if (result && image != NULL) {
        @autoreleasepool {
            forget(display, image);
        }
    }
    return result;
}

static egl_boolean image_destroyed(void *display, void *image) {
    const egl_boolean result = destroy_image(display, image);
    if (result && image != NULL) {
        @autoreleasepool {
            forget(display, image);
        }
    }
    return result;
}

void minato_graphics_images_terminated(void *display) {
    @autoreleasepool {
        forget(display, NULL);
    }
}

#define WRAP(name, angle, ours)                                         \
    angle = (__typeof__(angle))table[MINATO_EGL_SLOT_##name]; \
    table[MINATO_EGL_SLOT_##name] = (void *)ours;

void minato_graphics_images_wrap(void **table) {
    WRAP(eglCreateImageKHR, create_image_khr, image_created_khr)
    WRAP(eglCreateImage, create_image, image_created)
    WRAP(eglDestroyImageKHR, destroy_image_khr, image_destroyed_khr)
    WRAP(eglDestroyImage, destroy_image, image_destroyed)
}
#undef WRAP
