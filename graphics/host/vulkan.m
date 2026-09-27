/* graphics/host's functions in front of MoltenVK's for libvulkan (ADR-0030, decisions 5, 12 and 13); see
   minato_graphics.h. The "vulkan" provider puts them into their slots of libvulkan's table, so they run as
   host code on the guest threads that call libvulkan:
   - the submits, acquires and presents count the calls in MoltenVK, until which "scheduled" waits, and
     back out in the background (host_protocol.h), where libvulkan waits at the window's gate;
   - vkCreateAndroidSurfaceKHR, which MoltenVK lacks, makes MoltenVK's surface on the layer of the window
     libvulkan names; a swapchain takes a window that is there, and Core Animation gets MoltenVK's changes
     to the layer, made on a thread without a run loop, when it is created or destroyed; presents are
     counted per window for the FPS label;
   - vkCreateDevice keeps each queue's MTLCommandQueue for "scheduled", and vkDestroyDevice lets them go;
   - the app's debug callbacks, from vkCreateInstance's chain or their create functions, reach MoltenVK as
     graphics/host's functions, which call the app's through apkrt_call_guest on guest threads only and log
     the message on any other thread (decision 5).
   Manual reference counting: the queues live in plain C structures. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "host_protocol.h"
#include "minato_graphics.h"
#include "vulkan_host.h"
#include "vulkan_table.h"

/* MoltenVK's functions that graphics/host's stand in front of, and three they use. */
static PFN_vkCreateInstance create_instance;
static PFN_vkDestroyInstance destroy_instance;
static PFN_vkCreateDevice create_device;
static PFN_vkDestroyDevice destroy_device;
static PFN_vkQueueSubmit queue_submit;
static PFN_vkQueueSubmit2 queue_submit2;
static PFN_vkQueueBindSparse queue_bind_sparse;
static PFN_vkAcquireNextImageKHR acquire_image;
static PFN_vkQueuePresentKHR queue_present;
static PFN_vkCreateSwapchainKHR create_swapchain;
static PFN_vkDestroySwapchainKHR destroy_swapchain;
static PFN_vkCreateDebugUtilsMessengerEXT create_messenger;
static PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger;
static PFN_vkCreateDebugReportCallbackEXT create_report;
static PFN_vkDestroyDebugReportCallbackEXT destroy_report;
static PFN_vkGetDeviceQueue2 get_queue;
static PFN_vkGetDeviceProcAddr device_function;
static PFN_vkGetInstanceProcAddr instance_function;

/* libvulkan's calls in MoltenVK that make it commit (decision 13): each counts itself, then reads the
   background flag and backs out if it is set; the app sets the flag, then "scheduled" reads the count. Both
   sides sequentially consistent, so either the call sees the flag or "scheduled" sees the call. */
static atomic_uint in_flight;

static int entered(void) {
    atomic_fetch_add(&in_flight, 1);
    if (apkrt_is_background()) {
        atomic_fetch_sub(&in_flight, 1);
        return 0;
    }
    return 1;
}

static void left(void) {
    atomic_fetch_sub(&in_flight, 1);
}

static VkResult submitted(VkQueue queue, uint32_t count, const VkSubmitInfo *batches, VkFence fence) {
    if (!entered()) {
        return MINATO_VK_BACKGROUND;
    }
    const VkResult result = queue_submit(queue, count, batches, fence);
    left();
    return result;
}

static VkResult submitted2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *batches, VkFence fence) {
    if (!entered()) {
        return MINATO_VK_BACKGROUND;
    }
    const VkResult result = queue_submit2(queue, count, batches, fence);
    left();
    return result;
}

static VkResult bound_sparse(VkQueue queue, uint32_t count, const VkBindSparseInfo *batches, VkFence fence) {
    if (!entered()) {
        return MINATO_VK_BACKGROUND;
    }
    const VkResult result = queue_bind_sparse(queue, count, batches, fence);
    left();
    return result;
}

static VkResult acquired(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                         VkFence fence, uint32_t *index) {
    if (!entered()) {
        return MINATO_VK_BACKGROUND;
    }
    const VkResult result = acquire_image(device, swapchain, timeout, semaphore, fence, index);
    left();
    return result;
}

