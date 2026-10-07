/* graphics-run: apk-run (runtime/tools/apk_run.cpp, built again here) with graphics/host's providers of
   the front libs' tables (ADR-0030, decision 3), for test 4 and the graphics tests on the Mac. Before
   main, it sets them from the environment: MINATO_MOLTENVK is the library the "vulkan" provider opens,
   MINATO_ANGLE the one the "gles" and "egl" providers open (ANGLE's libGLESv2). Without the variable,
   those libs have no provider and answer "no driver". The windows' buffers for drawing by the CPU come with them,
   or window_host.m's fake ones with MINATO_WINDOW_BUFFERS=fake. MINATO_ANGLE_BACKEND=vulkan puts ANGLE on its Vulkan
   backend, the ES 3.1 path (ADR-0044 c)), once the run-time check passed, as the session does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minato_graphics.h"
#include "test_host.h"

/* So that apk-run's --lifecycle windows take the buffers set below, not apk-run's headless ones (ADR-0043 c3). */
int apk_run_sets_window_buffers(void);
int apk_run_sets_window_buffers(void) {
    return 1;
}

__attribute__((constructor)) static void set_providers(void) {
    if (minato_graphics_set_providers(getenv("MINATO_MOLTENVK"), getenv("MINATO_ANGLE")) != 0) {
        fprintf(stderr, "graphics-run: the runtime refused graphics/host's providers\n");
        exit(2);
    }
    minato_test_use_fake_buffers();
    const char *backend = getenv("MINATO_ANGLE_BACKEND");
    if (backend != NULL && strcmp(backend, "vulkan") == 0) {
        char reason[256] = "";
        if (minato_graphics_angle_vulkan_check(0, reason, sizeof reason) != 1) {
            fprintf(stderr, "graphics-run: the run-time check refuses the ES 3.1 path: %s\n", reason);
            exit(2);
        }
        if (minato_graphics_set_angle_backend(1) != 0) {
            fprintf(stderr, "graphics-run: graphics/host took no Vulkan backend for ANGLE\n");
            exit(2);
        }
    }
}
