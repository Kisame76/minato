/* libvulkan's queue path for MinatoBench (queue_path.h). The scene calls these on its render thread only, so the
   state is plain; the submit's copies follow libvulkan's SUBMIT_WITH_SIGNAL and add_signal for a batch without a
   chain, and the slices its minato_sliced. */
#include "queue_path.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../scene.h"

#define SLICE 10000000u /* ns, libvulkan's */
#define QUEUES 16

static PFN_vkGetInstanceProcAddr moltenvk_instance;
static PFN_vkGetDeviceProcAddr moltenvk_device;
static PFN_vkCreateDevice create_device;
static PFN_vkDestroyDevice destroy_device;
static PFN_vkGetDeviceQueue get_queue;
static PFN_vkCreateSemaphore create_semaphore;
static PFN_vkDestroySemaphore destroy_semaphore;
static PFN_vkGetSemaphoreCounterValue counter_value;
static PFN_vkQueueSubmit queue_submit;
static PFN_vkWaitForFences wait_for_fences;
static PFN_vkAcquireNextImageKHR acquire_next_image;

/* The device's queues, each with its timeline at `last` for its last submit that succeeded. */
struct queue {
    VkQueue queue;
    VkSemaphore timeline;
    uint64_t last;
};
static struct {
    VkDevice device;
    uint32_t count;
    struct queue queue[QUEUES];
    uint64_t submits, waits, slices;  // the waits are fence waits and acquires, in MoltenVK's in slices
} ours;

static struct queue *find(VkQueue queue) {
    for (uint32_t index = 0; index < ours.count; ++index) {
        if (ours.queue[index].queue == queue) return &ours.queue[index];
    }
    return NULL;
}

static VkResult submitted(VkQueue queue, uint32_t count, const VkSubmitInfo *batches, VkFence fence) {
    struct queue *own = find(queue);
    if (own == NULL || (count == 0 && fence == VK_NULL_HANDLE)) return queue_submit(queue, count, batches, fence);
    const int alone = count == 0 || batches[count - 1].pNext != NULL;
    const uint32_t total = alone ? count + 1 : count;
    const uint32_t old = alone ? 0 : batches[count - 1].signalSemaphoreCount;
    VkSubmitInfo *copies = malloc(total * sizeof *copies);
    VkSemaphore *semaphores = malloc((old + 1) * (sizeof(VkSemaphore) + sizeof(uint64_t)));
    if (copies == NULL || semaphores == NULL) {
        free(copies);
        free(semaphores);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (count > 0) memcpy(copies, batches, count * sizeof *copies);
    VkSubmitInfo *last = &copies[total - 1];
    if (alone) *last = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    uint64_t *values = (uint64_t *)(semaphores + old + 1);
    for (uint32_t index = 0; index < old; ++index) {
        semaphores[index] = last->pSignalSemaphores[index];
        values[index] = 0;
    }
    const uint64_t value = own->last + 1;
    semaphores[old] = own->timeline;
    values[old] = value;
    const VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                    .signalSemaphoreValueCount = old + 1,
                                                    .pSignalSemaphoreValues = values};
    last->pNext = &timeline;
    last->signalSemaphoreCount = old + 1;
    last->pSignalSemaphores = semaphores;
    const VkResult result = queue_submit(queue, total, copies, fence);
    if (result == VK_SUCCESS) {
        own->last = value;
        ++ours.submits;
    }
    free(semaphores);
    free(copies);
    return result;
}

static uint64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

/* A wait in slices of at most 10 ms until it answers other than VK_TIMEOUT or `timeout` ns passed. */
static VkResult sliced(VkResult (*wait)(const void *arguments, uint64_t timeout), const void *arguments,
                       uint64_t timeout) {
    if (timeout <= SLICE) return wait(arguments, timeout);
    const uint64_t start = now();
    const uint64_t deadline = timeout > UINT64_MAX - start ? UINT64_MAX : start + timeout;
    for (uint64_t at = start; at < deadline; at = now()) {
        const VkResult result = wait(arguments, deadline - at < SLICE ? deadline - at : SLICE);
        if (result != VK_TIMEOUT) return result;
    }
    return VK_TIMEOUT;
}

struct fences {
    VkDevice device;
    uint32_t count;
    const VkFence *fences;
    VkBool32 all;
};

static VkResult wait_fences(const void *arguments, uint64_t timeout) {
    const struct fences *fences = arguments;
    ++ours.slices;
    return wait_for_fences(fences->device, fences->count, fences->fences, fences->all, timeout);
}

static VkResult waited(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
    const struct fences arguments = {device, count, fences, all};
    ++ours.waits;
    return sliced(wait_fences, &arguments, timeout);
}

struct acquisition {
    VkDevice device;
    VkSwapchainKHR swapchain;
    VkSemaphore semaphore;
    VkFence fence;
    uint32_t *index;
};

static VkResult acquire_slice(const void *arguments, uint64_t timeout) {
    const struct acquisition *acquisition = arguments;
    ++ours.slices;
    return acquire_next_image(acquisition->device, acquisition->swapchain, timeout, acquisition->semaphore,
                              acquisition->fence, acquisition->index);
}

static VkResult acquired(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                         VkFence fence, uint32_t *index) {
    const struct acquisition arguments = {device, swapchain, semaphore, fence, index};
    ++ours.waits;
    return sliced(acquire_slice, &arguments, timeout);
}