/* The windows libvulkan names first in a chain (host_protocol.h). */
static const struct minato_windows *windows_of(const void *chain) {
    const struct minato_windows *windows = chain;
    return windows != NULL && windows->sType == MINATO_STRUCTURE_TYPE_WINDOWS ? windows : NULL;
}

static VkResult presented(VkQueue queue, const VkPresentInfoKHR *info) {
    if (!entered()) {
        return MINATO_VK_BACKGROUND;
    }
    const struct minato_windows *windows = windows_of(info->pNext);
    VkPresentInfoKHR ours = *info;
    ours.pNext = windows != NULL ? windows->pNext : info->pNext;
    const VkResult result = queue_present(queue, &ours);
    left();
    /* Each window's presents, for the FPS label. */
    for (uint32_t index = 0; windows != NULL && index < windows->count && index < info->swapchainCount; index++) {
        const VkResult own = info->pResults != NULL ? info->pResults[index] : result;
        if (own == VK_SUCCESS || own == VK_SUBOPTIMAL_KHR) {
            apkrt_window_presented(windows->windows[index]);
        }
    }
    return result;
}

/* In the slot of vkCreateAndroidSurfaceKHR, which MoltenVK lacks: MoltenVK's surface on the layer of the
   window libvulkan names. The layer comes retained for this call, and MoltenVK's surface retains it for its
   own life; a window that is gone answers as Android does when it cannot connect to one. */
