/* What libvulkan's gate relies on (ADR-0030, decision 8), for MoltenVK whatever its environment variables say:
   synchronous submits, no prefilled command buffers, MTLEvent semaphores (MoltenVK's style "where safe", which is
   MTLEvent on Apple's GPUs) and Vulkan 1.3 advertised, so that devices report it (decision 11). Shared with the
   parity benchmark's native build (ADR-0037, decision 1), which gives MoltenVK the same through
   VK_EXT_layer_settings. */
#ifndef MINATO_MOLTENVK_SETTINGS_H
#define MINATO_MOLTENVK_SETTINGS_H

#include <vulkan/vulkan.h>

static const VkBool32 minato_synchronous = VK_TRUE;
static const int32_t minato_no_prefill = 0, minato_metal_events = 1;
static const uint32_t minato_version_1_3 = VK_API_VERSION_1_3;
static const VkLayerSettingEXT minato_moltenvk_settings[] = {
    {"MoltenVK", "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &minato_synchronous},
    {"MoltenVK", "MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS", VK_LAYER_SETTING_TYPE_INT32_EXT, 1, &minato_no_prefill},
    {"MoltenVK", "MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE", VK_LAYER_SETTING_TYPE_INT32_EXT, 1, &minato_metal_events},
    {"MoltenVK", "MVK_CONFIG_API_VERSION_TO_ADVERTISE", VK_LAYER_SETTING_TYPE_UINT32_EXT, 1, &minato_version_1_3},
};

#endif
