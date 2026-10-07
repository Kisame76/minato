/* graphics/host's functions in front of ANGLE's for libEGL and libGLES (angle.m), for the "egl" and "gles"
   providers (providers.c). */
#ifndef MINATO_ANGLE_HOST_H
#define MINATO_ANGLE_HOST_H

#include <stdint.h>

/* Put graphics/host's functions into the slots of libEGL's and libGLES's tables whose ANGLE functions they
   stand in front of, keep those to call, and set ANGLE's commit gate (patch 0003) from `library`, ANGLE's
   libGLESv2 as the provider opened it. The tables hold ANGLE's functions. */
void minato_graphics_egl_wrap(void **table, void *library);
void minato_graphics_gles_wrap(void **table, void *library);

/* "Scheduled" for the runtime (apkrt_add_scheduled_provider): waits until no command buffer is between
   ANGLE's commit gate and its count, then until every one ANGLE committed is scheduled (patch 0003's
   ANGLEMetalWaitUntilScheduled); 1 at once while no front lib opened ANGLE. */
int minato_graphics_angle_provider(void *context, uint64_t deadline);

/* The session's choice (angle_vulkan.m): 1 when ANGLE runs on its Vulkan backend, the ES 3.1 path. */
int minato_graphics_angle_vulkan_backend(void);

/* graphics/host's vkGetInstanceProcAddr for ANGLE's Vulkan backend (patch 0009), as a plain pointer, since angle.m
   has no Vulkan types; NULL without MoltenVK. */
void *minato_graphics_angle_vulkan_loader(void);

#endif