static VkResult surface_created(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *info,
                                const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    const struct minato_windows *windows = windows_of(info->pNext);
    const PFN_vkCreateMetalSurfaceEXT create =
        (PFN_vkCreateMetalSurfaceEXT)instance_function(instance, "vkCreateMetalSurfaceEXT");
    if (windows == NULL || windows->count != 1 || create == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    CAMetalLayer *layer = (CAMetalLayer *)apkrt_window_layer(windows->windows[0]);
    if (layer == nil) {
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    const VkMetalSurfaceCreateInfoEXT metal = {.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                                               .pLayer = layer};
    const VkResult result = create(instance, &metal, allocator, surface);
    [layer release];
    return result;
}

/* A window that is gone takes no swapchain, as on Android. MoltenVK changes the layer on the calling
   thread, which has no run loop to commit it, so Core Animation gets the changes here. */
static VkResult swapchain_created(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                  const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
    const struct minato_windows *windows = windows_of(info->pNext);
    int32_t width, height;
    if (windows != NULL && windows->count == 1 && apkrt_window_size(windows->windows[0], &width, &height) != 0) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    VkSwapchainCreateInfoKHR ours = *info;
    ours.pNext = windows != NULL ? windows->pNext : info->pNext;
    const VkResult result = create_swapchain(device, &ours, allocator, swapchain);
    if (result == VK_SUCCESS) {
        [CATransaction flush];
    }
    return result;
}

static void swapchain_destroyed(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator) {
    destroy_swapchain(device, swapchain, allocator);
    [CATransaction flush];
}

/* A device libvulkan made, with its queues' MTLCommandQueues, retained. */
struct device {
    struct device *next;
    VkDevice device;
    uint32_t count;
    id<MTLCommandQueue> queues[];
};
static struct device *devices;
static pthread_mutex_t devices_lock = PTHREAD_MUTEX_INITIALIZER;

static VkResult device_created(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                               const VkAllocationCallbacks *allocator, VkDevice *device) {
    VkResult result = create_device(physical, info, allocator, device);
    if (result != VK_SUCCESS) {
        return result;
    }
    uint32_t count = 0;
    for (uint32_t family = 0; family < info->queueCreateInfoCount; family++) {
        count += info->pQueueCreateInfos[family].queueCount;
    }
    struct device *entry = calloc(1, sizeof *entry + count * sizeof entry->queues[0]);
    if (entry == NULL) {
        destroy_device(*device, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    entry->device = *device;
    const PFN_vkExportMetalObjectsEXT export_objects =
        (PFN_vkExportMetalObjectsEXT)device_function(*device, "vkExportMetalObjectsEXT");
    for (uint32_t family = 0; family < info->queueCreateInfoCount; family++) {
        const VkDeviceQueueCreateInfo *created = &info->pQueueCreateInfos[family];
        for (uint32_t index = 0; index < created->queueCount; index++) {
            const VkDeviceQueueInfo2 which = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .flags = created->flags,
                                              .queueFamilyIndex = created->queueFamilyIndex, .queueIndex = index};
            VkQueue queue = VK_NULL_HANDLE;
            get_queue(*device, &which, &queue);
            VkExportMetalCommandQueueInfoEXT exported = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT,
                                                         .queue = queue};
            VkExportMetalObjectsInfoEXT objects = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
                                                   .pNext = &exported};
            if (export_objects != NULL) {
                export_objects(*device, &objects);
            }
            if (exported.mtlCommandQueue != nil) {
                entry->queues[entry->count++] = [exported.mtlCommandQueue retain];
            }
        }
    }
    pthread_mutex_lock(&devices_lock);
    entry->next = devices;
    devices = entry;
    pthread_mutex_unlock(&devices_lock);
    return VK_SUCCESS;
}

static void device_destroyed(VkDevice device, const VkAllocationCallbacks *allocator) {
    struct device *gone = NULL;
    /* The lock waits while "scheduled" commits and waits on these queues. */
    pthread_mutex_lock(&devices_lock);
    for (struct device **entry = &devices; *entry != NULL; entry = &(*entry)->next) {
        if ((*entry)->device == device) {
            gone = *entry;
            *entry = gone->next;
            break;
        }
    }
    pthread_mutex_unlock(&devices_lock);
    for (uint32_t index = 0; gone != NULL && index < gone->count; index++) {
        [gone->queues[index] release];
    }
    free(gone);
    destroy_device(device, allocator);
}

int minato_graphics_vulkan_provider(void *context, uint64_t deadline) {
    (void)context;
    return minato_graphics_vulkan_scheduled(deadline);
}

int minato_graphics_vulkan_scheduled(uint64_t deadline) {
    while (atomic_load(&in_flight) != 0) {
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
            return 0;
        }
        usleep(100);
    }
    int all = 1;
    pthread_mutex_lock(&devices_lock);
    @autoreleasepool {
        dispatch_semaphore_t scheduled = dispatch_semaphore_create(0);
        uint32_t committed = 0;
        for (const struct device *device = devices; device != NULL; device = device->next) {
            for (uint32_t index = 0; index < device->count; index++) {
                id<MTLCommandBuffer> buffer = [device->queues[index] commandBuffer];
                if (buffer == nil) {
                    all = 0;
                    continue;
                }
                [buffer addScheduledHandler:^(id<MTLCommandBuffer> unused) {
                    (void)unused;
                    dispatch_semaphore_signal(scheduled);
                }];
                [buffer commit];
                committed++;
            }
        }
        /* Metal schedules a queue's command buffers in their order, so each earlier one is scheduled too. */
        for (uint32_t index = 0; index < committed && all; index++) {
            const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            all = now < deadline &&
                  dispatch_semaphore_wait(scheduled, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(deadline - now))) == 0;
        }
        dispatch_release(scheduled);
    }
    pthread_mutex_unlock(&devices_lock);
    return all;
}

/* An app's debug callback, the user data of graphics/host's function: the app's function and user data,
   and what they belong to, the messenger's or report callback's handle or the instance of vkCreateInstance's
   chain. */
struct callback {
    struct callback *next;
    uint64_t owner, function, user;
};
static struct callback *callbacks;
static pthread_mutex_t callbacks_lock = PTHREAD_MUTEX_INITIALIZER;

static struct callback *callback_new(uint64_t function, uint64_t user) {
    struct callback *callback = calloc(1, sizeof *callback);
    if (callback != NULL) {
        callback->function = function;
        callback->user = user;
    }
    return callback;
}

static void callbacks_free(struct callback *callback) {
    while (callback != NULL) {
        struct callback *next = callback->next;
        free(callback);
        callback = next;
    }
}

