/* libvulkan's queue path for MinatoBench (ADR-0030 V5, the queue path's A/B): what libvulkan changes in the calls
   MoltenVK gets (graphics/vulkan/device.c and queue.c), put in front of MoltenVK's vkGetInstanceProcAddr, so that
   the native build asks MoltenVK what the APK asks:
   - vkCreateDevice enables timeline semaphores and VK_EXT_metal_objects and makes a timeline semaphore per queue;
   - vkQueueSubmit signals the queue's timeline last, at the count of the queue's submits: in the last batch when
     its chain is empty, else in a batch of its own after the others;
   - vkWaitForFences and vkAcquireNextImageKHR wait in slices of at most 10 ms.
   vkQueueWaitIdle and vkDeviceWaitIdle stay MoltenVK's; the scene waits for its device only before and after its
   frames. One device at a time, as the scene makes, whose vkDestroyDevice logs the submits, the timelines' values,
   and the fence waits and acquires with the slices they took. */
#ifndef BENCH_QUEUE_PATH_H
#define BENCH_QUEUE_PATH_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

/* The vkGetInstanceProcAddr with libvulkan's queue path in front of `moltenvk`, MoltenVK's own. */
PFN_vkGetInstanceProcAddr bench_queue_path(PFN_vkGetInstanceProcAddr moltenvk);

#endif
