/* graphics/host's functions in front of MoltenVK's for libvulkan (vulkan.m), for the "vulkan" provider
   (providers.c). */
#ifndef MINATO_VULKAN_HOST_H
#define MINATO_VULKAN_HOST_H

/* Puts graphics/host's functions into the slots of libvulkan's table whose MoltenVK functions they stand
   in front of, and keeps those to call. The table holds MoltenVK's functions. */
void minato_graphics_vulkan_wrap(void **table);

#endif