/* Adds a list of callbacks for their owner, once MoltenVK took them. */
static void callbacks_keep(struct callback *list, uint64_t owner) {
    if (list == NULL) {
        return;
    }
    struct callback *last = list;
    for (; last->next != NULL; last = last->next) {
        last->owner = owner;
    }
    last->owner = owner;
    pthread_mutex_lock(&callbacks_lock);
    last->next = callbacks;
    callbacks = list;
    pthread_mutex_unlock(&callbacks_lock);
}

/* Frees an owner's callbacks, once MoltenVK let them go: its destroy functions wait for a message in
   progress. */
static void callbacks_forget(uint64_t owner) {
    struct callback *gone = NULL;
    pthread_mutex_lock(&callbacks_lock);
    for (struct callback **entry = &callbacks; *entry != NULL;) {
        if ((*entry)->owner == owner) {
            struct callback *callback = *entry;
            *entry = callback->next;
            callback->next = gone;
            gone = callback;
        } else {
            entry = &(*entry)->next;
        }
    }
    pthread_mutex_unlock(&callbacks_lock);
    callbacks_free(gone);
}

static void logged(const char *message) {
    fprintf(stderr, "minato graphics: a Vulkan debug message on a host thread, logged instead of calling the app: %s\n",
            message != NULL ? message : "(none)");
}

static VkBool32 message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                        const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    const struct callback *callback = user;
    if (!apkrt_is_guest_thread()) {
        logged(data != NULL ? data->pMessage : NULL);
        return VK_FALSE;
    }
    return callback->function == 0 ? VK_FALSE
                                   : (VkBool32)apkrt_call_guest(callback->function, (uint64_t)severity, types,
                                                                (uint64_t)(uintptr_t)data, callback->user, 0, 0, 0, 0);
}

static VkBool32 report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type, uint64_t object, size_t location,
                       int32_t code, const char *prefix, const char *text, void *user) {
    const struct callback *callback = user;
    if (!apkrt_is_guest_thread()) {
        logged(text);
        return VK_FALSE;
    }
    return callback->function == 0
               ? VK_FALSE
               : (VkBool32)apkrt_call_guest(callback->function, flags, (uint64_t)type, object, location,
                                            (uint32_t)code, (uint64_t)(uintptr_t)prefix, (uint64_t)(uintptr_t)text,
                                            callback->user);
}

/* libvulkan hands the app's debug callbacks of vkCreateInstance's chain over as copies of its own
   (graphics/vulkan/instance.c): they get graphics/host's functions here, for the instance's life. */
