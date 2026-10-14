/* ADR-0058 decision 7: external-memory-host-test, libvulkan's AHardwareBuffer functions (graphics/vulkan/
   external_memory.c, with common.c's chains and handles) built for the Mac against a MoltenVK of the test's own and a
   counted fake AHardwareBuffer, so that this preset's AddressSanitizer and UndefinedBehaviorSanitizer, and a run under
   ThreadSanitizer by hand, see the references and lists that vk-ahb-test runs as guest code:
   - an import holds one reference until vkFreeMemory, after the app's release; graphics/host gets the buffer's ID
     first in the chain, without the import, with the dedicated image;
   - an export allocates a buffer of the image's description, each vkGetMemoryAndroidHardwareBufferANDROID gives one
     more reference, and vkFreeMemory gives the memory's back;
   - the refusals keep no reference: a failed allocation, a buffer of another size, a second buffer for an image;
   - an image of an external format libvulkan handed out reaches MoltenVK with its VkFormat;
   - barriers with the foreign or the external queue family reach MoltenVK with both families ignored and leave the
     app's alone, and barriers without reach it as the app's own;
   - threads that import, get, free and destroy at once end with every buffer freed. */
#include <android/hardware_buffer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libvulkan.h"

static int failures;

static void check(int condition, const char *what) {
    printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    failures += !condition;
}

/* The fake buffers: their description and references, by pointer. */
struct AHardwareBuffer {
    AHardwareBuffer_Desc description;
    atomic_int references;
    uint64_t id;
};
static atomic_int buffers_alive;
static atomic_uint_fast64_t next_id = 1;

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *description, AHardwareBuffer **out) {
    AHardwareBuffer *made = calloc(1, sizeof *made);
    made->description = *description;
    made->description.stride = description->width;
    atomic_init(&made->references, 1);
    made->id = atomic_fetch_add(&next_id, 1);
    atomic_fetch_add(&buffers_alive, 1);
    *out = made;
    return 0;
}

void AHardwareBuffer_acquire(AHardwareBuffer *buffer) {
    atomic_fetch_add(&buffer->references, 1);
}

void AHardwareBuffer_release(AHardwareBuffer *buffer) {
    if (atomic_fetch_sub(&buffer->references, 1) == 1) {
        atomic_fetch_sub(&buffers_alive, 1);
        free(buffer);
    }
}

void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *description) {
    *description = buffer->description;
}

int AHardwareBuffer_getId(const AHardwareBuffer *buffer, uint64_t *id) {
    *id = buffer->id;
    return 0;
}

/* common.c's window gate, which nothing here calls. */
long minato_host_syscall(long number, ...) {
    (void)number;
    return -1;
}

/* The test's MoltenVK: handles are counters; what an allocation and a barrier got is kept. */
static atomic_uint_fast64_t handles = 0x1000;
static _Thread_local uint64_t got_buffer;
static _Thread_local VkImage got_dedicated;
static _Thread_local int got_import, got_export;
static _Thread_local VkFormat got_format;
static _Thread_local VkResult allocation_answer = VK_SUCCESS;
static _Thread_local uint32_t got_families[8];
static _Thread_local const void *got_barriers;

VkResult minato_host_vkCreateImage(VkDevice device, const VkImageCreateInfo *info, const VkAllocationCallbacks *a,
                                   VkImage *image) {
    (void)device, (void)a;
    got_format = info->format;
    *image = (VkImage)atomic_fetch_add(&handles, 1);
    return VK_SUCCESS;
}

void minato_host_vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *a) {
    (void)device, (void)image, (void)a;
}

VkResult minato_host_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                      const VkAllocationCallbacks *a, VkDeviceMemory *memory) {
    (void)device, (void)a;
    const struct minato_hardware_buffer *named = info->pNext;
    got_buffer = named != NULL && named->sType == MINATO_STRUCTURE_TYPE_HARDWARE_BUFFER ? named->buffer : 0;
    got_dedicated = VK_NULL_HANDLE;
    got_import = got_export = 0;
    for (const VkBaseInStructure *next = info->pNext; next != NULL; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            got_dedicated = ((const VkMemoryDedicatedAllocateInfo *)next)->image;
        }
        got_import |= next->sType == VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        got_export |= next->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    }
    if (allocation_answer != VK_SUCCESS) {
        return allocation_answer;
    }
    *memory = (VkDeviceMemory)atomic_fetch_add(&handles, 1);
    return VK_SUCCESS;
}

