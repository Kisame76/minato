/* libvulkan's shared pieces: its extension list, handing out lists, copies of pNext chains and what it keeps
   per instance, device and queue; see libvulkan.h. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "libvulkan.h"

#define MINATO_EXTENSION(name, kind, owner) {#name, MINATO_##kind, owner},
enum { MINATO_INSTANCE, MINATO_DEVICE };
static const struct minato_extension extensions[] = {
#include "vulkan_extensions.inc"
};
#undef MINATO_EXTENSION

static int by_name(const void *key, const void *element) {
    return strcmp(key, ((const struct minato_extension *)element)->name);
}

const struct minato_extension *minato_extension(const char *name) {
    return bsearch(name, extensions, sizeof extensions / sizeof *extensions, sizeof *extensions, by_name);
}

VkResult minato_hand_out(const VkExtensionProperties *list, uint32_t length, uint32_t *count,
                         VkExtensionProperties *properties) {
    if (properties == NULL) {
        *count = length;
        return VK_SUCCESS;
    }
    const uint32_t copied = *count < length ? *count : length;
    memcpy(properties, list, copied * sizeof *list);
    *count = copied;
    return copied < length ? VK_INCOMPLETE : VK_SUCCESS;
}

/* The sizes of the structures that may extend the chains libvulkan copies. */
#define MINATO_STRUCT(type, value) {value, sizeof(type)},
static const struct {
    VkStructureType type;
    size_t size;
} sizes[] = {
#include "vulkan_structs.inc"
};
#undef MINATO_STRUCT

static size_t size_of(VkStructureType type) {
    for (size_t index = 0; index < sizeof sizes / sizeof *sizes; index++) {
        if (sizes[index].type == type) {
            return sizes[index].size;
        }
    }
    return 0;
}

static int listed(const VkBaseInStructure *structure, const VkStructureType *types, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (structure->sType == types[index]) {
            return 1;
        }
    }
    return 0;
}

VkResult minato_chain_edit(const void *chain, const VkStructureType *removed, size_t removed_count,
                           const VkStructureType *changed, size_t changed_count, const void **head, void **copy) {
    /* The last structure removed or changed, the room for copies of the ones kept up to it, and whether one
       of those has a size unknown here. */
    const VkBaseInStructure *last = NULL;
    size_t room = 0, kept = 0;
    int unknown = 0, unknown_before_last = 0;
    for (const VkBaseInStructure *structure = chain; structure != NULL; structure = structure->pNext) {
        const int left_out = listed(structure, removed, removed_count);
        if (!left_out) {
            const size_t size = size_of(structure->sType);
            unknown |= size == 0;
            kept += size;
        }
        if (left_out || listed(structure, changed, changed_count)) {
            last = structure;
            room = kept;
            unknown_before_last = unknown;
        }
    }
    *head = chain;
    *copy = NULL;
    if (last == NULL) {
        return VK_SUCCESS;
    }
    if (unknown_before_last) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    char *memory = NULL;
    if (room != 0 && (memory = malloc(room)) == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    /* Copies of the kept structures up to the last one removed or changed, linked in their order, then the
       rest of the chain as it is. The sizes are multiples of 8, so every copy stays aligned. */
    VkBaseOutStructure *previous = NULL;
    char *next = memory;
    for (const VkBaseInStructure *structure = chain; structure != last->pNext; structure = structure->pNext) {
        if (listed(structure, removed, removed_count)) {
            continue;
        }
        const size_t size = size_of(structure->sType);
        VkBaseOutStructure *copied = memcpy(next, structure, size);
        if (previous == NULL) {
            *head = copied;
        } else {
            previous->pNext = copied;
        }
        previous = copied;
        next += size;
    }
    if (previous == NULL) {
        *head = last->pNext;
    } else {
        previous->pNext = (VkBaseOutStructure *)last->pNext;
    }
    *copy = memory;
    return VK_SUCCESS;
}

void *minato_chain_find(const void *chain, VkStructureType type) {
    for (const VkBaseInStructure *structure = chain; structure != NULL; structure = structure->pNext) {
        if (structure->sType == type) {
            return (void *)structure;
        }
    }
    return NULL;
}

/* The handles libvulkan knows, few in any app: a list under a lock. */
struct known {
    struct known *next;
    const void *handle;
    uint32_t owned;
    struct minato_queues *queues;  /* a device's, else NULL */
};
static struct known *known;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int minato_remember(const void *handle, uint32_t owned, struct minato_queues *queues) {
    struct known *entry = malloc(sizeof *entry);
    if (entry == NULL) {
        return -1;
    }
    entry->handle = handle;
    entry->owned = owned;
    entry->queues = queues;
    pthread_mutex_lock(&lock);
    entry->next = known;
    known = entry;
    pthread_mutex_unlock(&lock);
    return 0;
}

int minato_recall(const void *handle, uint32_t *owned, struct minato_queues **queues) {
    int found = 0;
    pthread_mutex_lock(&lock);
    for (const struct known *entry = known; entry != NULL; entry = entry->next) {
        if (entry->handle == handle) {
            *owned = entry->owned;
            if (queues != NULL) {
                *queues = entry->queues;
            }
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return found;
}

struct minato_queue *minato_queue(VkQueue queue) {
    struct minato_queue *found = NULL;
    pthread_mutex_lock(&lock);
    for (const struct known *entry = known; entry != NULL && found == NULL; entry = entry->next) {
        for (uint32_t index = 0; entry->queues != NULL && index < entry->queues->count; index++) {
            if (entry->queues->queue[index].queue == queue) {
                found = &entry->queues->queue[index];
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return found;
}

void minato_forget(const void *handle) {
    pthread_mutex_lock(&lock);
    for (struct known **entry = &known; *entry != NULL; entry = &(*entry)->next) {
        if ((*entry)->handle == handle) {
            struct known *gone = *entry;
            *entry = gone->next;
            free(gone);
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}
