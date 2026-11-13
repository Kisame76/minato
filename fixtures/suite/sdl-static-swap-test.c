// sdl-static-swap-test: an app with SDL 2 linked into its own library, as Ren'Py's (the SDL swap override,
// runtime/android/sdl.c): libfake-sdl2-static.so's flip calls SDL_GL_SwapWindow's dynamic API stub directly, in the
// same library. With the argument "import" the program calls the stub itself through its import instead, so that both
// of the override's ways meet: the import's and the dynamic API's. One line per flip, in the states of sdl-swap-test.
// Built twice: as sdl-static-swap-test and, with a library whose stub has another shape, as sdl-odd-swap-test
// (PROGRAM).
// Expected output: fixtures/expected/<program>.out, as Android prints it; runtime/tests/sdl_swap_run.py has the line
// the override changes.
#include <stdio.h>
#include <string.h>

#ifndef PROGRAM
#define PROGRAM "sdl-static-swap-test"
#endif

void *fake_sdl_static_window(void);
void fake_sdl_static_set_current(int current);
void fake_sdl_static_set_surface_lost(int lost);
void fake_activity_set_paused(int paused);
int fake_app_flip(void);
int SDL_GL_SwapWindow(void *window);
const char *SDL_GetError(void);

static int imported;

static void flip(const char *state, int paused, int current, int surface_lost) {
    fake_activity_set_paused(paused);
    fake_sdl_static_set_current(current);
    fake_sdl_static_set_surface_lost(surface_lost);
    int answer = imported ? SDL_GL_SwapWindow(fake_sdl_static_window()) : fake_app_flip();
    if (answer < 0) {
        printf(PROGRAM ": %s: swap %d (%s)\n", state, answer, SDL_GetError());
    } else {
        printf(PROGRAM ": %s: swap %d\n", state, answer);
    }
}

int main(int argc, char **argv) {
    imported = argc > 1 && strcmp(argv[1], "import") == 0;
    flip("running, context current", 0, 1, 0);
    flip("running, no context current", 0, 0, 0);
    flip("paused, context current, surface lost", 1, 1, 1);
    flip("paused, no context current", 1, 0, 0);
    flip("paused, context current", 1, 1, 0);
    return 0;
}
