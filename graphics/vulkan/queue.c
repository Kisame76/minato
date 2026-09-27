/* libvulkan's queues (ADR-0030, decisions 4 and 13): every submit that reaches MoltenVK signals the queue's
   own timeline semaphore last, so vkQueueWaitIdle and vkDeviceWaitIdle wait for its value instead of
   MoltenVK's functions, which commit; every wait goes to MoltenVK in slices of at most 10 ms, with a return
   to guest code between them, so a guest signal waits about 10 ms at most. The submits go through
   graphics/host's functions, which count the calls in MoltenVK for "scheduled". */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libvulkan.h"

MINATO_HOST(vkGetDeviceQueue);
MINATO_HOST(vkGetDeviceQueue2);
MINATO_HOST(vkQueueSubmit);
MINATO_HOST(vkQueueSubmit2);
MINATO_HOST(vkQueueBindSparse);
MINATO_HOST(vkQueueWaitIdle);
MINATO_HOST(vkDeviceWaitIdle);
MINATO_HOST(vkWaitForFences);
MINATO_HOST(vkWaitSemaphores);
MINATO_HOST(vkGetQueryPoolResults);

#define SLICE 10000000u /* ns */

VkResult minato_queues_create(VkDevice device, const VkDeviceCreateInfo *info, struct minato_queues **queues) {
    uint32_t count = 0;
    for (uint32_t family = 0; family < info->queueCreateInfoCount; family++) {
        count += info->pQueueCreateInfos[family].queueCount;
    }
    *queues = calloc(1, sizeof **queues + count * sizeof (*queues)->queue[0]);
    if (*queues == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    for (uint32_t family = 0; family < info->queueCreateInfoCount; family++) {
        const VkDeviceQueueCreateInfo *created = &info->pQueueCreateInfos[family];
        for (uint32_t index = 0; index < created->queueCount; index++) {
            const VkDeviceQueueInfo2 which = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .flags = created->flags,
                                              .queueFamilyIndex = created->queueFamilyIndex, .queueIndex = index};
            struct minato_queue *queue = &(*queues)->queue[(*queues)->count];
            minato_host_vkGetDeviceQueue2(device, &which, &queue->queue);
            queue->device = device;
            const VkResult result = vkCreateSemaphore(device, &semaphore, NULL, &queue->timeline);
            if (result != VK_SUCCESS) {
                return result;
            }
            (*queues)->count++;
        }
    }
    return VK_SUCCESS;
}

void minato_queues_destroy(struct minato_queues *queues) {
    for (uint32_t index = 0; queues != NULL && index < queues->count; index++) {
        vkDestroySemaphore(queues->queue[index].device, queues->queue[index].timeline, NULL);
    }
    free(queues);
}

void vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue) {
    minato_host_vkGetDeviceQueue(device, family, index, queue);
}

void vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue) {
    minato_host_vkGetDeviceQueue2(device, info, queue);
}

/* The memory of a batch whose signals libvulkan extended. */
struct signal {
    void *copy;              /* its chain up to the structures changed */
    VkSemaphore *semaphores; /* its signals, then the values and device indices that match them */
    VkTimelineSemaphoreSubmitInfo values; /* for a batch without one */
};

static const VkStructureType submit_values[] = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};

/* Adds the queue's timeline semaphore at `value` to the signals of a batch of vkQueueSubmit or
   vkQueueBindSparse: *next, *count and *semaphores become copies with it last, and the batch's
   VkTimelineSemaphoreSubmitInfo (or libvulkan's) and VkDeviceGroupSubmitInfo name as many values and
   devices. VK_ERROR_INITIALIZATION_FAILED when the chain cannot be copied. */
