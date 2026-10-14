/* libvulkan's VK_ANDROID_external_memory_android_hardware_buffer and VK_EXT_queue_family_foreign, both its own
   (ADR-0058 decision 7; ADR-0030 decision 11): AHardwareBuffers of graphics/host's hardware buffers, whose memory is
   an IOSurface, imported into and exported from the dedicated VkDeviceMemory of a VkImage.
   - MoltenVK takes an image's IOSurface at the image, where Android binds memory to an image after it is made. So
     the app's VkImage is MoltenVK's from vkCreateImage, without the handle type, and vkAllocateMemory hands
     graphics/host the buffer's ID first in its chain (host_protocol.h), which gives the dedicated image the buffer's
     IOSurface before MoltenVK allocates the memory. Every other call that takes the image goes to MoltenVK as it is.
   - The memory holds a reference to its buffer until vkFreeMemory, as Android's does. An export allocates a buffer
     from the dedicated image's description (AHardwareBuffer_allocate) and imports it, and
     vkGetMemoryAndroidHardwareBufferANDROID gives the app a reference of its own.
   - A buffer's properties: the format of the specification's equivalence table, the size and memory types of an
     image of its format and size as MoltenVK makes it (the device-local types the host does not map, where there are
     some), the format's features for optimal tiling with a chroma location, and the buffer format as its external
     format, which an image of VK_FORMAT_UNDEFINED may name for that VkFormat.
   - Barriers to and from the foreign and the external queue family reach MoltenVK without a transfer of ownership.
   - An image takes a buffer once, in the one shape an IOSurface has: 2D, one sample, level and layer, optimal tiling,
     the buffer's format and size; anything else answers VK_ERROR_INVALID_EXTERNAL_HANDLE at vkAllocateMemory.
   - Named limits: no buffer without an image (a BLOB for a VkBuffer, the only import the specification allows
     without a dedicated image), no other external format (YUV), no storage, cube maps or protected content:
     VK_ERROR_INVALID_EXTERNAL_HANDLE or VK_ERROR_FORMAT_NOT_SUPPORTED. R8G8B8X8 is R8G8B8A8 and samples the alpha in
     memory, as on the EGL path (graphics/host/image.m). A second image bound to the memory, which the specification
     forbids, gets a texture of its own from MoltenVK, not the buffer's (with VK_IMAGE_CREATE_ALIAS_BIT, the
     buffer's). */
#include <android/hardware_buffer.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "libvulkan.h"

MINATO_HOST(vkCreateImage);
MINATO_HOST(vkDestroyImage);
MINATO_HOST(vkAllocateMemory);
MINATO_HOST(vkFreeMemory);
MINATO_HOST(vkGetPhysicalDeviceImageFormatProperties2);

#define HANDLE_TYPE VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID

/* The formats of the equivalence table that graphics/host's buffers make (hardware_buffer.m); its R8 is not in the
   table, so it would take an external format. */
