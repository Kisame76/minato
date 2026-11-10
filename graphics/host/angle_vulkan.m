/* graphics/host's functions in front of MoltenVK's for ANGLE's Vulkan backend, the ES 3.1 path (ADR-0044, decisions
   2 to 4); see minato_graphics.h. ANGLE takes vkGetInstanceProcAddr from here (patch 0009) and reaches MoltenVK only
   through it, on the guest threads that call libEGL and libGLES:
   - vkCreateInstance chains MoltenVK's MVK_CONFIG_USE_METAL_PRIVATE_API through VK_EXT_layer_settings, so that
     ANGLE's instance, and no other, gets the last vertex and OpenGL mode (decision 2), and
     MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS off (the ES 3.1 path's speed): without Metal argument buffers MoltenVK binds
     the uniforms, textures and samplers straight to each stage, as ANGLE's Metal backend does, and encodes none of
     its barrier fences, which with argument buffers hold every render encoder's vertex and fragment stages behind
     the encoders before it [measured on the Mac: 16 waits and 4 fence-only blit encoders a frame of the bench's heavy
     load, none without, its frame 1000 the same]. ANGLE's ES 3.1 needs fit the limits without them; libvulkan's
     instances (the apps' own Vulkan) keep MoltenVK's default;
   - vkCreateDevice adds VK_EXT_metal_objects and keeps the device's MTLCommandQueues for "scheduled", as vulkan.m
     keeps libvulkan's, and vkDestroyDevice lets them go;
   - the submits, acquires and presents count themselves in vulkan.m's count, which "scheduled" waits for. In the
     background each waits for the foreground itself and then goes on, as the commit gate does for ANGLE's Metal
     backend (patch 0003): ANGLE knows nothing of libvulkan's background result;
   - the acquires and presents answer VK_SUBOPTIMAL_KHR as VK_SUCCESS, as libvulkan's swapchain.c does: MoltenVK
     answers it whenever a swapchain's extent differs from its layer's bounds times their scale, which a fixed size
     (patch 0008) always may, and ANGLE would make its swapchain again for every frame;
   - vkCreateSwapchainKHR counts ANGLE's swapchains for the tests;
   - ANGLE's instance and device are kept for the EGL images of the guest's hardware buffers (ADR-0058 b), image.m):
     a VkImage of ANGLE's device that MoltenVK makes over the buffer's IOSurface (VK_EXT_metal_objects' import), bound
     to dedicated memory, which ANGLE takes as an EGL_VULKAN_IMAGE_ANGLE image.
   MoltenVK's functions come from the library the "vulkan" provider names, opened once the session chose the
   Vulkan backend. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include "APKRuntime/apkrt.h"
#include "angle_host.h"
#include "minato_graphics.h"
#include "vulkan_host.h"

/* MoltenVK's functions that graphics/host's stand in front of, and those it uses. */
static PFN_vkGetInstanceProcAddr instance_function;
static PFN_vkGetDeviceProcAddr device_function;
static PFN_vkCreateInstance create_instance;
static PFN_vkCreateDevice create_device;
static PFN_vkDestroyDevice destroy_device;
static PFN_vkGetDeviceQueue2 get_queue;
static PFN_vkQueueSubmit queue_submit;
static PFN_vkQueueSubmit2 queue_submit2;
static PFN_vkQueueBindSparse queue_bind_sparse;
static PFN_vkAcquireNextImageKHR acquire_image;
static PFN_vkAcquireNextImage2KHR acquire_image2;
static PFN_vkQueuePresentKHR queue_present;
static PFN_vkCreateSwapchainKHR create_swapchain;

static atomic_int vulkan_backend;
static atomic_uint swapchains;

/* ANGLE's instance, physical device and device, the last made; images are made on that device. */
static pthread_mutex_t angle_lock = PTHREAD_MUTEX_INITIALIZER;
static VkInstance angle_instance;
static VkPhysicalDevice angle_physical;
static VkDevice angle_device;

/* Counts a call that makes MoltenVK commit; in the background it waits for the foreground first. */
static void enter(void) {
    while (!minato_graphics_vulkan_enter()) {
        apkrt_wait_foreground();
    }
}

static VkResult not_suboptimal(VkResult result) {
    return result == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : result;
}

/* A copy of `names` with `name` once among them, in `room` of `count` + 1 entries. */
static uint32_t with_name(const char *const *names, uint32_t count, const char *name, const char **room) {
    uint32_t total = 0;
    int found = 0;
    for (uint32_t index = 0; index < count; index++) {
        found = found || strcmp(names[index], name) == 0;
        room[total++] = names[index];
    }
    if (!found) {
        room[total++] = name;
    }
    return total;
}

