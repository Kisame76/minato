/* vkGetInstanceProcAddr and vkGetDeviceProcAddr (ADR-0030, decision 10): only libvulkan's own functions,
   the stubs and the ones written by hand, never MoltenVK's pointers. A name gives NULL above Vulkan 1.3
   (tools/frontgen leaves those out), at the wrong level, for an own extension the app did not enable, and
   otherwise when MoltenVK gives NULL for it: for an extension not enabled, or a function MoltenVK lacks
   (decision 3). */
#include <stdlib.h>
#include <string.h>

#include "libvulkan.h"

MINATO_HOST(vkGetInstanceProcAddr);
MINATO_HOST(vkGetDeviceProcAddr);

enum level { LEVEL_GLOBAL, LEVEL_INSTANCE, LEVEL_DEVICE };
struct proc {
    const char *name;
    PFN_vkVoidFunction function;
    enum level level;
    uint32_t owners;  /* the own extensions that bring it */
};
#define MINATO_PROC(name, function, level, owners) {#name, (PFN_vkVoidFunction)function, LEVEL_##level, owners},
static const struct proc procs[] = {
#include "vulkan_procs.inc"
};
#undef MINATO_PROC

/* The own extensions of a device; their functions are there for an instance before any device. */
#define OWN_DEVICE_EXTENSIONS (MINATO_OWNER(VK_KHR_swapchain) | MINATO_OWNER(VK_KHR_incremental_present))

static int by_name(const void *key, const void *element) {
    return strcmp(key, ((const struct proc *)element)->name);
}

static const struct proc *find(const char *name) {
    return name != NULL ? bsearch(name, procs, sizeof procs / sizeof *procs, sizeof *procs, by_name) : NULL;
}

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    const struct proc *proc = find(name);
    uint32_t owned;
    if (proc == NULL) {
        return NULL;
    }
    if (proc->function == (PFN_vkVoidFunction)vkGetInstanceProcAddr) {
        return proc->function;
    }
    /* Without an instance, the global functions only; with one, no global function (as Android). */
    if (instance == VK_NULL_HANDLE || proc->level == LEVEL_GLOBAL) {
        return instance == VK_NULL_HANDLE && proc->level == LEVEL_GLOBAL ? proc->function : NULL;
    }
    if (!minato_recall(instance, &owned, NULL)) {
        return NULL;
    }
    if (proc->owners != 0) {
        return proc->owners & (owned | OWN_DEVICE_EXTENSIONS) ? proc->function : NULL;
    }
    return minato_host_vkGetInstanceProcAddr(instance, name) != NULL ? proc->function : NULL;
}

PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *name) {
    const struct proc *proc = find(name);
    uint32_t owned;
    if (proc == NULL || proc->level != LEVEL_DEVICE || !minato_recall(device, &owned, NULL)) {
        return NULL;
    }
    if (proc->owners != 0) {
        return proc->owners & owned ? proc->function : NULL;
    }
    return minato_host_vkGetDeviceProcAddr(device, name) != NULL ? proc->function : NULL;
}