static VkResult add_signal(struct signal *signal, const void **next, uint32_t *count, const VkSemaphore **semaphores,
                           VkSemaphore timeline, uint64_t value) {
    const uint32_t old = *count;
    const void *chain;
    VkResult result = minato_chain_edit(*next, NULL, 0, submit_values, 2, &chain, &signal->copy);
    if (result != VK_SUCCESS) {
        return result;
    }
    signal->semaphores = malloc((old + 1) * (sizeof(VkSemaphore) + sizeof(uint64_t) + sizeof(uint32_t)));
    if (signal->semaphores == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint64_t *values = (uint64_t *)(signal->semaphores + old + 1);
    uint32_t *devices = (uint32_t *)(values + old + 1);
    VkTimelineSemaphoreSubmitInfo *timeline_values =
        minato_chain_find(chain, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
    VkDeviceGroupSubmitInfo *group = minato_chain_find(chain, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO);
    for (uint32_t index = 0; index < old; index++) {
        signal->semaphores[index] = (*semaphores)[index];
        values[index] = timeline_values != NULL && index < timeline_values->signalSemaphoreValueCount
                            ? timeline_values->pSignalSemaphoreValues[index]
                            : 0;
        devices[index] =
            group != NULL && index < group->signalSemaphoreCount ? group->pSignalSemaphoreDeviceIndices[index] : 0;
    }
    signal->semaphores[old] = timeline;
    values[old] = value;
    devices[old] = 0;
    if (timeline_values == NULL) {
        signal->values = (VkTimelineSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                         .pNext = chain};
        timeline_values = &signal->values;
        chain = timeline_values;
    }
    timeline_values->signalSemaphoreValueCount = old + 1;
    timeline_values->pSignalSemaphoreValues = values;
    if (group != NULL) {
        group->signalSemaphoreCount = old + 1;
        group->pSignalSemaphoreDeviceIndices = devices;
    }
    *next = chain;
    *count = old + 1;
    *semaphores = signal->semaphores;
    return VK_SUCCESS;
}

static uint64_t next_value(const struct minato_queue *queue) {
    return __atomic_load_n(&queue->last, __ATOMIC_RELAXED) + 1;
}

/* A submit to MoltenVK that signals the queue's timeline semaphore last, in the last batch or in a batch of
   its own when there is none or its chain cannot be copied. */
#define SUBMIT_WITH_SIGNAL(host, Batch, kind)                                                                 \
    struct minato_queue *ours = minato_queue(queue);                                                          \
    if (ours == NULL || (count == 0 && fence == VK_NULL_HANDLE)) {                                            \
        return host(queue, count, batches, fence);                                                            \
    }                                                                                                         \
    const uint64_t value = next_value(ours);                                                                  \
    struct signal signal = {0};                                                                               \
    Batch *copies = malloc((count + 1) * sizeof *copies);                                                     \
    if (copies == NULL) {                                                                                     \
        return VK_ERROR_OUT_OF_HOST_MEMORY;                                                                   \
    }                                                                                                         \
    if (count > 0) {                                                                                          \
        memcpy(copies, batches, count * sizeof *copies);                                                      \
    }                                                                                                         \
    uint32_t total = count;                                                                                   \
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;                                                         \
    if (count > 0) {                                                                                          \
        Batch *last = &copies[count - 1];                                                                     \
        result = add_signal(&signal, &last->pNext, &last->signalSemaphoreCount, &last->pSignalSemaphores,     \
                            ours->timeline, value);                                                           \
    }                                                                                                         \
    if (result == VK_ERROR_INITIALIZATION_FAILED) {                                                           \
        copies[total] = (Batch){.sType = kind};                                                               \
        result = add_signal(&signal, &copies[total].pNext, &copies[total].signalSemaphoreCount,               \
                            &copies[total].pSignalSemaphores, ours->timeline, value);                         \
        total++;                                                                                              \
    }                                                                                                         \
    if (result == VK_SUCCESS) {                                                                               \
        result = host(queue, total, copies, fence);                                                           \
    }                                                                                                         \
    if (result == VK_SUCCESS) {                                                                               \
        __atomic_store_n(&ours->last, value, __ATOMIC_RELAXED);                                               \
    }                                                                                                         \
    free(signal.copy);                                                                                        \
    free(signal.semaphores);                                                                                  \
    free(copies);                                                                                             \
    return result;

VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *batches, VkFence fence) {
    SUBMIT_WITH_SIGNAL(minato_host_vkQueueSubmit, VkSubmitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO)
}

VkResult vkQueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo *batches, VkFence fence) {
    SUBMIT_WITH_SIGNAL(minato_host_vkQueueBindSparse, VkBindSparseInfo, VK_STRUCTURE_TYPE_BIND_SPARSE_INFO)
}