static VkResult instance_created(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                 VkInstance *instance) {
    struct callback *chained = NULL;
    for (VkBaseOutStructure *structure = (VkBaseOutStructure *)info->pNext; structure != NULL;
         structure = structure->pNext) {
        struct callback *callback = NULL;
        if (structure->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) {
            VkDebugUtilsMessengerCreateInfoEXT *messenger = (VkDebugUtilsMessengerCreateInfoEXT *)structure;
            callback = callback_new((uint64_t)(uintptr_t)messenger->pfnUserCallback,
                                    (uint64_t)(uintptr_t)messenger->pUserData);
            if (callback != NULL) {
                messenger->pfnUserCallback = message;
                messenger->pUserData = callback;
            }
        } else if (structure->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) {
            VkDebugReportCallbackCreateInfoEXT *reported = (VkDebugReportCallbackCreateInfoEXT *)structure;
            callback = callback_new((uint64_t)(uintptr_t)reported->pfnCallback, (uint64_t)(uintptr_t)reported->pUserData);
            if (callback != NULL) {
                reported->pfnCallback = report;
                reported->pUserData = callback;
            }
        } else {
            continue;
        }
        if (callback == NULL) {
            callbacks_free(chained);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        callback->next = chained;
        chained = callback;
    }
    const VkResult result = create_instance(info, allocator, instance);
    if (result != VK_SUCCESS) {
        callbacks_free(chained);
        return result;
    }
    callbacks_keep(chained, (uint64_t)(uintptr_t)*instance);
    return VK_SUCCESS;
}

static void instance_destroyed(VkInstance instance, const VkAllocationCallbacks *allocator) {
    destroy_instance(instance, allocator);
    callbacks_forget((uint64_t)(uintptr_t)instance);
}

static VkResult messenger_created(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *info,
                                  const VkAllocationCallbacks *allocator, VkDebugUtilsMessengerEXT *messenger) {
    struct callback *callback =
        callback_new((uint64_t)(uintptr_t)info->pfnUserCallback, (uint64_t)(uintptr_t)info->pUserData);
    if (callback == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkDebugUtilsMessengerCreateInfoEXT ours = *info;
    ours.pfnUserCallback = message;
    ours.pUserData = callback;
    const VkResult result = create_messenger(instance, &ours, allocator, messenger);
    if (result != VK_SUCCESS) {
        free(callback);
        return result;
    }
    callbacks_keep(callback, (uint64_t)(uintptr_t)*messenger);
    return VK_SUCCESS;
}

static void messenger_destroyed(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
                                const VkAllocationCallbacks *allocator) {
    destroy_messenger(instance, messenger, allocator);
    callbacks_forget((uint64_t)(uintptr_t)messenger);
}

static VkResult report_created(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT *info,
                               const VkAllocationCallbacks *allocator, VkDebugReportCallbackEXT *reported) {
    struct callback *callback =
        callback_new((uint64_t)(uintptr_t)info->pfnCallback, (uint64_t)(uintptr_t)info->pUserData);
    if (callback == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkDebugReportCallbackCreateInfoEXT ours = *info;
    ours.pfnCallback = report;
    ours.pUserData = callback;
    const VkResult result = create_report(instance, &ours, allocator, reported);
    if (result != VK_SUCCESS) {
        free(callback);
        return result;
    }
    callbacks_keep(callback, (uint64_t)(uintptr_t)*reported);
    return VK_SUCCESS;
}

static void report_destroyed(VkInstance instance, VkDebugReportCallbackEXT reported,
                             const VkAllocationCallbacks *allocator) {
    destroy_report(instance, reported, allocator);
    callbacks_forget((uint64_t)(uintptr_t)reported);
}

void minato_graphics_vulkan_wrap(void **table) {
#define WRAP(name, moltenvk, ours)                            \
    moltenvk = (PFN_##name)table[MINATO_VULKAN_SLOT_##name]; \
    table[MINATO_VULKAN_SLOT_##name] = (void *)ours;
    WRAP(vkCreateInstance, create_instance, instance_created)
    WRAP(vkDestroyInstance, destroy_instance, instance_destroyed)
    WRAP(vkCreateDevice, create_device, device_created)
    WRAP(vkDestroyDevice, destroy_device, device_destroyed)
    WRAP(vkQueueSubmit, queue_submit, submitted)
    WRAP(vkQueueSubmit2, queue_submit2, submitted2)
    WRAP(vkQueueBindSparse, queue_bind_sparse, bound_sparse)
    WRAP(vkAcquireNextImageKHR, acquire_image, acquired)
    WRAP(vkQueuePresentKHR, queue_present, presented)
    WRAP(vkCreateSwapchainKHR, create_swapchain, swapchain_created)
    WRAP(vkDestroySwapchainKHR, destroy_swapchain, swapchain_destroyed)
    WRAP(vkCreateDebugUtilsMessengerEXT, create_messenger, messenger_created)
    WRAP(vkDestroyDebugUtilsMessengerEXT, destroy_messenger, messenger_destroyed)
    WRAP(vkCreateDebugReportCallbackEXT, create_report, report_created)
    WRAP(vkDestroyDebugReportCallbackEXT, destroy_report, report_destroyed)
#undef WRAP
    table[MINATO_VULKAN_SLOT_vkCreateAndroidSurfaceKHR] = (void *)surface_created;
    get_queue = (PFN_vkGetDeviceQueue2)table[MINATO_VULKAN_SLOT_vkGetDeviceQueue2];
    device_function = (PFN_vkGetDeviceProcAddr)table[MINATO_VULKAN_SLOT_vkGetDeviceProcAddr];
    instance_function = (PFN_vkGetInstanceProcAddr)table[MINATO_VULKAN_SLOT_vkGetInstanceProcAddr];
}
