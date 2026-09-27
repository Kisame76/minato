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

#include "minato_bind.h"

/* The host function of a name written by hand: its stub in vulkan_stubs.S, which takes what the name
   takes. */
#define MINATO_HOST(name) extern __typeof__(name) minato_host_##name MINATO_HIDDEN

/* The extensions libvulkan implements itself (decision 11), as bits. */
#define MINATO_OWN_EXTENSIONS(X)                                                                    \
    X(VK_KHR_surface) X(VK_KHR_surface_protected_capabilities) X(VK_KHR_android_surface)           \
    X(VK_EXT_swapchain_colorspace) X(VK_KHR_get_surface_capabilities2) X(VK_GOOGLE_surfaceless_query) \
    X(VK_KHR_swapchain) X(VK_KHR_incremental_present)
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

/* A pNext chain without the structures of `types` (decisions 5 and 8): the app's structures up to the
   last one left out are copied into *copy, which the caller frees (NULL when nothing is left out); the
   rest stays the app's. Returns VK_SUCCESS with the new head in *head, VK_ERROR_OUT_OF_HOST_MEMORY, or
   VK_ERROR_INITIALIZATION_FAILED when a structure before one left out has a type of unknown size
   (vulkan_structs.inc). */
MINATO_HIDDEN VkResult minato_chain_without(const void *chain, const VkStructureType *types, size_t count,
                                            const void **head, void **copy);

/* What libvulkan keeps per VkInstance and per VkDevice, MoltenVK's handles: the own extensions the app
   enabled. minato_recall returns 1 and sets *owned for a handle it knows, else 0. */
MINATO_HIDDEN int minato_remember(const void *handle, uint32_t owned);
MINATO_HIDDEN int minato_recall(const void *handle, uint32_t *owned);
MINATO_HIDDEN void minato_forget(const void *handle);

#endif