VkResult vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *batches, VkFence fence) {
    struct minato_queue *ours = minato_queue(queue);
    if (ours == NULL || (count == 0 && fence == VK_NULL_HANDLE)) {
        return minato_host_vkQueueSubmit2(queue, count, batches, fence);
    }
    const uint64_t value = next_value(ours);
    const uint32_t total = count > 0 ? count : 1;
    const uint32_t signals = count > 0 ? batches[count - 1].signalSemaphoreInfoCount : 0;
    VkSubmitInfo2 *copies = malloc(total * sizeof *copies);
    VkSemaphoreSubmitInfo *infos = malloc((signals + 1) * sizeof *infos);
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (copies != NULL && infos != NULL) {
        if (count > 0) {
            memcpy(copies, batches, count * sizeof *copies);
        } else {
            copies[0] = (VkSubmitInfo2){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        }
        VkSubmitInfo2 *last = &copies[total - 1];
        if (signals > 0) {
            memcpy(infos, last->pSignalSemaphoreInfos, signals * sizeof *infos);
        }
        infos[signals] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                 .semaphore = ours->timeline, .value = value,
                                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        last->signalSemaphoreInfoCount = signals + 1;
        last->pSignalSemaphoreInfos = infos;
        result = minato_host_vkQueueSubmit2(queue, total, copies, fence);
    }
    if (result == VK_SUCCESS) {
        __atomic_store_n(&ours->last, value, __ATOMIC_RELAXED);
    }
    free(infos);
    free(copies);
    return result;
}

static uint64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

/* A host wait in slices of at most 10 ms until it answers other than VK_TIMEOUT or `timeout` ns passed
   (decision 4). A timeout of one slice or less goes as it is. */
static VkResult sliced(VkResult (*wait)(const void *arguments, uint64_t timeout), const void *arguments,
                       uint64_t timeout) {
    if (timeout <= SLICE) {
        return wait(arguments, timeout);
    }
    const uint64_t start = now();
    const uint64_t deadline = timeout > UINT64_MAX - start ? UINT64_MAX : start + timeout;
    for (uint64_t at = start; at < deadline; at = now()) {
        const VkResult result = wait(arguments, deadline - at < SLICE ? deadline - at : SLICE);
        if (result != VK_TIMEOUT) {
            return result;
        }
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
    return minato_host_vkWaitForFences(fences->device, fences->count, fences->fences, fences->all, timeout);
}

VkResult vkWaitForFences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
    const struct fences arguments = {device, count, fences, all};
    return sliced(wait_fences, &arguments, timeout);
}

struct semaphores {
    VkDevice device;
    const VkSemaphoreWaitInfo *info;
};

static VkResult wait_semaphores(const void *arguments, uint64_t timeout) {
    const struct semaphores *semaphores = arguments;
    return minato_host_vkWaitSemaphores(semaphores->device, semaphores->info, timeout);
}

VkResult vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    const struct semaphores arguments = {device, info};
    return sliced(wait_semaphores, &arguments, timeout);
}

/* Until the timeline semaphores reach their values, without a timeout. */
static VkResult wait_values(VkDevice device, uint32_t count, const VkSemaphore *timelines, const uint64_t *values) {
    const VkSemaphoreWaitInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = count,
                                      .pSemaphores = timelines, .pValues = values};
    return vkWaitSemaphores(device, &info, UINT64_MAX);
}

VkResult vkQueueWaitIdle(VkQueue queue) {
    const struct minato_queue *ours = minato_queue(queue);
    if (ours == NULL) {
        return minato_host_vkQueueWaitIdle(queue);
    }
    const uint64_t last = __atomic_load_n(&ours->last, __ATOMIC_RELAXED);
    return last == 0 ? VK_SUCCESS : wait_values(ours->device, 1, &ours->timeline, &last);
}

VkResult vkDeviceWaitIdle(VkDevice device) {
    uint32_t owned;
    struct minato_queues *queues = NULL;
    if (!minato_recall(device, &owned, &queues) || queues == NULL) {
        return minato_host_vkDeviceWaitIdle(device);
    }
    VkSemaphore *timelines = malloc(queues->count * (sizeof *timelines + sizeof(uint64_t)));
    if (timelines == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint64_t *values = (uint64_t *)(timelines + queues->count);
    uint32_t count = 0;
    for (uint32_t index = 0; index < queues->count; index++) {
        const uint64_t last = __atomic_load_n(&queues->queue[index].last, __ATOMIC_RELAXED);
        if (last != 0) {
            timelines[count] = queues->queue[index].timeline;
            values[count++] = last;
        }
    }
    const VkResult result = count == 0 ? VK_SUCCESS : wait_values(device, count, timelines, values);
    free(timelines);
    return result;
}

VkResult vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                               void *data, VkDeviceSize stride, VkQueryResultFlags flags) {
    if (!(flags & VK_QUERY_RESULT_WAIT_BIT)) {
        return minato_host_vkGetQueryPoolResults(device, pool, first, count, size, data, stride, flags);
    }
    /* MoltenVK waits without a timeout: polled without the wait, and without partial results, which matter
       only without it, with sleeps in guest code from 64 us to 1 ms between the polls. */
    const VkQueryResultFlags polled =
        flags & ~(VkQueryResultFlags)(VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
    for (long pause = 64000;; pause = pause < 1000000 ? pause * 2 : 1000000) {
        const VkResult result =
            minato_host_vkGetQueryPoolResults(device, pool, first, count, size, data, stride, polled);
        if (result != VK_NOT_READY) {
            return result;
        }
        const struct timespec sleep = {0, pause};
        nanosleep(&sleep, NULL);
    }
}
