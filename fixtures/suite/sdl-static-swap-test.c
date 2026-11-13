// sdl-static-swap-test: an app with SDL 2 linked into its own library, as Ren'Py's (the SDL swap override,
// runtime/android/sdl.c): libfake-sdl2-static.so's flip calls SDL_GL_SwapWindow's dynamic API stub directly, in the
// same library. With the argument "import" the program flips itself, through the stub's import, so that both of the
// override's ways meet: the import's and the dynamic API's. One line per flip, in the states of sdl-swap-test, each a
// pygame_sdl2 flip: the flip failed when SDL's error text is not empty after the swap.
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
const char *fake_app_flip(void);
void SDL_GL_SwapWindow(void *window);
const char *SDL_GetError(void);
void SDL_ClearError(void);

static int imported;

static void flip(const char *state, int paused, int current, int surface_lost) {
    fake_activity_set_paused(paused);
    fake_sdl_static_set_current(current);
    fake_sdl_static_set_surface_lost(surface_lost);
    const char *error;
    if (imported) {
        SDL_ClearError();
        SDL_GL_SwapWindow(fake_sdl_static_window());
        error = SDL_GetError();
    } else {
        error = fake_app_flip();
    }
    if (error[0] != '\0') {
        printf(PROGRAM ": %s: flip failed (%s)\n", state, error);
    } else {
        printf(PROGRAM ": %s: flip ok\n", state);
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
