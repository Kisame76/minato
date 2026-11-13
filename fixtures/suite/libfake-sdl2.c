// libfake-sdl2.so: stands in for libSDL2 by its exported names only (the SDL swap override, runtime/android/sdl.c).
// SDL_GL_SwapWindow checks first, as SDL 2's SDL_video.c does, that the window is the current one ("The specified
// window has not been made current"), and only then swaps through EGL, which fails while the surface is lost.
// SDL_GL_GetCurrentContext and SDL_GetError answer as SDL's do. fake_sdl_set_current makes the window and its context
// current, or none, as SDL's context backup at a pause does (SDL_androidevents.c: SDL_GL_MakeCurrent(window, NULL));
// fake_sdl_set_surface_lost makes EGL's swap fail. No GL: a window and a context are two small objects.
#include <stddef.h>
#include <stdio.h>

static int window_object, context_object;
static void *current_window, *current_context;
static int surface_lost;
static char error[160];

static int set_error(const char *message) {
    snprintf(error, sizeof error, "%s", message);
    return -1;
}

void *fake_sdl_window(void) {
    return &window_object;
}

void fake_sdl_set_current(int current) {
    current_window = current ? &window_object : NULL;
    current_context = current ? &context_object : NULL;
}

void fake_sdl_set_surface_lost(int lost) {
    surface_lost = lost;
}

void *SDL_GL_GetCurrentContext(void) {
    return current_context;
}

const char *SDL_GetError(void) {
    return error;
}

int SDL_GL_SwapWindow(void *window) {
    if (window != &window_object) return set_error("Invalid window");
    if (current_window != window) return set_error("The specified window has not been made current");
    if (surface_lost) return set_error("unable to show color buffer in an OS-native window");
    return 0;
}