static void destroyed(VkDevice device, const VkAllocationCallbacks *allocator) {
    if (device != VK_NULL_HANDLE && device == ours.device) {
        uint64_t values = 0;
        for (uint32_t index = 0; index < ours.count; ++index) {
            uint64_t value = 0;
            if (counter_value(device, ours.queue[index].timeline, &value) == VK_SUCCESS) values += value;
            destroy_semaphore(device, ours.queue[index].timeline, NULL);
        }
        bench_log("queue path libvulkan: %llu submits, the timeline at %llu; %llu waits in %llu slices",
                  (unsigned long long)ours.submits, (unsigned long long)values, (unsigned long long)ours.waits,
                  (unsigned long long)ours.slices);
        memset(&ours, 0, sizeof ours);
    }
    if (destroy_device != NULL) destroy_device(device, allocator);
}

static PFN_vkVoidFunction device_function(VkDevice device, const char *name);

/* The device functions this file puts in front of MoltenVK's, else NULL. */
static PFN_vkVoidFunction in_front(const char *name) {
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)device_function;
    if (strcmp(name, "vkDestroyDevice") == 0) return (PFN_vkVoidFunction)destroyed;
    if (strcmp(name, "vkQueueSubmit") == 0) return (PFN_vkVoidFunction)submitted;
    if (strcmp(name, "vkWaitForFences") == 0) return (PFN_vkVoidFunction)waited;
    if (strcmp(name, "vkAcquireNextImageKHR") == 0) return (PFN_vkVoidFunction)acquired;
    return NULL;
}

static PFN_vkVoidFunction device_function(VkDevice device, const char *name) {
    const PFN_vkVoidFunction own = in_front(name);
    return own != NULL ? own : moltenvk_device(device, name);
}

/* The features libvulkan enables itself, which the app's chain must not hold as well. */
static int has_timeline_features(const void *chain) {
    for (const VkBaseInStructure *next = chain; next != NULL; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
            next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            return 1;
        }
    }
    return 0;
}

static VkResult created(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                        const VkAllocationCallbacks *allocator, VkDevice *device) {
    uint32_t queues = 0;
    for (uint32_t family = 0; family < info->queueCreateInfoCount; ++family) {
        queues += info->pQueueCreateInfos[family].queueCount;
    }
    if (ours.device != VK_NULL_HANDLE || queues > QUEUES || has_timeline_features(info->pNext)) {
        bench_log("queue path libvulkan: no second device, more than %d queues or timeline features of the app's",
                  QUEUES);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const char **names = malloc((info->enabledExtensionCount + 1) * sizeof *names);
    if (names == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (info->enabledExtensionCount > 0) {
        memcpy(names, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof *names);
    }
    names[info->enabledExtensionCount] = "VK_EXT_metal_objects";
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext = (void *)info->pNext,
        .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo ours_info = *info;
    ours_info.pNext = &timeline;
    ours_info.enabledExtensionCount = info->enabledExtensionCount + 1;
    ours_info.ppEnabledExtensionNames = names;
    VkResult result = create_device(physical, &ours_info, allocator, device);
    free(names);
    if (result != VK_SUCCESS) return result;

#define MOLTENVK(variable, type, name) variable = (type)moltenvk_device(*device, name)
    MOLTENVK(destroy_device, PFN_vkDestroyDevice, "vkDestroyDevice");
    MOLTENVK(get_queue, PFN_vkGetDeviceQueue, "vkGetDeviceQueue");
    MOLTENVK(create_semaphore, PFN_vkCreateSemaphore, "vkCreateSemaphore");
    MOLTENVK(destroy_semaphore, PFN_vkDestroySemaphore, "vkDestroySemaphore");
    MOLTENVK(counter_value, PFN_vkGetSemaphoreCounterValue, "vkGetSemaphoreCounterValue");
    MOLTENVK(queue_submit, PFN_vkQueueSubmit, "vkQueueSubmit");
    MOLTENVK(wait_for_fences, PFN_vkWaitForFences, "vkWaitForFences");
    MOLTENVK(acquire_next_image, PFN_vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
#undef MOLTENVK
    ours.device = *device;
    const VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    for (uint32_t family = 0; family < info->queueCreateInfoCount && result == VK_SUCCESS; ++family) {
        const VkDeviceQueueCreateInfo *made = &info->pQueueCreateInfos[family];
        for (uint32_t index = 0; index < made->queueCount && result == VK_SUCCESS; ++index) {
            struct queue *queue = &ours.queue[ours.count];
            get_queue(*device, made->queueFamilyIndex, index, &queue->queue);
            result = create_semaphore(*device, &semaphore, NULL, &queue->timeline);
            if (result == VK_SUCCESS) ++ours.count;
        }
    }
    if (result != VK_SUCCESS) {
        destroyed(*device, allocator);
        *device = VK_NULL_HANDLE;
    }
    return result;
}

static PFN_vkVoidFunction instance_function(VkInstance instance, const char *name) {
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)instance_function;
    if (instance == VK_NULL_HANDLE) return moltenvk_instance(instance, name);
    const int creates = strcmp(name, "vkCreateDevice") == 0;
    const PFN_vkVoidFunction own = creates ? (PFN_vkVoidFunction)created : in_front(name);
    if (own == NULL) return moltenvk_instance(instance, name);
    create_device = (PFN_vkCreateDevice)moltenvk_instance(instance, "vkCreateDevice");
    moltenvk_device = (PFN_vkGetDeviceProcAddr)moltenvk_instance(instance, "vkGetDeviceProcAddr");
    return create_device != NULL && moltenvk_device != NULL ? own : NULL;
}

PFN_vkGetInstanceProcAddr bench_queue_path(PFN_vkGetInstanceProcAddr moltenvk) {
    moltenvk_instance = moltenvk;
    return moltenvk != NULL ? instance_function : NULL;
}