static VkResult instance_created(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                 VkInstance *instance) {
    static const VkBool32 on = VK_TRUE, off = VK_FALSE;
    const VkLayerSettingEXT setting[2] = {
        {.pLayerName = "MoltenVK", .pSettingName = "MVK_CONFIG_USE_METAL_PRIVATE_API",
         .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT, .valueCount = 1, .pValues = &on},
        {.pLayerName = "MoltenVK", .pSettingName = "MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS",
         .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT, .valueCount = 1, .pValues = &off},
    };
    const VkLayerSettingsCreateInfoEXT settings = {.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                                                   .pNext = info->pNext, .settingCount = 2, .pSettings = setting};
    const char *names[info->enabledExtensionCount + 1];
    VkInstanceCreateInfo ours = *info;
    ours.pNext = &settings;
    ours.enabledExtensionCount = with_name(info->ppEnabledExtensionNames, info->enabledExtensionCount,
                                           VK_EXT_LAYER_SETTINGS_EXTENSION_NAME, names);
    ours.ppEnabledExtensionNames = names;
    const VkResult result = create_instance(&ours, allocator, instance);
    if (result == VK_SUCCESS) {
        pthread_mutex_lock(&angle_lock);
        angle_instance = *instance;
        pthread_mutex_unlock(&angle_lock);
    }
    return result;
}

