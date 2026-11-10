/* graphics/host's functions in front of MoltenVK's for libvulkan (ADR-0030, decisions 5, 12 and 13); see
   minato_graphics.h. The "vulkan" provider puts them into their slots of libvulkan's table, so they run as
   host code on the guest threads that call libvulkan:
   - the submits, acquires and presents count the calls in MoltenVK, until which "scheduled" waits, and
     back out in the background (host_protocol.h), where libvulkan waits at the window's gate;
   - vkCreateAndroidSurfaceKHR, which MoltenVK lacks, makes MoltenVK's surface on the layer of the window
     libvulkan names; a swapchain takes a window that is there, and Core Animation gets MoltenVK's changes
     to the layer, made on a thread without a run loop, when it is created or destroyed; presents are
     counted per window for the FPS label;
   - VK_GOOGLE_display_timing's times reach MoltenVK on the display's refresh grid, less half a period, 0 as 0, and
     as 0 for a refresh at most 2 periods after the display link's last tick (present_time.h, ADR-0041 decision 11
     and its b)), or all as 0 for a present libvulkan flags MINATO_WINDOWS_WITHOUT_TIME;
     the past timings give the app the times it asked for back, as Android's do;
   - each present leaves a record per swapchain on its window's layer before MoltenVK gets it, with the time the app
     asked for (none for a present libvulkan flags), and takes them back when MoltenVK fails it short of its drawables
     (present_pace.h, decision 11 c)); the past timings leave out a present whose drawable Core Animation did not
     get, as Android's leave out a frame SurfaceFlinger dropped;
   - vkCreateDevice keeps each queue's MTLCommandQueue for "scheduled", and vkDestroyDevice lets them go;
   - the app's debug callbacks, from vkCreateInstance's chain or their create functions, reach MoltenVK as
     graphics/host's functions, which call the app's through apkrt_call_guest on guest threads only and log
     the message on any other thread (decision 5);
   - a dedicated allocation of a hardware buffer, which libvulkan names first in vkAllocateMemory's chain
     (host_protocol.h, ADR-0058 decision 7), gives the dedicated image the buffer's IOSurface (vkUseIOSurfaceMVK, which
     replaces the IOSurface of an image that has no memory yet) before MoltenVK allocates the memory; an ID without a
     buffer of graphics/host's, a buffer of more than one layer, or a MoltenVK without the function answer
     VK_ERROR_INVALID_EXTERNAL_HANDLE; when MoltenVK's allocation fails after it, the image keeps the IOSurface, which
     a later import replaces. vkUseIOSurfaceMVK is MoltenVK's deprecated API, ADR-0058 decision 7's departure: check it
     at every update of MoltenVK;
   - the image's shape is the guest's, which libvulkan checks (external_memory.c) and a guest may skip: vkCreateImage
     keeps each image's flags, type, format, extent, levels, layers, samples and tiling until vkDestroyImage or its
     device's vkDestroyDevice, and an import takes only an image the host made in the one shape a buffer's IOSurface
     has (2D, depth 1, one sample, level and layer, optimal tiling, not sparse, protected or disjoint, the buffer's
     size and the format of the specification's equivalence table for its format) that has no buffer's memory yet;
     any other answers VK_ERROR_INVALID_EXTERNAL_HANDLE before MoltenVK sees the IOSurface, which it would wrap in a
     texture of another shape (vkUseIOSurfaceMVK checks only the size and the bytes per texel). Android 15's driver
     takes such an import (the specification's valid usage, which it does not check): the step's departure.
   Manual reference counting: the queues live in plain C structures. */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "host_protocol.h"
