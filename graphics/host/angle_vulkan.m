/* graphics/host's functions in front of MoltenVK's for ANGLE's Vulkan backend, the ES 3.1 path (ADR-0044, decisions
   2 to 4); see minato_graphics.h. ANGLE takes vkGetInstanceProcAddr from here (patch 0009) and reaches MoltenVK only
   through it, on the guest threads that call libEGL and libGLES:
   - vkCreateInstance chains MoltenVK's MVK_CONFIG_USE_METAL_PRIVATE_API through VK_EXT_layer_settings, so that
     ANGLE's instance, and no other, gets the last vertex and OpenGL mode (decision 2);
   - vkCreateDevice adds VK_EXT_metal_objects and keeps the device's MTLCommandQueues for "scheduled", as vulkan.m
     keeps libvulkan's, and vkDestroyDevice lets them go;
   - the submits, acquires and presents count themselves in vulkan.m's count, which "scheduled" waits for. In the
     background each waits for the foreground itself and then goes on, as the commit gate does for ANGLE's Metal
     backend (patch 0003): ANGLE knows nothing of libvulkan's background result;
   - the acquires and presents answer VK_SUBOPTIMAL_KHR as VK_SUCCESS, as libvulkan's swapchain.c does: MoltenVK
     answers it whenever a swapchain's extent differs from its layer's bounds times their scale, which a fixed size
     (patch 0008) always may, and ANGLE would make its swapchain again for every frame;
   - vkCreateSwapchainKHR counts ANGLE's swapchains for the tests.
   MoltenVK's functions come from the library the "vulkan" provider names, opened once the session chose the
   Vulkan backend. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
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
    static const VkBool32 on = VK_TRUE;
    const VkLayerSettingEXT setting = {.pLayerName = "MoltenVK", .pSettingName = "MVK_CONFIG_USE_METAL_PRIVATE_API",
                                       .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT, .valueCount = 1, .pValues = &on};
    const VkLayerSettingsCreateInfoEXT settings = {.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                                                   .pNext = info->pNext, .settingCount = 1, .pSettings = &setting};
    const char *names[info->enabledExtensionCount + 1];
    VkInstanceCreateInfo ours = *info;
    ours.pNext = &settings;
    ours.enabledExtensionCount = with_name(info->ppEnabledExtensionNames, info->enabledExtensionCount,
                                           VK_EXT_LAYER_SETTINGS_EXTENSION_NAME, names);
    ours.ppEnabledExtensionNames = names;
    return create_instance(&ours, allocator, instance);
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
    return VK_SUCCESS;
}

static void device_destroyed(VkDevice device, const VkAllocationCallbacks *allocator) {
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
