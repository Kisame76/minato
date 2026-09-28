/* libGLESv2's and libGLESv3's own code (ADR-0029, decisions 14 and 16; ADR-0030, decision 4): the functions
   written by hand, which reach ANGLE through the stubs minato_host_<name> of tools/frontgen's table. Guest
   code, built by tools/build-front-libs.sh against the NDK's headers; the two libraries are one set of
   objects with two export lists. */
#ifndef MINATO_LIBGLES_H
#define MINATO_LIBGLES_H

#define GL_GLEXT_PROTOTYPES
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include "minato_bind.h"

/* The host function of a name written by hand: its stub in gles_stubs.S, which takes what the name takes. */
#define MINATO_HOST(name) extern __typeof__(name) minato_host_##name MINATO_HIDDEN

/* For libEGL, in version APKRT_PRIVATE (B1's departure from decision 13's exports): the function this lib
   hands out for a name of its table, the one written by hand or else the stub; NULL for any other name.
   libEGL's eglGetProcAddress finds it with dlsym on libGLESv2.so. */
void *minato_gles_proc(const char *name);

#endif
