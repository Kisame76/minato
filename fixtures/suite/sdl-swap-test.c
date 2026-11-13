// sdl-swap-test: an SDL 2 app's flips around a pause (the SDL swap override, runtime/android/sdl.c). Without an
// argument libfake-sdl2.so is linked in and SDL_GL_SwapWindow is imported, as pygame_sdl2 does; "dlsym" opens the
// library and looks the name up. One line per flip, in one state each: the activity running or paused, the window's
// context current or none (SDL's backup at a pause), EGL's surface there or lost. A flip is pygame_sdl2's (display.pyx
// Window.flip): SDL_ClearError, SDL_GL_SwapWindow, and the flip failed when SDL_GetError is not empty; SDL 2's swap
// returns nothing. Expected output:
// fixtures/expected/sdl-swap-test.out, as Android prints it; runtime/tests/sdl_swap_run.py has the line the override
// changes. Built a second time as sdl3-swap-test (FAKE_SDL3) against libfake-sdl3.so: an SDL 3 app, whose swap returns
// a bool and whose flip failed when it is false, with SDL_GetError's text; the override leaves SDL 3 alone, so its
// expected output is the same with and without the override. And as sdl3-preview-swap-test (FAKE_SDL3_PREVIEW too)
// against libfake-sdl3-preview.so, an early SDL 3 preview.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#if defined(FAKE_SDL3_PREVIEW)
#define PROGRAM "sdl3-preview-swap-test"
#define LIBRARY "libfake-sdl3-preview.so"
typedef _Bool Swap;
#elif defined(FAKE_SDL3)
#define PROGRAM "sdl3-swap-test"
#define LIBRARY "libfake-sdl3.so"
typedef _Bool Swap;
#else
#define PROGRAM "sdl-swap-test"
#define LIBRARY "libfake-sdl2.so"
typedef void Swap;
#endif

void *fake_sdl_window(void);
void fake_sdl_set_current(int current);
void fake_sdl_set_surface_lost(int lost);
void fake_activity_set_paused(int paused);
Swap SDL_GL_SwapWindow(void *window);
const char *SDL_GetError(void);
void SDL_ClearError(void);

static Swap (*swap_window)(void *window) = SDL_GL_SwapWindow;

static void flip(const char *state, int paused, int current, int surface_lost) {
    fake_activity_set_paused(paused);
    fake_sdl_set_current(current);
    fake_sdl_set_surface_lost(surface_lost);
    SDL_ClearError();
#ifdef FAKE_SDL3
    const int failed = !swap_window(fake_sdl_window());
#else
    swap_window(fake_sdl_window());
    const int failed = SDL_GetError()[0] != '\0';
#endif
    if (failed) {
        printf(PROGRAM ": %s: flip failed (%s)\n", state, SDL_GetError());
    } else {
        printf(PROGRAM ": %s: flip ok\n", state);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "dlsym") == 0) {
        void *library = dlopen(LIBRARY, RTLD_NOW);
        if (library == NULL) {
            printf(PROGRAM ": %s\n", dlerror());
            return 1;
        }
        swap_window = (Swap (*)(void *))dlsym(library, "SDL_GL_SwapWindow");
        if (swap_window == NULL) {
            printf(PROGRAM ": SDL_GL_SwapWindow is missing\n");
            return 1;
        }
    }
    flip("running, context current", 0, 1, 0);
    flip("running, no context current", 0, 0, 0);
    flip("paused, context current, surface lost", 1, 1, 1);
    flip("paused, no context current", 1, 0, 0);
    flip("paused, context current", 1, 1, 0);
    return 0;
}
