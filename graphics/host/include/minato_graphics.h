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
   path leaves its libs without a provider, so they answer "no driver" (the Simulator for Vulkan).
   Returns 0, or -1 when a path is longer than 1023 bytes or the runtime refused a provider. */
int minato_graphics_set_providers(const char *moltenvk, const char *angle);

#ifdef __cplusplus
}
#endif

#endif