static VkResult device_created(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                               const VkAllocationCallbacks *allocator, VkDevice *device) {
    const char *names[info->enabledExtensionCount + 1];
    VkDeviceCreateInfo ours = *info;
    ours.enabledExtensionCount = with_name(info->ppEnabledExtensionNames, info->enabledExtensionCount,
                                           VK_EXT_METAL_OBJECTS_EXTENSION_NAME, names);
    ours.ppEnabledExtensionNames = names;
    const VkResult result = create_device(physical, &ours, allocator, device);
    if (result != VK_SUCCESS) {
        return result;
    }
    if (minato_graphics_vulkan_keep_device(*device, info, get_queue, device_function) != 0) {
        destroy_device(*device, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    pthread_mutex_lock(&angle_lock);
    angle_physical = physical;
    angle_device = *device;
    pthread_mutex_unlock(&angle_lock);
    return VK_SUCCESS;
}

static void device_destroyed(VkDevice device, const VkAllocationCallbacks *allocator) {
    pthread_mutex_lock(&angle_lock);
    if (angle_device == device) {
        angle_device = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&angle_lock);
    minato_graphics_vulkan_forget_device(device);
    destroy_device(device, allocator);
}

static VkResult submitted(VkQueue queue, uint32_t count, const VkSubmitInfo *batches, VkFence fence) {
    enter();
    const VkResult result = queue_submit(queue, count, batches, fence);
    minato_graphics_vulkan_leave();
    return result;
}

static VkResult submitted2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *batches, VkFence fence) {
    enter();
    const VkResult result = queue_submit2(queue, count, batches, fence);
    minato_graphics_vulkan_leave();
    return result;
}

static VkResult bound_sparse(VkQueue queue, uint32_t count, const VkBindSparseInfo *batches, VkFence fence) {
    enter();
    const VkResult result = queue_bind_sparse(queue, count, batches, fence);
    minato_graphics_vulkan_leave();
    return result;
}

static VkResult acquired(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                         VkFence fence, uint32_t *index) {
    enter();
    const VkResult result = acquire_image(device, swapchain, timeout, semaphore, fence, index);
    minato_graphics_vulkan_leave();
    return not_suboptimal(result);
}

static VkResult acquired2(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *index) {
    enter();
    const VkResult result = acquire_image2(device, info, index);
    minato_graphics_vulkan_leave();
    return not_suboptimal(result);
}

static VkResult presented(VkQueue queue, const VkPresentInfoKHR *info) {
    enter();
    const VkResult result = queue_present(queue, info);
    minato_graphics_vulkan_leave();
    for (uint32_t index = 0; info->pResults != NULL && index < info->swapchainCount; index++) {
        info->pResults[index] = not_suboptimal(info->pResults[index]);
    }
    return not_suboptimal(result);
}

static VkResult swapchain_created(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                  const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
    const VkResult result = create_swapchain(device, info, allocator, swapchain);
    if (result == VK_SUCCESS) {
        atomic_fetch_add(&swapchains, 1);
    }
    return result;
}

static PFN_vkVoidFunction instance_function_given(VkInstance instance, const char *name);
static PFN_vkVoidFunction device_function_given(VkDevice device, const char *name);

/* graphics/host's function for a name, when MoltenVK has the name's function too; NULL for the others. */
static PFN_vkVoidFunction ours(const char *name) {
    static const struct {
        const char *name;
        PFN_vkVoidFunction *moltenvk;
        PFN_vkVoidFunction ours;
    } functions[] = {
        {"vkGetInstanceProcAddr", (PFN_vkVoidFunction *)&instance_function,
         (PFN_vkVoidFunction)instance_function_given},
        {"vkGetDeviceProcAddr", (PFN_vkVoidFunction *)&device_function, (PFN_vkVoidFunction)device_function_given},
        {"vkCreateInstance", (PFN_vkVoidFunction *)&create_instance, (PFN_vkVoidFunction)instance_created},
        {"vkCreateDevice", (PFN_vkVoidFunction *)&create_device, (PFN_vkVoidFunction)device_created},
        {"vkDestroyDevice", (PFN_vkVoidFunction *)&destroy_device, (PFN_vkVoidFunction)device_destroyed},
        {"vkQueueSubmit", (PFN_vkVoidFunction *)&queue_submit, (PFN_vkVoidFunction)submitted},
        {"vkQueueSubmit2", (PFN_vkVoidFunction *)&queue_submit2, (PFN_vkVoidFunction)submitted2},
        {"vkQueueSubmit2KHR", (PFN_vkVoidFunction *)&queue_submit2, (PFN_vkVoidFunction)submitted2},
        {"vkQueueBindSparse", (PFN_vkVoidFunction *)&queue_bind_sparse, (PFN_vkVoidFunction)bound_sparse},
        {"vkAcquireNextImageKHR", (PFN_vkVoidFunction *)&acquire_image, (PFN_vkVoidFunction)acquired},
        {"vkAcquireNextImage2KHR", (PFN_vkVoidFunction *)&acquire_image2, (PFN_vkVoidFunction)acquired2},
        {"vkQueuePresentKHR", (PFN_vkVoidFunction *)&queue_present, (PFN_vkVoidFunction)presented},
        {"vkCreateSwapchainKHR", (PFN_vkVoidFunction *)&create_swapchain, (PFN_vkVoidFunction)swapchain_created},
    };
    for (size_t index = 0; index < sizeof functions / sizeof functions[0]; index++) {
        if (strcmp(functions[index].name, name) == 0) {
            return *functions[index].moltenvk != NULL ? functions[index].ours : NULL;
        }
    }
    return NULL;
}

static PFN_vkVoidFunction instance_function_given(VkInstance instance, const char *name) {
    const PFN_vkVoidFunction function = ours(name);
    return function != NULL ? function : instance_function(instance, name);
}

static PFN_vkVoidFunction device_function_given(VkDevice device, const char *name) {
    const PFN_vkVoidFunction function = ours(name);
    return function != NULL ? function : device_function(device, name);
}

static void opening(void) {
    const char *path = minato_graphics_moltenvk_path();
    void *library = path[0] != '\0' ? dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST) : NULL;
    if (library == NULL) {
        fprintf(stderr, "minato graphics: no MoltenVK for ANGLE's Vulkan backend: %s\n",
                path[0] != '\0' ? dlerror() : "none named");
        return;
    }
#define FUNCTION(name, variable) variable = (__typeof__(variable))dlsym(library, #name);
    FUNCTION(vkGetInstanceProcAddr, instance_function)
    FUNCTION(vkGetDeviceProcAddr, device_function)
    FUNCTION(vkCreateInstance, create_instance)
    FUNCTION(vkCreateDevice, create_device)
    FUNCTION(vkDestroyDevice, destroy_device)
    FUNCTION(vkGetDeviceQueue2, get_queue)
    FUNCTION(vkQueueSubmit, queue_submit)
    FUNCTION(vkQueueSubmit2, queue_submit2)
    FUNCTION(vkQueueBindSparse, queue_bind_sparse)
    FUNCTION(vkAcquireNextImageKHR, acquire_image)
    FUNCTION(vkAcquireNextImage2KHR, acquire_image2)
    FUNCTION(vkQueuePresentKHR, queue_present)
    FUNCTION(vkCreateSwapchainKHR, create_swapchain)
#undef FUNCTION
}

/* MoltenVK's functions, once: 1 when it is there with the ones ANGLE cannot do without. */
static int moltenvk_opened(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, opening);
    return instance_function != NULL && device_function != NULL && create_instance != NULL && create_device != NULL &&
           destroy_device != NULL && get_queue != NULL;
}

int minato_graphics_set_angle_backend(int vulkan) {
    if (vulkan && !moltenvk_opened()) {
        return -1;
    }
    atomic_store(&vulkan_backend, vulkan ? 1 : 0);
    // ro.opengles.version follows the backend (ADR-0044 d)), so that reqGlEsVersion tells the app what its session
    // gives; the runtime takes only these two versions.
    apkrt_set_opengles_version(vulkan ? 0x30001 : 0x30000);
    return 0;
}

int minato_graphics_angle_vulkan_backend(void) {
    return atomic_load(&vulkan_backend);
}

void *minato_graphics_angle_vulkan_loader(void) {
    return moltenvk_opened() ? (void *)instance_function_given : NULL;
}