void minato_host_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *a) {
    (void)device, (void)memory, (void)a;
}

VkResult minato_host_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physical,
                                                               const VkPhysicalDeviceImageFormatInfo2 *info,
                                                               VkImageFormatProperties2 *properties) {
    (void)physical, (void)info;
    properties->imageFormatProperties.maxMipLevels = 12;
    return VK_SUCCESS;
}

void vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *requirements) {
    (void)device, (void)image;
    *requirements = (VkMemoryRequirements){256, 256, 0x3};
}

void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties *memory) {
    (void)physical;
    memset(memory, 0, sizeof *memory);
    memory->memoryTypeCount = 2;
    memory->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}

void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2 *properties) {
    (void)physical, (void)format;
    properties->formatProperties.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}

void minato_host_vkCmdPipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags source,
                                      VkPipelineStageFlags destination, VkDependencyFlags dependencies,
                                      uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
                                      const VkBufferMemoryBarrier *buffers, uint32_t image_count,
                                      const VkImageMemoryBarrier *images) {
    (void)commands, (void)source, (void)destination, (void)dependencies, (void)memory_count, (void)memory;
    got_barriers = images;
    got_families[0] = images[0].srcQueueFamilyIndex;
    got_families[1] = images[0].dstQueueFamilyIndex;
    got_families[2] = image_count > 1 ? images[1].srcQueueFamilyIndex : 0;
    got_families[3] = buffer_count > 0 ? buffers[0].dstQueueFamilyIndex : 0;
}

void minato_host_vkCmdWaitEvents(VkCommandBuffer commands, uint32_t count, const VkEvent *events,
                                 VkPipelineStageFlags source, VkPipelineStageFlags destination, uint32_t memory_count,
                                 const VkMemoryBarrier *memory, uint32_t buffer_count,
                                 const VkBufferMemoryBarrier *buffers, uint32_t image_count,
                                 const VkImageMemoryBarrier *images) {
    minato_host_vkCmdPipelineBarrier(commands, source, destination, 0, memory_count, memory, buffer_count, buffers,
                                     image_count, images);
    (void)count, (void)events;
}

static void dependency_got(const VkDependencyInfo *info, uint32_t count) {
    got_barriers = info;
    got_families[4] = info[count - 1].pImageMemoryBarriers[0].srcQueueFamilyIndex;
    got_families[5] = info[count - 1].pImageMemoryBarriers[0].dstQueueFamilyIndex;
    got_families[6] = info[count - 1].pBufferMemoryBarriers[0].srcQueueFamilyIndex;
}

void minato_host_vkCmdPipelineBarrier2(VkCommandBuffer commands, const VkDependencyInfo *info) {
    (void)commands;
    dependency_got(info, 1);
}

void minato_host_vkCmdWaitEvents2(VkCommandBuffer commands, uint32_t count, const VkEvent *events,
                                  const VkDependencyInfo *infos) {
    (void)commands, (void)events;
    dependency_got(infos, count);
}

void minato_host_vkCmdSetEvent2(VkCommandBuffer commands, VkEvent event, const VkDependencyInfo *info) {
    (void)commands, (void)event;
    dependency_got(info, 1);
}

static const VkDevice device = (VkDevice)(uintptr_t)0xde71ce;

static AHardwareBuffer *buffer_of(uint32_t width) {
    AHardwareBuffer_Desc description = {.width = width, .height = 8, .layers = 1,
                                        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE};
    AHardwareBuffer *buffer = NULL;
    AHardwareBuffer_allocate(&description, &buffer);
    return buffer;
}

static VkImage image_of(void) {
    const VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
                                    .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .extent = {8, 8, 1}, .mipLevels = 1, .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImage image = VK_NULL_HANDLE;
    vkCreateImage(device, &info, NULL, &image);
    return image;
}

