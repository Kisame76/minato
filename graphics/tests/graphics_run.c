/* graphics-run: apk-run (runtime/tools/apk_run.cpp, built again here) with graphics/host's providers of
   the front libs' tables (ADR-0030, decision 3), for test 4 and the graphics tests on the Mac. Before
   main, it sets them from the environment: MINATO_MOLTENVK is the library the "vulkan" provider opens,
   MINATO_ANGLE the one the "gles" and "egl" providers open (ANGLE's libGLESv2). Without the variable,
   those libs have no provider and answer "no driver". */
#include <stdio.h>
#include <stdlib.h>

#include "minato_graphics.h"

__attribute__((constructor)) static void set_providers(void) {
    if (minato_graphics_set_providers(getenv("MINATO_MOLTENVK"), getenv("MINATO_ANGLE")) != 0) {
        fprintf(stderr, "graphics-run: the runtime refused graphics/host's providers\n");
        exit(2);
    }
}
