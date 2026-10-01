/* libvulkan's own code (ADR-0030, decisions 10 to 13): the functions written by hand, which reach MoltenVK
   through the stubs minato_host_<name> of tools/frontgen's table. Guest code, built by
   tools/build-front-libs.sh against the Vulkan headers of MoltenVK's release (VK_HEADER_VERSION 357, the
   version of the vk.xml the table comes from), which define every structure its names take. */
#ifndef MINATO_LIBVULKAN_H
#define MINATO_LIBVULKAN_H

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

#include "APKRuntime/guest_interface.h"
#include "host_protocol.h"
#include "minato_bind.h"

/* The host function of a name written by hand: its stub in vulkan_stubs.S, which takes what the name
   takes. */
#define MINATO_HOST(name) extern __typeof__(name) minato_host_##name MINATO_HIDDEN

/* The extensions libvulkan implements itself (decision 11), as bits. */
#define MINATO_OWN_EXTENSIONS(X)                                                                    \
    X(VK_KHR_surface) X(VK_KHR_surface_protected_capabilities) X(VK_KHR_android_surface)           \
    X(VK_EXT_swapchain_colorspace) X(VK_KHR_get_surface_capabilities2) X(VK_GOOGLE_surfaceless_query) \
    X(VK_KHR_swapchain) X(VK_KHR_incremental_present) X(VK_GOOGLE_display_timing)
#define MINATO_OWN_BIT(name) MINATO_OWN_BIT_##name,
enum { MINATO_OWN_EXTENSIONS(MINATO_OWN_BIT) };
#undef MINATO_OWN_BIT
#define MINATO_OWNER(name) (1u << MINATO_OWN_BIT_##name)

/* An extension libvulkan may offer (vulkan_extensions.inc): its name, whether a device has it, and its
   bit when libvulkan implements it itself (else 0). */
struct minato_extension {
    const char *name;
    int device;
    uint32_t owner;
};
MINATO_HIDDEN const struct minato_extension *minato_extension(const char *name);

/* The Vulkan way of handing out a list: its length when `properties` is NULL, else as much of it as
   *count allows, with VK_INCOMPLETE when that was not all. */
MINATO_HIDDEN VkResult minato_hand_out(const VkExtensionProperties *list, uint32_t length, uint32_t *count,
                                       VkExtensionProperties *properties);

/* A pNext chain without the structures of the types in `removed` and with copies of those in `changed`,
   which the caller may change (decisions 5, 8, 11 and 13): the app's structures up to the last of either
   are copied into *copy, which the caller frees (NULL when the chain has neither); the rest stays the
   app's. Returns VK_SUCCESS with the new head in *head, VK_ERROR_OUT_OF_HOST_MEMORY, or
   VK_ERROR_INITIALIZATION_FAILED when a structure before the last of them has a type of unknown size
   (vulkan_structs.inc). */
MINATO_HIDDEN VkResult minato_chain_edit(const void *chain, const VkStructureType *removed, size_t removed_count,
                                         const VkStructureType *changed, size_t changed_count, const void **head,
                                         void **copy);

/* The first structure of `type` in a chain, or NULL. The caller changes only the copies minato_chain_edit
   made. */
MINATO_HIDDEN void *minato_chain_find(const void *chain, VkStructureType type);

/* A queue of a device libvulkan made (decision 13): MoltenVK's handle, its device and family, and the
   timeline semaphore whose signal libvulkan adds to every submit, at `last` for the last submit that
   succeeded. */
struct minato_queue {
    VkQueue queue;
    VkDevice device;
    uint32_t family;
    VkSemaphore timeline;
    uint64_t last;  /* read and written atomically */
};
/* A device's queues, with its physical device. */
struct minato_queues {
    VkPhysicalDevice physical;
    uint32_t count;
    struct minato_queue queue[];
};

/* The queues of a device MoltenVK made on `physical` from `info`, each with its timeline semaphore:
   VK_SUCCESS with *queues, or the error of the semaphore that failed. minato_queues_destroy destroys the
   semaphores. */
MINATO_HIDDEN VkResult minato_queues_create(VkDevice device, VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                            struct minato_queues **queues);
MINATO_HIDDEN void minato_queues_destroy(struct minato_queues *queues);

/* What libvulkan keeps per VkInstance and per VkDevice, MoltenVK's handles: the own extensions the app
   enabled, and a device's queues. minato_recall returns 1 and sets *owned and, when `queues` is not NULL,
   *queues for a handle it knows, else 0. minato_queue finds a queue of the devices libvulkan knows. */
MINATO_HIDDEN int minato_remember(const void *handle, uint32_t owned, struct minato_queues *queues);
MINATO_HIDDEN int minato_recall(const void *handle, uint32_t *owned, struct minato_queues **queues);
MINATO_HIDDEN void minato_forget(const void *handle);
MINATO_HIDDEN struct minato_queue *minato_queue(VkQueue queue);

/* A host wait in slices of at most 10 ms until it answers other than VK_TIMEOUT or `timeout` ns passed
   (decision 4). A timeout of one slice or less goes as it is. */
MINATO_HIDDEN VkResult minato_sliced(VkResult (*wait)(const void *arguments, uint64_t timeout),
                                     const void *arguments, uint64_t timeout);

/* The window's gate (ADR-0028, decision 11): the syscall window_gate of `kind` for the host window
   `window`, made again after a signal's handler ran. Returns 1 once the window is gone, else 0 (also for an
   answer this runtime does not give): at the present kind once the gate is open, at the background kind in
   the foreground, where window 0, for calls without a window, only waits. */
MINATO_HIDDEN int minato_gate(uint64_t window, int kind);

/* `result = call` through graphics/host's background gate (decision 13): while graphics/host backs out,
   waits at the window's gate of the background kind and calls again. VK_ERROR_SURFACE_LOST_KHR once
   `window` is gone. */
#define MINATO_GATED(result, window, call)                                                                         do {                                                                                                               (result) = (call);                                                                                         } while ((result) == MINATO_VK_BACKGROUND &&                                                                            ((result) = minato_gate((window), APKRT_GATE_BACKGROUND) ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS) ==                  VK_SUCCESS)

/* vkQueueSubmit with the queue's own timeline signal, through the background gate for `window` (0 for the
   app's submits). */
MINATO_HIDDEN VkResult minato_queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *batches, VkFence fence,
                                           uint64_t window);

/* A surface libvulkan made (decision 12), the app's VkSurfaceKHR: MoltenVK's surface on the window's layer,
   a reference to the app's window of its own, the host window's ID, and the swapchain made on it last,
   which alone is not replaced (read and written atomically). */
struct minato_surface {
    VkSurfaceKHR metal;
    struct ANativeWindow *window;
    uint64_t id;
    VkSwapchainKHR swapchain;
};

static inline struct minato_surface *minato_surface(VkSurfaceKHR surface) {
    return (struct minato_surface *)(uintptr_t)surface;
}

/* The surface's window's size in pixels as of now, which a turn changes (ADR-0038 c), whatever its buffer
   geometry; 0 x 0 when the window cannot tell. */
MINATO_HIDDEN VkExtent2D minato_window_size(const struct minato_surface *surface);

#endif