static VkResult import(AHardwareBuffer *buffer, VkImage image, VkDeviceMemory *memory) {
    const VkImportAndroidHardwareBufferInfoANDROID imported = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = buffer};
    const VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imported, .image = image};
    const VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated,
                                       .allocationSize = 256};
    return vkAllocateMemory(device, &info, NULL, memory);
}

static void *churn(void *argument) {
    (void)argument;
    for (int round = 0; round < 200; round++) {
        AHardwareBuffer *buffer = buffer_of(8);
        const VkImage image = image_of();
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (import(buffer, image, &memory) != VK_SUCCESS) {
            atomic_fetch_add((atomic_int *)&failures, 1);
        }
        AHardwareBuffer_release(buffer);
        AHardwareBuffer *got = NULL;
        const VkMemoryGetAndroidHardwareBufferInfoANDROID info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .memory = memory};
        if (vkGetMemoryAndroidHardwareBufferANDROID(device, &info, &got) == VK_SUCCESS) {
            AHardwareBuffer_release(got);
        }
        vkFreeMemory(device, memory, NULL);
        vkDestroyImage(device, image, NULL);
    }
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct minato_queues *queues = calloc(1, sizeof *queues);
    queues->physical = (VkPhysicalDevice)(uintptr_t)0x9e;
    minato_remember(device, 0, queues);

    // The import.
    AHardwareBuffer *buffer = buffer_of(8);
    VkImage image = image_of();
    check(got_format == VK_FORMAT_R8G8B8A8_UNORM, "an image of the handle type reaches MoltenVK");
    VkDeviceMemory memory = VK_NULL_HANDLE;
    check(import(buffer, image, &memory) == VK_SUCCESS, "the import");
    check(got_buffer == buffer->id && got_dedicated == image && !got_import && !got_export,
          "graphics/host gets the buffer's ID first, the dedicated image, and no import");
    check(atomic_load(&buffer->references) == 2, "the memory holds a reference");
    VkDeviceMemory second = VK_NULL_HANDLE;
    AHardwareBuffer *other = buffer_of(8);
    check(import(other, image, &second) == VK_ERROR_INVALID_EXTERNAL_HANDLE &&
              atomic_load(&other->references) == 1,
          "a second buffer for the image is refused without a reference");
    AHardwareBuffer_release(other);
    AHardwareBuffer_release(buffer);
    check(atomic_load(&buffers_alive) == 1, "the app's release leaves the buffer to the memory");
    vkFreeMemory(device, memory, NULL);
    check(atomic_load(&buffers_alive) == 0, "vkFreeMemory gives the memory's reference back");
    vkDestroyImage(device, image, NULL);

    // The refusals.
    buffer = buffer_of(16);
    image = image_of();
    check(import(buffer, image, &memory) == VK_ERROR_INVALID_EXTERNAL_HANDLE &&
              atomic_load(&buffer->references) == 1,
          "a buffer of another size is refused without a reference");
    AHardwareBuffer_release(buffer);
    buffer = buffer_of(8);
    allocation_answer = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    check(import(buffer, image, &memory) == VK_ERROR_OUT_OF_DEVICE_MEMORY && atomic_load(&buffer->references) == 1,
          "MoltenVK's failure keeps no reference");
    allocation_answer = VK_SUCCESS;
    check(import(buffer, image, &memory) == VK_SUCCESS, "the import after it");
    AHardwareBuffer_release(buffer);
    vkFreeMemory(device, memory, NULL);
    vkDestroyImage(device, image, NULL);
    check(atomic_load(&buffers_alive) == 0, "no buffer left");

    // The export.
    image = image_of();
    const VkExportMemoryAllocateInfo exported = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    const VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &exported, .image = image};
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated};
    check(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS && !got_export && got_buffer != 0,
          "the export allocates a buffer and imports it");
    const VkMemoryGetAndroidHardwareBufferInfoANDROID get = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .memory = memory};
    AHardwareBuffer *first = NULL, *again = NULL;
    vkGetMemoryAndroidHardwareBufferANDROID(device, &get, &first);
    vkGetMemoryAndroidHardwareBufferANDROID(device, &get, &again);
    check(first != NULL && first == again && atomic_load(&first->references) == 3 &&
              first->description.width == 8 &&
              first->description.usage == AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
          "each get gives one more reference of the image's buffer");
    AHardwareBuffer_release(first);
    AHardwareBuffer_release(again);
    vkFreeMemory(device, memory, NULL);
    vkDestroyImage(device, image, NULL);
    check(atomic_load(&buffers_alive) == 0, "the export's buffer goes with the memory");

    // An external format libvulkan handed out.
    const VkExternalFormatANDROID named = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
                                           .externalFormat = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM};
    const VkImageCreateInfo undefined = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &named,
                                         .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_UNDEFINED,
                                         .extent = {8, 8, 1}, .mipLevels = 1, .arrayLayers = 1,
                                         .samples = VK_SAMPLE_COUNT_1_BIT, .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    got_format = VK_FORMAT_UNDEFINED;
    check(vkCreateImage(device, &undefined, NULL, &image) == VK_SUCCESS && got_format == VK_FORMAT_R8G8B8A8_UNORM,
          "an external format of libvulkan's reaches MoltenVK as its VkFormat");
    vkDestroyImage(device, image, NULL);

    // The barriers.
    const VkCommandBuffer commands = (VkCommandBuffer)(uintptr_t)0xc0;
    const VkImageMemoryBarrier images[] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
         .dstQueueFamilyIndex = 0},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcQueueFamilyIndex = 0, .dstQueueFamilyIndex = 0}};
    const VkBufferMemoryBarrier buffers[] = {{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                              .srcQueueFamilyIndex = 0,
                                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL}};
    vkCmdPipelineBarrier(commands, 0, 0, 0, 0, NULL, 1, buffers, 2, images);
    check(got_families[0] == VK_QUEUE_FAMILY_IGNORED && got_families[1] == VK_QUEUE_FAMILY_IGNORED &&
              got_families[2] == 0 && got_families[3] == VK_QUEUE_FAMILY_IGNORED &&
              images[0].srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT,
          "vkCmdPipelineBarrier: the foreign and the external family ignored, the app's barriers alone");
    vkCmdWaitEvents(commands, 0, NULL, 0, 0, 0, NULL, 0, NULL, 1, images + 1);
    check(got_barriers == images + 1, "vkCmdWaitEvents: barriers without them as the app's own");
    const VkImageMemoryBarrier2 images2[] = {{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                              .srcQueueFamilyIndex = 0,
                                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT}};
    const VkBufferMemoryBarrier2 buffers2[] = {{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                .srcQueueFamilyIndex = 1, .dstQueueFamilyIndex = 1}};
    const VkDependencyInfo infos[] = {
        {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = buffers2,
         .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = images2},
        {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = buffers2,
         .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = images2}};
    vkCmdWaitEvents2(commands, 2, NULL, infos);
    check(got_families[4] == VK_QUEUE_FAMILY_IGNORED && got_families[5] == VK_QUEUE_FAMILY_IGNORED &&
              got_families[6] == 1 && images2[0].dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT,
          "vkCmdWaitEvents2: the foreign family ignored in each info, the other barrier's families kept");
    vkCmdPipelineBarrier2(commands, &infos[0]);
    check(got_barriers != infos, "vkCmdPipelineBarrier2: a copy");
    vkCmdSetEvent2(commands, VK_NULL_HANDLE, &infos[1]);
    check(got_families[5] == VK_QUEUE_FAMILY_IGNORED, "vkCmdSetEvent2: the foreign family ignored");

    // Threads at once.
    pthread_t threads[6];
    for (int index = 0; index < 6; index++) {
        pthread_create(&threads[index], NULL, churn, NULL);
    }
    for (int index = 0; index < 6; index++) {
        pthread_join(threads[index], NULL);
    }
    check(atomic_load(&buffers_alive) == 0, "threads that import, get, free and destroy at once free every buffer");

    minato_forget(device);
    free(queues);
    printf("%s\n", failures == 0 ? "done" : "failed");
    return failures == 0 ? 0 : 1;
}
