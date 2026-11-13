// sdl-swap-test: an SDL 2 app's flips around a pause (the SDL swap override, runtime/android/sdl.c). Without an
// argument libfake-sdl2.so is linked in and SDL_GL_SwapWindow is imported, as pygame_sdl2 does; "dlsym" opens the
// library and looks the name up. One line per flip, in one state each: the activity running or paused, the window's
// context current or none (SDL's backup at a pause), EGL's surface there or lost. Expected output:
// fixtures/expected/sdl-swap-test.out, as Android prints it; runtime/tests/sdl_swap_run.py has the line the override
// changes.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

void *fake_sdl_window(void);
void fake_sdl_set_current(int current);
void fake_sdl_set_surface_lost(int lost);
void fake_activity_set_paused(int paused);
int SDL_GL_SwapWindow(void *window);
const char *SDL_GetError(void);

static int (*swap_window)(void *window) = SDL_GL_SwapWindow;

static void flip(const char *state, int paused, int current, int surface_lost) {
    fake_activity_set_paused(paused);
    fake_sdl_set_current(current);
    fake_sdl_set_surface_lost(surface_lost);
    int answer = swap_window(fake_sdl_window());
    if (answer < 0) {
        printf("sdl-swap-test: %s: swap %d (%s)\n", state, answer, SDL_GetError());
    } else {
        printf("sdl-swap-test: %s: swap %d\n", state, answer);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "dlsym") == 0) {
        void *library = dlopen("libfake-sdl2.so", RTLD_NOW);
        if (library == NULL) {
            printf("sdl-swap-test: %s\n", dlerror());
            return 1;
        }
        swap_window = (int (*)(void *))dlsym(library, "SDL_GL_SwapWindow");
        if (swap_window == NULL) {
            printf("sdl-swap-test: SDL_GL_SwapWindow is missing\n");
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
