/* graphics/host: the host side of Minato's graphics for the app and the tests on the Mac (ADR-0029,
   ADR-0030). Plain C. */
#ifndef MINATO_GRAPHICS_H
#define MINATO_GRAPHICS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the providers of the front libs' host tables with the runtime (ADR-0030, decision 3), before the
   program starts: "vulkan" opens MoltenVK at `moltenvk`, "gles" and "egl" open ANGLE's libGLESv2 at
   `angle`, which holds EGL's functions as EGL_<name>. A provider fills its table from the library's
   exports; a name the library lacks gets a function that logs once and answers "not supported". A NULL
   path leaves its libs without a provider, so they answer "no driver" (the Simulator for Vulkan). With
   MoltenVK, it also sets MVK_CONFIG_LOG_LEVEL to warnings unless the environment has it.
   Returns 0, or -1 when a path is longer than 1023 bytes or the runtime refused a provider. */
int minato_graphics_set_providers(const char *moltenvk, const char *angle);

/* ADR-0030 V3's check for the iPhone, host code only: opens MoltenVK at `moltenvk` as the "vulkan"
   provider does, creates an instance at Vulkan 1.3 with VK_KHR_surface and VK_EXT_metal_surface and a
   Metal surface on a CAMetalLayer, prints what they report to stdout and destroys both. Returns 0, or 1
   after printing what failed to stderr. Its own object file: project.yml links it into debug builds
   only, and there, with MINATO_GRAPHICS_PROBE set, it runs before main on the MoltenVK in the app's
   Frameworks and the app exits with its result. */
int minato_graphics_probe(const char *moltenvk);

#ifdef __cplusplus
}
#endif

#endif