static const struct {
    uint32_t buffer;
    VkFormat format;
} formats[] = {
    {AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
    {AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
    {AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16},
    {AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
    {AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
};

static VkFormat format_of(uint32_t buffer) {
    for (size_t index = 0; index < sizeof formats / sizeof *formats; index++) {
        if (formats[index].buffer == buffer) {
            return formats[index].format;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

/* The buffer format of an image's, the first of the table's (R8G8B8A8 for VK_FORMAT_R8G8B8A8_UNORM); 0 for none. */
static uint32_t buffer_format_of(VkFormat format) {
    for (size_t index = 0; index < sizeof formats / sizeof *formats; index++) {
        if (formats[index].format == format) {
            return formats[index].buffer;
        }
    }
    return 0;
}

/* The buffer usage of an image's usage and flags, by the specification's equivalence table; GPU_SAMPLED_IMAGE for
   an image of transfers alone, as the specification asks for one GPU usage at least. */
static uint64_t buffer_usage_of(VkImageUsageFlags usage, VkImageCreateFlags flags, uint32_t levels) {
    uint64_t buffer = 0;
    if ((usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) != 0) {
        buffer |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    }
    if ((usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0) {
        buffer |= AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
    }
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) {
        buffer |= AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    }
    if ((flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0) {
        buffer |= AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP;
    }
    if ((flags & VK_IMAGE_CREATE_PROTECTED_BIT) != 0) {
        buffer |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;
    }
    if (levels > 1) {
        buffer |= AHARDWAREBUFFER_USAGE_GPU_MIPMAP_COMPLETE;
    }
    if ((buffer & (AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                   AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER)) == 0) {
        buffer |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    }
    return buffer;
}

/* What libvulkan keeps: the images made for the handle type, whose description an export allocates its buffer by, and
   the memory of buffers with the reference each holds. */
struct image {
    VkImage image;
    VkFormat format;
    VkExtent3D extent;
    uint32_t layers, levels;
    VkImageUsageFlags usage;
    VkImageCreateFlags flags;
    VkImageType type;
    VkSampleCountFlagBits samples;
    VkImageTiling tiling;
    int given;  /* the image has a buffer's memory */
};
struct memory {
    VkDeviceMemory memory;
    AHardwareBuffer *buffer;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct image *images;
static struct memory *memories;
static size_t images_kept, images_room, memories_kept, memories_room;

/* Room for one more of `size` bytes in *list of *room: 0, or -1 without memory. Under the lock. */
static int grow(void **list, size_t count, size_t *room, size_t size) {
    if (count < *room) {
        return 0;
    }
    const size_t larger = *room != 0 ? *room * 2 : 8;
    void *grown = realloc(*list, larger * size);
    if (grown == NULL) {
        return -1;
    }
    *list = grown;
    *room = larger;
    return 0;
}

static int image_keep(const struct image *kept) {
    pthread_mutex_lock(&lock);
    const int result = grow((void **)&images, images_kept, &images_room, sizeof *images);
    if (result == 0) {
        images[images_kept++] = *kept;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

/* The image `image` kept, into *found: 1, else 0. */
static int image_find(VkImage image, struct image *found) {
    int known = 0;
    pthread_mutex_lock(&lock);
    for (size_t index = 0; index < images_kept && !known; index++) {
        if (images[index].image == image) {
            *found = images[index];
            known = 1;
        }
    }
    pthread_mutex_unlock(&lock);
    return known;
}

/* Marks the image `image` as given a buffer's memory: 1, or 0 when it had one already. */
static int image_give(VkImage image) {
    int first = 0;
    pthread_mutex_lock(&lock);
    for (size_t index = 0; index < images_kept; index++) {
        if (images[index].image == image) {
            first = !images[index].given;
            images[index].given = 1;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return first;
}

static void image_forget(VkImage image) {
    pthread_mutex_lock(&lock);
    for (size_t index = 0; index < images_kept; index++) {
        if (images[index].image == image) {
            images[index] = images[--images_kept];
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

static int memory_keep(VkDeviceMemory memory, AHardwareBuffer *buffer) {
    pthread_mutex_lock(&lock);
    const int result = grow((void **)&memories, memories_kept, &memories_room, sizeof *memories);
    if (result == 0) {
        memories[memories_kept++] = (struct memory){memory, buffer};
    }
    pthread_mutex_unlock(&lock);
    return result;
}

/* The buffer of `memory`, with a reference of the caller's when `acquire` is set, or with the memory's reference taken
   out of the list when it is not; NULL for memory without one. */
static AHardwareBuffer *memory_buffer(VkDeviceMemory memory, int acquire) {
    AHardwareBuffer *buffer = NULL;
    pthread_mutex_lock(&lock);
    for (size_t index = 0; index < memories_kept; index++) {
        if (memories[index].memory == memory) {
            buffer = memories[index].buffer;
            if (acquire) {
                AHardwareBuffer_acquire(buffer);
            } else {
                memories[index] = memories[--memories_kept];
            }
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return buffer;
}

/* The physical device of a device libvulkan made, or VK_NULL_HANDLE. */
static VkPhysicalDevice physical_of(VkDevice device) {
    uint32_t owned;
    struct minato_queues *queues = NULL;
    return minato_recall(device, &owned, &queues) && queues != NULL ? queues->physical : VK_NULL_HANDLE;
}

/* The memory requirements of a sampled image of `format`, `width` x `height` and `layers`, as MoltenVK makes it. */
static VkResult requirements_of(VkDevice device, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                                VkMemoryRequirements *requirements) {
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image;
    const VkResult result = minato_host_vkCreateImage(device, &info, NULL, &image);
    if (result == VK_SUCCESS) {
        vkGetImageMemoryRequirements(device, image, requirements);
        minato_host_vkDestroyImage(device, image, NULL);
    }
    return result;
}

/* Of the memory types in `bits`, the device-local ones the host does not map and that are not lazily allocated; else
   those that are not lazily allocated; else `bits`. */
static uint32_t memory_types_of(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t device_only = 0, allocated = 0;
    for (uint32_t index = 0; index < memory.memoryTypeCount; index++) {
        const VkMemoryPropertyFlags flags = memory.memoryTypes[index].propertyFlags;
        if ((bits & (1u << index)) == 0 || (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0) {
            continue;
        }
        allocated |= 1u << index;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
            device_only |= 1u << index;
        }
    }
    return device_only != 0 ? device_only : allocated != 0 ? allocated : bits;
}

VkResult vkGetAndroidHardwareBufferPropertiesANDROID(VkDevice device, const struct AHardwareBuffer *buffer,
                                                     VkAndroidHardwareBufferPropertiesANDROID *properties) {
    uint64_t id = 0;
    if (buffer == NULL || &AHardwareBuffer_getId == NULL || AHardwareBuffer_getId(buffer, &id) != 0) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    AHardwareBuffer_Desc description;
    memset(&description, 0, sizeof description);  // left as it is for a buffer that is gone
    AHardwareBuffer_describe(buffer, &description);
    const VkFormat format = format_of(description.format);
    const VkPhysicalDevice physical = physical_of(device);
    if (format == VK_FORMAT_UNDEFINED || physical == VK_NULL_HANDLE) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkMemoryRequirements requirements;
    const VkResult result =
        requirements_of(device, format, description.width, description.height, description.layers, &requirements);
    if (result != VK_SUCCESS) {
        return result == VK_ERROR_OUT_OF_HOST_MEMORY ? result : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    properties->allocationSize = requirements.size;
    properties->memoryTypeBits = memory_types_of(physical, requirements.memoryTypeBits);

    /* The format's properties: its own format, no conversion of its components but R8G8B8X8's alpha of one, as the
       specification asks of a format of the table, and features with a chroma location, which it asks for too. */
    VkFormatProperties3 features3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 features = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &features3};
    vkGetPhysicalDeviceFormatProperties2(physical, format, &features);
    const VkComponentMapping components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        description.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM ? VK_COMPONENT_SWIZZLE_ONE
                                                                    : VK_COMPONENT_SWIZZLE_IDENTITY};
    VkAndroidHardwareBufferFormatPropertiesANDROID *found =
        minato_chain_find(properties->pNext, VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID);
    if (found != NULL) {
        found->format = format;
        found->externalFormat = description.format;
        found->formatFeatures =
            features.formatProperties.optimalTilingFeatures | VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
        found->samplerYcbcrConversionComponents = components;
        found->suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
        found->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
        found->suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        found->suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    }
    VkAndroidHardwareBufferFormatProperties2ANDROID *found2 =
        minato_chain_find(properties->pNext, VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_2_ANDROID);
    if (found2 != NULL) {
        found2->format = format;
        found2->externalFormat = description.format;
        found2->formatFeatures = features3.optimalTilingFeatures | VK_FORMAT_FEATURE_2_MIDPOINT_CHROMA_SAMPLES_BIT;
        found2->samplerYcbcrConversionComponents = components;
        found2->suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
        found2->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
        found2->suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        found2->suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    }
    return VK_SUCCESS;
}

VkResult vkGetMemoryAndroidHardwareBufferANDROID(VkDevice device,
                                                 const VkMemoryGetAndroidHardwareBufferInfoANDROID *info,
                                                 struct AHardwareBuffer **buffer) {
    (void)device;
    AHardwareBuffer *found = memory_buffer(info->memory, 1);
    if (found == NULL) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    *buffer = found;
    return VK_SUCCESS;
}

/* Out of an image's chain: an external format of 0, which is no external format. Changed: the handle types, without
   the buffer's. */
static const VkStructureType image_removed[] = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
static const VkStructureType image_changed[] = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};

VkResult vkCreateImage(VkDevice device, const VkImageCreateInfo *info, const VkAllocationCallbacks *allocator,
                       VkImage *image) {
    (void)allocator;  /* never passed on (decision 5) */
    const VkExternalMemoryImageCreateInfo *external =
        minato_chain_find(info->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    const VkExternalFormatANDROID *external_format =
        minato_chain_find(info->pNext, VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID);
    const int buffer = external != NULL && (external->handleTypes & HANDLE_TYPE) != 0;
    if (!buffer && external_format == NULL) {
        return minato_host_vkCreateImage(device, info, NULL, image);
    }
    /* An external format libvulkan handed out is the buffer format, which stands for its VkFormat; any other is a
       named limit (YUV). */
    const VkFormat named = external_format != NULL && external_format->externalFormat != 0
                               ? format_of((uint32_t)external_format->externalFormat)
                               : VK_FORMAT_UNDEFINED;
    if (external_format != NULL && external_format->externalFormat != 0 &&
        (named == VK_FORMAT_UNDEFINED || info->format != VK_FORMAT_UNDEFINED)) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    const void *chain;
    void *copy;
    VkResult result = minato_chain_edit(info->pNext, image_removed, sizeof image_removed / sizeof *image_removed,
                                        image_changed, sizeof image_changed / sizeof *image_changed, &chain, &copy);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkExternalMemoryImageCreateInfo *changed =
        minato_chain_find(chain, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    if (changed != NULL) {
        changed->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)HANDLE_TYPE;
    }
    VkImageCreateInfo ours = *info;
    ours.pNext = chain;
    if (named != VK_FORMAT_UNDEFINED) {
        ours.format = named;
    }
    result = minato_host_vkCreateImage(device, &ours, NULL, image);
    free(copy);
    if (result != VK_SUCCESS || !buffer) {
        return result;
    }
    const struct image kept = {*image,           ours.format,      info->extent,  info->arrayLayers,
                               info->mipLevels,  info->usage,       info->flags,   info->imageType,
                               info->samples,    info->tiling,      0};
    if (image_keep(&kept) != 0) {
        minato_host_vkDestroyImage(device, *image, NULL);
        *image = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

void vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    if (image != VK_NULL_HANDLE) {
        image_forget(image);
    }
    minato_host_vkDestroyImage(device, image, NULL);
}

/* Whether an image of the handle type has the one shape a buffer's IOSurface takes: 2D, one sample, one level, one
   layer, optimal tiling, a format of the table, and no buffer's memory yet. vkUseIOSurfaceMVK checks only the size
   and the bytes per texel. */
static int image_takes_buffer(const struct image *kept) {
    return kept->type == VK_IMAGE_TYPE_2D && kept->samples == VK_SAMPLE_COUNT_1_BIT && kept->levels == 1 &&
           kept->layers == 1 && kept->extent.depth == 1 && kept->tiling == VK_IMAGE_TILING_OPTIMAL &&
           buffer_format_of(kept->format) != 0 && !kept->given;
}

/* Whether an imported buffer is the image's: its format stands for the image's, and its size is the image's. */
static int buffer_fits(const struct image *kept, const AHardwareBuffer *buffer) {
    AHardwareBuffer_Desc description;
    memset(&description, 0, sizeof description);
    AHardwareBuffer_describe(buffer, &description);
    return format_of(description.format) == kept->format && description.width == kept->extent.width &&
           description.height == kept->extent.height && description.layers == 1;
}

/* An export's buffer: one of the dedicated image's description, with a reference of the caller's; NULL for a
   description graphics/host's buffers do not make. */
static AHardwareBuffer *exported_buffer(const struct image *image) {
    const struct image kept = *image;
    AHardwareBuffer_Desc description;
    memset(&description, 0, sizeof description);
    description.width = kept.extent.width;
    description.height = kept.extent.height;
    description.layers = kept.layers;
    description.format = buffer_format_of(kept.format);
    description.usage = buffer_usage_of(kept.usage, kept.flags, kept.levels);
    AHardwareBuffer *buffer = NULL;
    return description.format != 0 && AHardwareBuffer_allocate(&description, &buffer) == 0 ? buffer : NULL;
}

/* Out of an allocation's chain: the import and the export of a buffer, which graphics/host's structure stands for. */
static const VkStructureType memory_removed[] = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                 VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};

VkResult vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator,
                          VkDeviceMemory *memory) {
    (void)allocator;
    const VkImportAndroidHardwareBufferInfoANDROID *imported =
        minato_chain_find(info->pNext, VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
    const VkExportMemoryAllocateInfo *exported =
        minato_chain_find(info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    if (imported == NULL && (exported == NULL || (exported->handleTypes & HANDLE_TYPE) == 0)) {
        return minato_host_vkAllocateMemory(device, info, NULL, memory);
    }
    const VkMemoryDedicatedAllocateInfo *dedicated =
        minato_chain_find(info->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    struct image kept;
    /* Named limits: no buffer without an image (a BLOB's for a VkBuffer), and only the shape an IOSurface takes; an
       image of another handle type, or one that has a buffer's memory, is the app's error. */
    if (dedicated == NULL || dedicated->image == VK_NULL_HANDLE || !image_find(dedicated->image, &kept) ||
        !image_takes_buffer(&kept)) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    AHardwareBuffer *buffer = NULL;
    VkDeviceSize size = info->allocationSize;
    if (imported != NULL) {
        buffer = imported->buffer;
        if (buffer == NULL || !buffer_fits(&kept, buffer)) {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        AHardwareBuffer_acquire(buffer);
    } else if ((buffer = exported_buffer(&kept)) == NULL) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (size == 0) {
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, dedicated->image, &requirements);
        size = requirements.size;
    }
    uint64_t id = 0;
    const void *chain;
    void *copy = NULL;
    VkResult result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (&AHardwareBuffer_getId != NULL && AHardwareBuffer_getId(buffer, &id) == 0) {
        result = minato_chain_edit(info->pNext, memory_removed, sizeof memory_removed / sizeof *memory_removed, NULL,
                                   0, &chain, &copy);
    }
    if (result == VK_SUCCESS) {
        const struct minato_hardware_buffer named = {MINATO_STRUCTURE_TYPE_HARDWARE_BUFFER, chain, id};
        VkMemoryAllocateInfo ours = *info;
        ours.pNext = &named;
        ours.allocationSize = size;
        result = minato_host_vkAllocateMemory(device, &ours, NULL, memory);
        free(copy);
    }
    if (result == VK_SUCCESS && memory_keep(*memory, buffer) != 0) {
        minato_host_vkFreeMemory(device, *memory, NULL);
        *memory = VK_NULL_HANDLE;
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (result == VK_SUCCESS) {
        (void)image_give(dedicated->image);
    }
    if (result != VK_SUCCESS) {
        AHardwareBuffer_release(buffer);
    }
    return result;
}

void vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    AHardwareBuffer *buffer = memory != VK_NULL_HANDLE ? memory_buffer(memory, 0) : NULL;
    minato_host_vkFreeMemory(device, memory, NULL);
    // After MoltenVK let the memory go: the image keeps its IOSurface, the buffer's memory, for itself.
    if (buffer != NULL) {
        AHardwareBuffer_release(buffer);
    }
}

/* Out of a format query's chain: the buffer's handle type, which MoltenVK does not know. */
static const VkStructureType format_removed[] = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};

VkResult vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physical,
                                                   const VkPhysicalDeviceImageFormatInfo2 *info,
                                                   VkImageFormatProperties2 *properties) {
    const VkPhysicalDeviceExternalImageFormatInfo *external =
        minato_chain_find(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    if (external == NULL || external->handleType != HANDLE_TYPE) {
        return minato_host_vkGetPhysicalDeviceImageFormatProperties2(physical, info, properties);
    }
    /* What graphics/host's buffers do not make (hardware_buffer.m): storage, cube maps, protected content. */
    if (buffer_format_of(info->format) == 0 || info->type != VK_IMAGE_TYPE_2D ||
        info->tiling != VK_IMAGE_TILING_OPTIMAL || (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 ||
        (info->flags & (VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_PROTECTED_BIT)) != 0) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    const void *chain;
    void *copy;
    VkResult result = minato_chain_edit(info->pNext, format_removed, sizeof format_removed / sizeof *format_removed,
                                        NULL, 0, &chain, &copy);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkPhysicalDeviceImageFormatInfo2 ours = *info;
    ours.pNext = chain;
    result = minato_host_vkGetPhysicalDeviceImageFormatProperties2(physical, &ours, properties);
    free(copy);
    if (result != VK_SUCCESS) {
        return result;
    }
    /* The one shape an image of a buffer takes (image_takes_buffer). */
    VkImageFormatProperties *limits = &properties->imageFormatProperties;
    limits->maxExtent.depth = 1;
    limits->maxMipLevels = 1;
    limits->maxArrayLayers = 1;
    limits->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    VkExternalImageFormatProperties *memory =
        minato_chain_find(properties->pNext, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    if (memory != NULL) {
        memory->externalMemoryProperties = (VkExternalMemoryProperties){
            VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
            HANDLE_TYPE, HANDLE_TYPE};
    }
    VkAndroidHardwareBufferUsageANDROID *usage =
        minato_chain_find(properties->pNext, VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID);
    if (usage != NULL) {
        usage->androidHardwareBufferUsage = buffer_usage_of(info->usage, info->flags, 1);
    }
    return VK_SUCCESS;
}

/* The queue families no queue of MoltenVK's has: the foreign one of VK_EXT_queue_family_foreign and the external one
   of Vulkan 1.1. A barrier that passes an image or a buffer to or from one of them goes to MoltenVK with both
   families VK_QUEUE_FAMILY_IGNORED, a barrier without a transfer of ownership: its layouts and accesses stay, and
   MoltenVK never sees either value. Only a call with such a barrier copies its barriers. */
static int outside(uint32_t family) {
    return family == VK_QUEUE_FAMILY_FOREIGN_EXT || family == VK_QUEUE_FAMILY_EXTERNAL;
}

/* For each kind of barrier: whether one of `count` names a family outside MoltenVK's, and the change of those that
   do in a copy. */
#define FAMILIES(type)                                                                                     \
    static int outside_##type(const type *barriers, uint32_t count) {                                     \
        for (uint32_t index = 0; index < count; index++) {                                                 \
            if (outside(barriers[index].srcQueueFamilyIndex) || outside(barriers[index].dstQueueFamilyIndex)) { \
                return 1;                                                                                  \
            }                                                                                              \
        }                                                                                                  \
        return 0;                                                                                          \
    }                                                                                                      \
    static type *ignored_##type(type *copy, const type *barriers, uint32_t count) {                       \
        memcpy(copy, barriers, count * sizeof *copy);                                                      \
        for (uint32_t index = 0; index < count; index++) {                                                 \
            if (outside(copy[index].srcQueueFamilyIndex) || outside(copy[index].dstQueueFamilyIndex)) {     \
                copy[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;                                 \
                copy[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;                                 \
            }                                                                                              \
        }                                                                                                  \
        return copy;                                                                                       \
    }
FAMILIES(VkBufferMemoryBarrier)
FAMILIES(VkImageMemoryBarrier)
FAMILIES(VkBufferMemoryBarrier2)
FAMILIES(VkImageMemoryBarrier2)
#undef FAMILIES

MINATO_HOST(vkCmdPipelineBarrier);
MINATO_HOST(vkCmdWaitEvents);
MINATO_HOST(vkCmdPipelineBarrier2);
MINATO_HOST(vkCmdWaitEvents2);
MINATO_HOST(vkCmdSetEvent2);

/* The barriers of Vulkan 1.0's two calls: the app's, or copies in one block in *copy, which the caller frees. Without
   memory for the copies, the app's, whose families MoltenVK keeps no use of. */
static void barriers_of(uint32_t buffer_count, const VkBufferMemoryBarrier **buffers, uint32_t image_count,
                        const VkImageMemoryBarrier **images_of, void **copy) {
    *copy = NULL;
    if (!outside_VkBufferMemoryBarrier(*buffers, buffer_count) &&
        !outside_VkImageMemoryBarrier(*images_of, image_count)) {
        return;
    }
    VkImageMemoryBarrier *image_copy = malloc(image_count * sizeof *image_copy + buffer_count * sizeof **buffers + 1);
    if (image_copy == NULL) {
        return;
    }
    *copy = image_copy;
    *buffers = ignored_VkBufferMemoryBarrier((VkBufferMemoryBarrier *)(image_copy + image_count), *buffers,
                                             buffer_count);
    *images_of = ignored_VkImageMemoryBarrier(image_copy, *images_of, image_count);
}

void vkCmdPipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags source, VkPipelineStageFlags destination,
                          VkDependencyFlags dependencies, uint32_t memory_count, const VkMemoryBarrier *memory,
                          uint32_t buffer_count, const VkBufferMemoryBarrier *buffers, uint32_t image_count,
                          const VkImageMemoryBarrier *images_of) {
    void *copy;
    barriers_of(buffer_count, &buffers, image_count, &images_of, &copy);
    minato_host_vkCmdPipelineBarrier(commands, source, destination, dependencies, memory_count, memory, buffer_count,
                                     buffers, image_count, images_of);
    free(copy);
}

void vkCmdWaitEvents(VkCommandBuffer commands, uint32_t event_count, const VkEvent *events,
                     VkPipelineStageFlags source, VkPipelineStageFlags destination, uint32_t memory_count,
                     const VkMemoryBarrier *memory, uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
                     uint32_t image_count, const VkImageMemoryBarrier *images_of) {
    void *copy;
    barriers_of(buffer_count, &buffers, image_count, &images_of, &copy);
    minato_host_vkCmdWaitEvents(commands, event_count, events, source, destination, memory_count, memory, buffer_count,
                                buffers, image_count, images_of);
    free(copy);
}

/* `count` dependency infos: the app's, or copies with copies of their barriers in one block in *copy, which the caller
   frees. Without memory for the copies, the app's. */
static const VkDependencyInfo *dependencies_of(const VkDependencyInfo *infos, uint32_t count, void **copy) {
    *copy = NULL;
    size_t bytes = 0;
    for (uint32_t index = 0; index < count; index++) {
        const VkDependencyInfo *info = &infos[index];
        if (outside_VkBufferMemoryBarrier2(info->pBufferMemoryBarriers, info->bufferMemoryBarrierCount) ||
            outside_VkImageMemoryBarrier2(info->pImageMemoryBarriers, info->imageMemoryBarrierCount)) {
            bytes += info->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2) +
                     info->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2);
        }
    }
    VkDependencyInfo *made = bytes != 0 ? malloc(count * sizeof *made + bytes) : NULL;
    if (made == NULL) {
        return infos;
    }
    memcpy(made, infos, count * sizeof *made);
    /* The barriers after the infos: every size is a multiple of 8, so each copy stays aligned. */
    char *at = (char *)(made + count);
    for (uint32_t index = 0; index < count; index++) {
        VkDependencyInfo *info = &made[index];
        if (!outside_VkBufferMemoryBarrier2(info->pBufferMemoryBarriers, info->bufferMemoryBarrierCount) &&
            !outside_VkImageMemoryBarrier2(info->pImageMemoryBarriers, info->imageMemoryBarrierCount)) {
            continue;
        }
        info->pImageMemoryBarriers = ignored_VkImageMemoryBarrier2((VkImageMemoryBarrier2 *)at,
                                                                   info->pImageMemoryBarriers,
                                                                   info->imageMemoryBarrierCount);
        at += info->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2);
        info->pBufferMemoryBarriers = ignored_VkBufferMemoryBarrier2((VkBufferMemoryBarrier2 *)at,
                                                                     info->pBufferMemoryBarriers,
                                                                     info->bufferMemoryBarrierCount);
        at += info->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2);
    }
    *copy = made;
    return made;
}

void vkCmdPipelineBarrier2(VkCommandBuffer commands, const VkDependencyInfo *info) {
    void *copy;
    minato_host_vkCmdPipelineBarrier2(commands, dependencies_of(info, 1, &copy));
    free(copy);
}

void vkCmdWaitEvents2(VkCommandBuffer commands, uint32_t count, const VkEvent *events, const VkDependencyInfo *infos) {
    void *copy;
    minato_host_vkCmdWaitEvents2(commands, count, events, dependencies_of(infos, count, &copy));
    free(copy);
}

void vkCmdSetEvent2(VkCommandBuffer commands, VkEvent event, const VkDependencyInfo *info) {
    void *copy;
    minato_host_vkCmdSetEvent2(commands, event, dependencies_of(info, 1, &copy));
    free(copy);
}