#include "minato_graphics.h"
#include "present_pace.h"
#include "present_time.h"
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
static PFN_vkGetPastPresentationTimingGOOGLE past_timing;
static PFN_vkCreateDebugUtilsMessengerEXT create_messenger;
static PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger;
static PFN_vkCreateDebugReportCallbackEXT create_report;
static PFN_vkDestroyDebugReportCallbackEXT destroy_report;
static PFN_vkAllocateMemory allocate_memory;
static PFN_vkCreateImage create_image;
static PFN_vkDestroyImage destroy_image;
static PFN_vkGetDeviceQueue2 get_queue;
/* MoltenVK's vkUseIOSurfaceMVK (mvk_deprecated_api.h), NULL for a library without it. */
static VkResult (*use_iosurface)(VkImage image, IOSurfaceRef surface);
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

int minato_graphics_vulkan_enter(void) {
    return entered();
}

void minato_graphics_vulkan_leave(void) {
    left();
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

/* VK_GOOGLE_display_timing's times, right after libvulkan's windows in a present's chain (swapchain.c). */
static const VkPresentTimesInfoGOOGLE *times_of(const void *chain) {
    const VkPresentTimesInfoGOOGLE *times = chain;
    return times != NULL && times->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE && times->pTimes != NULL
               ? times
               : NULL;
}

/* The times the app asked for, per MoltenVK swapchain in present order, with the ones MoltenVK got: MoltenVK's past
   timings carry the time it got, and Android's carry the app's own (swapchain.cpp, TimingInfo). A ring of the last
   64, more than MoltenVK keeps (60) with the presents on their way.
   With them, the presents whose drawable Core Animation did not get (present_pace.h), until MoltenVK's past timing for
   each comes: MoltenVK records it once it lets the drawable go, when it acquires that image again, so after the
   timings of later presents [measured: the Mac, graphics.vulkan_drop]. The last 8. */
#define ASKED_KEPT 64u
#define DROPPED_KEPT 8u
struct asked {
    struct asked *next;
    VkSwapchainKHR swapchain;
    uint32_t first, count;
    struct {
        uint32_t id;
        uint64_t handed, asked;
    } times[ASKED_KEPT];
    uint32_t dropped_count;
    struct {
        uint32_t id;
        uint64_t handed;
    } dropped[DROPPED_KEPT];
};
static struct asked *askeds;
static pthread_mutex_t askeds_lock = PTHREAD_MUTEX_INITIALIZER;

/* Under the lock: the swapchain's ring, made when `make` is set; NULL without one or without memory. */
static struct asked *asked_of(VkSwapchainKHR swapchain, int make) {
    struct asked *entry = askeds;
    while (entry != NULL && entry->swapchain != swapchain) {
        entry = entry->next;
    }
    if (entry == NULL && make && (entry = calloc(1, sizeof *entry)) != NULL) {
        entry->swapchain = swapchain;
        entry->next = askeds;
        askeds = entry;
    }
    return entry;
}

static void asked_keep(const VkSwapchainKHR *swapchains, const VkPresentTimeGOOGLE *asked,
                       const VkPresentTimeGOOGLE *handed, uint32_t count) {
    pthread_mutex_lock(&askeds_lock);
    for (uint32_t index = 0; index < count; index++) {
        struct asked *entry = asked_of(swapchains[index], 1);
        if (entry == NULL) {
            continue;  /* its past timings keep MoltenVK's times */
        }
        if (entry->count == ASKED_KEPT) {
            entry->first = (entry->first + 1) % ASKED_KEPT;
            entry->count--;
        }
        const uint32_t slot = (entry->first + entry->count++) % ASKED_KEPT;
        entry->times[slot].id = asked[index].presentID;
        entry->times[slot].handed = handed[index].desiredPresentTime;
        entry->times[slot].asked = asked[index].desiredPresentTime;
    }
    pthread_mutex_unlock(&askeds_lock);
}

/* Core Animation did not get the drawable of the present with the record's ID and time handed (present_pace.h). */
static void present_dropped(const struct minato_present_record *record) {
    pthread_mutex_lock(&askeds_lock);
    struct asked *entry = asked_of((VkSwapchainKHR)record->swapchain, 0);
    if (entry != NULL) {
        if (entry->dropped_count == DROPPED_KEPT) {
            memmove(&entry->dropped[0], &entry->dropped[1], (DROPPED_KEPT - 1) * sizeof entry->dropped[0]);
            entry->dropped_count--;
        }
        entry->dropped[entry->dropped_count].id = record->id;
        entry->dropped[entry->dropped_count++].handed = record->handed;
    }
    pthread_mutex_unlock(&askeds_lock);
}

/* Under the lock: whether the timing is of a present whose drawable Core Animation did not get, which it forgets. */
static int dropped_taken(struct asked *entry, const VkPastPresentationTimingGOOGLE *timing) {
    for (uint32_t index = 0; entry != NULL && index < entry->dropped_count; index++) {
        if (entry->dropped[index].id == timing->presentID &&
            entry->dropped[index].handed == timing->desiredPresentTime) {
            memmove(&entry->dropped[index], &entry->dropped[index + 1],
                    (entry->dropped_count - index - 1) * sizeof entry->dropped[0]);
            entry->dropped_count--;
            return 1;
        }
    }
    return 0;
}

/* A timing of a present whose drawable Core Animation did not get goes, as Android's past timings leave out a frame
   SurfaceFlinger dropped, and the later ones move up. Each other timing gets the time of the oldest present kept with
   its ID and the time MoltenVK got; MoltenVK hands those out in present order, so the presents before that one are
   gone. A count asked for without timings stays MoltenVK's, which may hold such a present. */
static VkResult past_timings_given(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count,
                                   VkPastPresentationTimingGOOGLE *timings) {
    const VkResult result = past_timing(device, swapchain, count, timings);
    if (timings == NULL || (result != VK_SUCCESS && result != VK_INCOMPLETE)) {
        return result;
    }
    pthread_mutex_lock(&askeds_lock);
    struct asked *entry = asked_of(swapchain, 0);
    uint32_t given = 0;
    for (uint32_t index = 0; index < *count; index++) {
        VkPastPresentationTimingGOOGLE timing = timings[index];
        if (dropped_taken(entry, &timing)) {
            continue;
        }
        for (uint32_t kept = 0; entry != NULL && kept < entry->count; kept++) {
            const uint32_t slot = (entry->first + kept) % ASKED_KEPT;
            if (entry->times[slot].id == timing.presentID && entry->times[slot].handed == timing.desiredPresentTime) {
                timing.desiredPresentTime = entry->times[slot].asked;
                entry->first = (slot + 1) % ASKED_KEPT;
                entry->count -= kept + 1;
                break;
            }
        }
        timings[given++] = timing;
    }
    *count = given;
    pthread_mutex_unlock(&askeds_lock);
    return result;
}

static void asked_forget(VkSwapchainKHR swapchain) {
    pthread_mutex_lock(&askeds_lock);
    for (struct asked **entry = &askeds; *entry != NULL; entry = &(*entry)->next) {
        if ((*entry)->swapchain == swapchain) {
            struct asked *gone = *entry;
            *entry = gone->next;
            free(gone);
            break;
        }
    }
    pthread_mutex_unlock(&askeds_lock);
}

/* The layer of each swapchain on a window, retained while the swapchain lives, for the presents' records
   (present_pace.h): few, a list under a lock. */
struct chain {
    struct chain *next;
    VkSwapchainKHR swapchain;
    CAMetalLayer *layer;
};
static struct chain *chains;
static pthread_mutex_t chains_lock = PTHREAD_MUTEX_INITIALIZER;

/* Keeps the swapchain's layer, whose retain it takes. */
static void chain_keep(VkSwapchainKHR swapchain, CAMetalLayer *layer) {
    struct chain *entry = malloc(sizeof *entry);
    if (entry == NULL) {
        [layer release];
        return;  /* its presents leave no records */
    }
    *entry = (struct chain){NULL, swapchain, layer};
    pthread_mutex_lock(&chains_lock);
    entry->next = chains;
    chains = entry;
    pthread_mutex_unlock(&chains_lock);
}

/* The swapchain's layer, unretained: the list holds it while the app presents on the swapchain. nil for none. */
static CAMetalLayer *chain_layer(VkSwapchainKHR swapchain) {
    CAMetalLayer *layer = nil;
    pthread_mutex_lock(&chains_lock);
    for (const struct chain *entry = chains; entry != NULL; entry = entry->next) {
        if (entry->swapchain == swapchain) {
            layer = entry->layer;
            break;
        }
    }
    pthread_mutex_unlock(&chains_lock);
    return layer;
}

/* Takes the swapchain out of the list: its layer, retained for the caller, or nil. */
static CAMetalLayer *chain_forget(VkSwapchainKHR swapchain) {
    CAMetalLayer *layer = nil;
    pthread_mutex_lock(&chains_lock);
    for (struct chain **entry = &chains; *entry != NULL; entry = &(*entry)->next) {
        if ((*entry)->swapchain == swapchain) {
            struct chain *gone = *entry;
            *entry = gone->next;
            layer = gone->layer;
            free(gone);
            break;
        }
    }
    pthread_mutex_unlock(&chains_lock);
    return layer;
}

static VkResult presented(VkQueue queue, const VkPresentInfoKHR *info) {
    const struct minato_windows *windows = windows_of(info->pNext);
    VkPresentInfoKHR ours = *info;
    ours.pNext = windows != NULL ? windows->pNext : info->pNext;
    const VkPresentTimesInfoGOOGLE *asked = times_of(ours.pNext);
    VkPresentTimeGOOGLE few[4];
    VkPresentTimeGOOGLE *const times =
        asked == NULL || asked->swapchainCount <= 4 ? few : malloc(asked->swapchainCount * sizeof *times);
    if (times == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkPresentTimesInfoGOOGLE handed = {0};
    const int without_time = windows != NULL && (windows->flags & MINATO_WINDOWS_WITHOUT_TIME) != 0;
    if (asked != NULL) {
        /* 0, for no time, stays 0, and so does a time beyond INT64_MAX, which is negative as an int64_t. */
        for (uint32_t index = 0; index < asked->swapchainCount; index++) {
            int64_t time = 0;
            const int without = without_time || minato_present_time((int64_t)asked->pTimes[index].desiredPresentTime,
                                                                    &time) == MINATO_PRESENT_WITHOUT_TIME;
            times[index] = asked->pTimes[index];
            times[index].desiredPresentTime = without ? 0 : (uint64_t)time;
        }
        handed = *asked;
        handed.pTimes = times;
        ours.pNext = &handed;
    }
    /* The frame rate limit (ADR-0078 decision 4), for each swapchain's window. */
    for (uint32_t index = 0; index < info->swapchainCount; index++) {
        minato_present_hold(chain_layer(info->pSwapchains[index]));
    }
    if (!entered()) {
        if (times != few) {
            free(times);
        }
        return MINATO_VK_BACKGROUND;
    }
    if (asked != NULL) {
        asked_keep(info->pSwapchains, asked->pTimes, times,
                   asked->swapchainCount < info->swapchainCount ? asked->swapchainCount : info->swapchainCount);
    }
    /* Each swapchain's record on its layer, before MoltenVK can present a drawable (present_pace.h). */
    uint64_t some[4];
    uint64_t *const tickets = info->swapchainCount <= 4 ? some : calloc(info->swapchainCount, sizeof *tickets);
    for (uint32_t index = 0; tickets != NULL && index < info->swapchainCount; index++) {
        const int timed = asked != NULL && index < asked->swapchainCount;
        const int64_t time = timed ? (int64_t)asked->pTimes[index].desiredPresentTime : 0;
        const struct minato_present_record record = {.asked = !without_time && time > 0 ? time : 0,
                                                     .dropped = present_dropped,
                                                     .swapchain = (uint64_t)info->pSwapchains[index],
                                                     .handed = timed ? times[index].desiredPresentTime : 0,
                                                     .id = timed ? asked->pTimes[index].presentID : 0};
        tickets[index] = minato_present_push(chain_layer(info->pSwapchains[index]), &record);
    }
    const VkResult result = queue_present(queue, &ours);
    left();
    /* MoltenVK presents each image it was given whatever the surface's state, VK_ERROR_OUT_OF_DATE_KHR or
       VK_ERROR_SURFACE_LOST_KHR [verified: MVKQueue.mm at db66022]; another error leaves its drawables unpresented. */
    for (uint32_t index = 0; tickets != NULL && result < 0 && result != VK_ERROR_OUT_OF_DATE_KHR &&
                             result != VK_ERROR_SURFACE_LOST_KHR && index < info->swapchainCount;
         index++) {
        minato_present_take_back(chain_layer(info->pSwapchains[index]), tickets[index]);
    }
    if (tickets != some) {
        free(tickets);
    }
    if (times != few) {
        free(times);
    }
    /* Each window's presents, for the FPS label. */
    for (uint32_t index = 0; windows != NULL && index < windows->count && index < info->swapchainCount; index++) {
        const VkResult own = info->pResults != NULL ? info->pResults[index] : result;
        if (own == VK_SUCCESS || own == VK_SUBOPTIMAL_KHR) {
            apkrt_window_presented_as(windows->windows[index], APKRT_PRESENT_VULKAN);
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
   thread, which has no run loop to commit it, so Core Animation gets the changes here. The swapchain's layer is kept
   for its presents' records. */
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
        CAMetalLayer *layer =
            windows != NULL && windows->count == 1 ? (CAMetalLayer *)apkrt_window_layer(windows->windows[0]) : nil;
        if (layer != nil) {
            chain_keep(*swapchain, layer);
        }
    }
    return result;
}

/* MoltenVK presents a drawable from its present's scheduled handler, which first counts the present on the swapchain
   without checking that it is still there [verified: MVKImage.mm at v1.4.2], and vkDeviceWaitIdle does not wait for
   that handler [measured]: a handler that Metal runs late, after the app destroyed the swapchain, would crash. So the
   swapchain goes once its presents' drawables reached their present (their records, present_pace.h), or after 1 s.
   The layer then forgets the swapchain's records; drawables still on their way go as they come. */
static void swapchain_destroyed(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator) {
    CAMetalLayer *layer = chain_forget(swapchain);
    const int64_t deadline = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000;
    if (layer != nil && !minato_present_wait(layer, (uint64_t)swapchain, deadline)) {
        fprintf(stderr, "minato graphics: a swapchain destroyed while drawables of its presents were on their way\n");
    }
    minato_present_forget(layer);
    destroy_swapchain(device, swapchain, allocator);
    asked_forget(swapchain);
    [layer release];
    [CATransaction flush];
}

/* A device libvulkan or ANGLE made, with its queues' MTLCommandQueues, retained. */
struct device {
    struct device *next;
    VkDevice device;
    uint32_t count;
    id<MTLCommandQueue> queues[];
};
static struct device *devices;
static pthread_mutex_t devices_lock = PTHREAD_MUTEX_INITIALIZER;

int minato_graphics_vulkan_keep_device(VkDevice device, const VkDeviceCreateInfo *info, PFN_vkGetDeviceQueue2 queue_of,
                                       PFN_vkGetDeviceProcAddr function_of) {
    uint32_t count = 0;
    for (uint32_t family = 0; family < info->queueCreateInfoCount; family++) {
        count += info->pQueueCreateInfos[family].queueCount;
    }
    struct device *entry = calloc(1, sizeof *entry + count * sizeof entry->queues[0]);
    if (entry == NULL) {
        return -1;
    }
    entry->device = device;
    const PFN_vkExportMetalObjectsEXT export_objects =
        (PFN_vkExportMetalObjectsEXT)function_of(device, "vkExportMetalObjectsEXT");
    for (uint32_t family = 0; family < info->queueCreateInfoCount; family++) {
        const VkDeviceQueueCreateInfo *created = &info->pQueueCreateInfos[family];
        for (uint32_t index = 0; index < created->queueCount; index++) {
            const VkDeviceQueueInfo2 which = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .flags = created->flags,
                                              .queueFamilyIndex = created->queueFamilyIndex, .queueIndex = index};
            VkQueue queue = VK_NULL_HANDLE;
            queue_of(device, &which, &queue);
            VkExportMetalCommandQueueInfoEXT exported = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT,
                                                         .queue = queue};
            VkExportMetalObjectsInfoEXT objects = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
                                                   .pNext = &exported};
            if (export_objects != NULL) {
                export_objects(device, &objects);
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
    return 0;
}

static VkResult device_created(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                               const VkAllocationCallbacks *allocator, VkDevice *device) {
    VkResult result = create_device(physical, info, allocator, device);
    if (result != VK_SUCCESS) {
        return result;
    }
    if (minato_graphics_vulkan_keep_device(*device, info, get_queue, device_function) != 0) {
        destroy_device(*device, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

void minato_graphics_vulkan_hold_devices_for_testing(uint64_t nanoseconds) {
    pthread_mutex_lock(&devices_lock);
    const uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + nanoseconds;
    while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < end) {
        usleep(1000);
    }
    pthread_mutex_unlock(&devices_lock);
}

void minato_graphics_vulkan_forget_device(VkDevice device) {
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
}

static void images_forget_device(VkDevice device);

static void device_destroyed(VkDevice device, const VkAllocationCallbacks *allocator) {
    minato_graphics_vulkan_forget_device(device);
    images_forget_device(device);
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
    /* The app asks on its main thread: a guest thread that holds the lock while it is blocked must not keep the main
       thread past the deadline. Not scheduled in time is an answer; the background goes on. */
    while (pthread_mutex_trylock(&devices_lock) != 0) {
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
            fprintf(stderr, "minato graphics: Vulkan's \"scheduled\" gave up on the devices' lock at its deadline\n");
            return 0;
        }
        usleep(100);
    }
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

/* An image the host's vkCreateImage made, as the guest asked for it, kept by its handle until vkDestroyImage. */
struct image {
    struct image *next;
    VkDevice device;
    VkImage handle;
    VkImageCreateFlags flags;
    VkImageType type;
    VkFormat format;
    VkExtent3D extent;
    uint32_t levels, layers;
    VkSampleCountFlagBits samples;
    VkImageTiling tiling;
    uint64_t taken; /* the import that has given it a buffer's IOSurface or is giving it one, else 0 */
};
enum { image_buckets = 256 };
static struct image *images[image_buckets];
static uint64_t imports; /* the last import's number */
static pthread_mutex_t images_lock = PTHREAD_MUTEX_INITIALIZER;

static struct image **image_bucket(VkImage handle) {
    const uint64_t key = (uint64_t)(uintptr_t)handle;
    return &images[(key ^ (key >> 8) ^ (key >> 16)) % image_buckets];
}

/* The entry of `handle` in its bucket, or the end of the bucket; under images_lock. */
static struct image **image_entry(VkImage handle) {
    struct image **entry = image_bucket(handle);
    while (*entry != NULL && (*entry)->handle != handle) {
        entry = &(*entry)->next;
    }
    return entry;
}

/* Keeps the shape of a new image of `device`; 0, or -1 without memory. */
static int image_keep(VkDevice device, VkImage handle, const VkImageCreateInfo *info) {
    struct image *kept = calloc(1, sizeof *kept);
    if (kept == NULL) {
        return -1;
    }
    *kept = (struct image){.device = device,
                           .handle = handle,
                           .flags = info->flags,
                           .type = info->imageType,
                           .format = info->format,
                           .extent = info->extent,
                           .levels = info->mipLevels,
                           .layers = info->arrayLayers,
                           .samples = info->samples,
                           .tiling = info->tiling};
    pthread_mutex_lock(&images_lock);
    struct image **entry = image_entry(handle);
    if (*entry != NULL) { /* MoltenVK's handle again, of an image destroyed past the host */
        kept->next = (*entry)->next;
        free(*entry);
    }
    *entry = kept;
    pthread_mutex_unlock(&images_lock);
    return 0;
}

static void image_forget(VkImage handle) {
    pthread_mutex_lock(&images_lock);
    struct image **entry = image_entry(handle);
    struct image *gone = *entry;
    if (gone != NULL) {
        *entry = gone->next;
    }
    pthread_mutex_unlock(&images_lock);
    free(gone);
}

/* The images of a destroyed device, which the app may not have destroyed: their handles may come again. */
static void images_forget_device(VkDevice device) {
    struct image *gone = NULL;
    pthread_mutex_lock(&images_lock);
    for (size_t bucket = 0; bucket < image_buckets; bucket++) {
        for (struct image **entry = &images[bucket]; *entry != NULL;) {
            struct image *kept = *entry;
            if (kept->device == device) {
                *entry = kept->next;
                kept->next = gone;
                gone = kept;
            } else {
                entry = &kept->next;
            }
        }
    }
    pthread_mutex_unlock(&images_lock);
    while (gone != NULL) {
        struct image *next = gone->next;
        free(gone);
        gone = next;
    }
}

/* The VkFormat of a buffer format in the specification's equivalence table, as libvulkan maps it
   (graphics/vulkan/external_memory.c); VK_FORMAT_UNDEFINED for any other, which no image imports. */
static VkFormat buffer_vk_format(uint32_t format) {
    switch (format) {
        case 1: /* AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM */
        case 2: /* R8G8B8X8_UNORM, whose alpha is in memory */
            return VK_FORMAT_R8G8B8A8_UNORM;
        case 4: /* R5G6B5_UNORM */
            return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case 0x16: /* R16G16B16A16_FLOAT */
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 0x2b: /* R10G10B10A2_UNORM */
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

/* What no IOSurface's texture is. */
static const VkImageCreateFlags flags_refused = VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                                                VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                                                VK_IMAGE_CREATE_SPARSE_ALIASED_BIT | VK_IMAGE_CREATE_PROTECTED_BIT |
                                                VK_IMAGE_CREATE_DISJOINT_BIT;

/* Takes image `handle` for an import of a buffer of `description`: the import's number when the host made it in the
   buffer's shape and no import took it yet, else 0. image_release with that number gives it back after an import
   that failed, unless the image is gone by then. */
static uint64_t image_take(VkImage handle, const struct apkrt_hardware_buffer_description *description) {
    const VkFormat format = buffer_vk_format(description->format);
    pthread_mutex_lock(&images_lock);
    struct image *kept = *image_entry(handle);
    const int fits = kept != NULL && kept->taken == 0 && (kept->flags & flags_refused) == 0 &&
                     kept->type == VK_IMAGE_TYPE_2D && kept->extent.depth == 1 &&
                     kept->samples == VK_SAMPLE_COUNT_1_BIT && kept->tiling == VK_IMAGE_TILING_OPTIMAL &&
                     kept->levels == 1 && kept->layers == 1 && description->layers == 1 &&
                     format != VK_FORMAT_UNDEFINED && kept->format == format &&
                     kept->extent.width == description->width && kept->extent.height == description->height;
    const uint64_t taken = fits ? (kept->taken = ++imports) : 0;
    pthread_mutex_unlock(&images_lock);
    return taken;
}

static void image_release(VkImage handle, uint64_t taken) {
    pthread_mutex_lock(&images_lock);
    struct image *kept = *image_entry(handle);
    if (kept != NULL && kept->taken == taken) {
        kept->taken = 0;
    }
    pthread_mutex_unlock(&images_lock);
}

static VkResult image_created(VkDevice device, const VkImageCreateInfo *info, const VkAllocationCallbacks *allocator,
                              VkImage *image) {
    const VkResult result = create_image(device, info, allocator, image);
    if (result != VK_SUCCESS) {
        return result;
    }
    if (image_keep(device, *image, info) != 0) {
        destroy_image(device, *image, allocator);
        *image = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

/* The shape goes before MoltenVK lets the handle go, which a new image may get again. */
static void image_destroyed(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
    if (image != VK_NULL_HANDLE) {
        image_forget(image);
    }
    destroy_image(device, image, allocator);
}

/* The IOSurface of the hardware buffer `buffer`, retained, with the buffer's description; NULL for an ID without a
   buffer of graphics/host's. The host's reference is only taken for the lookup: the memory's reference to the buffer
   is libvulkan's, and the image retains the IOSurface. */
static IOSurfaceRef buffer_surface(uint64_t buffer, struct apkrt_hardware_buffer_description *description) {
    uint64_t handle = 0;
    void *context = NULL;
    uint32_t stride = 0;
    if (buffer == 0 || apkrt_hardware_buffer_acquire_handle(buffer, &handle, &context, description, &stride) != 0) {
        return NULL;
    }
    IOSurfaceRef surface = context == minato_graphics_hardware_buffers()->context
                               ? (IOSurfaceRef)minato_graphics_hardware_buffer_surface(handle)
                               : NULL;
    apkrt_hardware_buffer_release_handle(buffer);
    return surface;
}

static VkResult memory_allocated(VkDevice device, const VkMemoryAllocateInfo *info,
                                 const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    const struct minato_hardware_buffer *named = info->pNext;
    if (named == NULL || named->sType != MINATO_STRUCTURE_TYPE_HARDWARE_BUFFER) {
        return allocate_memory(device, info, allocator, memory);
    }
    VkMemoryAllocateInfo theirs = *info;
    theirs.pNext = named->pNext;
    const VkMemoryDedicatedAllocateInfo *dedicated = NULL;
    for (const VkBaseInStructure *next = theirs.pNext; next != NULL && dedicated == NULL; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            dedicated = (const VkMemoryDedicatedAllocateInfo *)next;
        }
    }
    if (dedicated == NULL || dedicated->image == VK_NULL_HANDLE || use_iosurface == NULL) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    struct apkrt_hardware_buffer_description description;
    IOSurfaceRef surface = buffer_surface(named->buffer, &description);
    if (surface == NULL) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    const uint64_t taken = image_take(dedicated->image, &description);
    if (taken == 0) {
        CFRelease(surface);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    const VkResult used = use_iosurface(dedicated->image, surface);
    CFRelease(surface);
    const VkResult result =
        used == VK_SUCCESS ? allocate_memory(device, &theirs, allocator, memory) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (result != VK_SUCCESS) {
        image_release(dedicated->image, taken);
    }
    return result;
}

void minato_graphics_vulkan_wrap(void **table, void *library) {
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
    WRAP(vkGetPastPresentationTimingGOOGLE, past_timing, past_timings_given)
    WRAP(vkCreateDebugUtilsMessengerEXT, create_messenger, messenger_created)
    WRAP(vkDestroyDebugUtilsMessengerEXT, destroy_messenger, messenger_destroyed)
    WRAP(vkCreateDebugReportCallbackEXT, create_report, report_created)
    WRAP(vkDestroyDebugReportCallbackEXT, destroy_report, report_destroyed)
    WRAP(vkAllocateMemory, allocate_memory, memory_allocated)
    WRAP(vkCreateImage, create_image, image_created)
    WRAP(vkDestroyImage, destroy_image, image_destroyed)
#undef WRAP
    use_iosurface = (VkResult(*)(VkImage, IOSurfaceRef))dlsym(library, "vkUseIOSurfaceMVK");
    table[MINATO_VULKAN_SLOT_vkCreateAndroidSurfaceKHR] = (void *)surface_created;
    get_queue = (PFN_vkGetDeviceQueue2)table[MINATO_VULKAN_SLOT_vkGetDeviceQueue2];
    device_function = (PFN_vkGetDeviceProcAddr)table[MINATO_VULKAN_SLOT_vkGetDeviceProcAddr];
    instance_function = (PFN_vkGetInstanceProcAddr)table[MINATO_VULKAN_SLOT_vkGetInstanceProcAddr];
}