unsigned minato_graphics_angle_vulkan_swapchains(void) {
    return atomic_load(&swapchains);
}

/* An image of a hardware buffer on ANGLE's device: the VkImage, its memory and the create info ANGLE reads. */
struct minato_vulkan_image {
    VkDevice device;
    VkImage image;
    VkDeviceMemory memory;
    VkImageCreateInfo info;
};

static VkFormat vulkan_format(uint32_t format) {
    switch (format) {
        case 1:  // R8G8B8A8_UNORM
        case 2: return VK_FORMAT_R8G8B8A8_UNORM;  // R8G8B8X8_UNORM, its alpha as in memory (image.m)
        case 4: return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case 0x16: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 0x2b: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 0x38: return VK_FORMAT_R8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

/* The first memory type of `bits`, device-local where one is. */
static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits) {
    PFN_vkGetPhysicalDeviceMemoryProperties properties_of = (PFN_vkGetPhysicalDeviceMemoryProperties)instance_function(
        angle_instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties properties = {0};
    if (properties_of != NULL) {
        properties_of(physical, &properties);
    }
    uint32_t found = UINT32_MAX;
    for (uint32_t index = 0; index < properties.memoryTypeCount; index++) {
        if ((bits & (1u << index)) == 0) {
            continue;
        }
        if ((properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            return index;
        }
        found = found == UINT32_MAX ? index : found;
    }
    return found;
}

void minato_graphics_angle_vulkan_image_free(struct minato_vulkan_image *made) {
    if (made == NULL) {
        return;
    }
    PFN_vkDeviceWaitIdle wait_idle = (PFN_vkDeviceWaitIdle)device_function(made->device, "vkDeviceWaitIdle");
    PFN_vkDestroyImage destroy_image = (PFN_vkDestroyImage)device_function(made->device, "vkDestroyImage");
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)device_function(made->device, "vkFreeMemory");
    // ANGLE's destroy does not wait for its commands that still read the image.
    if (wait_idle != NULL) {
        wait_idle(made->device);
    }
    if (destroy_image != NULL && made->image != VK_NULL_HANDLE) {
        destroy_image(made->device, made->image, NULL);
    }
    if (free_memory != NULL && made->memory != VK_NULL_HANDLE) {
        free_memory(made->device, made->memory, NULL);
    }
    free(made);
}

struct minato_vulkan_image *minato_graphics_angle_vulkan_image(void *surface, uint32_t format, uint32_t width,
                                                               uint32_t height) {
    pthread_mutex_lock(&angle_lock);
    const VkDevice device = angle_device;
    const VkPhysicalDevice physical = angle_physical;
    pthread_mutex_unlock(&angle_lock);
    const VkFormat vk_format = vulkan_format(format);
    if (!moltenvk_opened() || device == VK_NULL_HANDLE || vk_format == VK_FORMAT_UNDEFINED) {
        return NULL;
    }
    PFN_vkCreateImage create_image = (PFN_vkCreateImage)device_function(device, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements requirements_of =
        (PFN_vkGetImageMemoryRequirements)device_function(device, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)device_function(device, "vkAllocateMemory");
    PFN_vkBindImageMemory bind_memory = (PFN_vkBindImageMemory)device_function(device, "vkBindImageMemory");
    struct minato_vulkan_image *made = calloc(1, sizeof *made);
    if (made == NULL || create_image == NULL || requirements_of == NULL || allocate_memory == NULL ||
        bind_memory == NULL) {
        free(made);
        return NULL;
    }
    made->device = device;
    made->info = (VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VkImportMetalIOSurfaceInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT,
                                                  .ioSurface = (IOSurfaceRef)surface};
    VkImageCreateInfo imported = made->info;
    imported.pNext = &import;
    VkMemoryRequirements requirements;
    if (create_image(device, &imported, NULL, &made->image) != VK_SUCCESS) {
        minato_graphics_angle_vulkan_image_free(made);
        return NULL;
    }
    requirements_of(device, made->image, &requirements);
    const VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                                     .image = made->image};
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .pNext = &dedicated,
                                             .allocationSize = requirements.size,
                                             .memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits)};
    if (allocation.memoryTypeIndex == UINT32_MAX ||
        allocate_memory(device, &allocation, NULL, &made->memory) != VK_SUCCESS ||
        bind_memory(device, made->image, made->memory, 0) != VK_SUCCESS) {
        minato_graphics_angle_vulkan_image_free(made);
        return NULL;
    }
    return made;
}

void *minato_graphics_angle_vulkan_image_buffer(struct minato_vulkan_image *made) {
    return &made->image;
}

const void *minato_graphics_angle_vulkan_image_info(const struct minato_vulkan_image *made) {
    return &made->info;
}
