// libfake-sdl2.so: stands in for libSDL2 by its exported names only (the SDL swap override, runtime/android/sdl.c).
// SDL_GL_SwapWindow returns nothing, as SDL 2's: it checks first, as SDL_video.c does, that the window is the current
// one, and only then swaps through EGL, which fails while the surface is lost; a failure sets the error text ("The
// specified window has not been made current"). SDL_GL_GetCurrentContext, SDL_GetError ("" without an error) and
// SDL_ClearError answer as SDL's do. fake_sdl_set_current makes the window and its context
// current, or none, as SDL's context backup at a pause does (SDL_androidevents.c: SDL_GL_MakeCurrent(window, NULL));
// fake_sdl_set_surface_lost makes EGL's swap fail. No GL: a window and a context are two small objects. Built a
// second time as libfake-sdl3.so (FAKE_SDL3): SDL 3's swap returns a bool, false with the same error text, and SDL 3
// defines SDL_GL_DestroyContext, which SDL 2 does not (its SDL_GL_DeleteContext): a name the override tells SDL 3 by.
// And as libfake-sdl3-preview.so (FAKE_SDL3_PREVIEW too): an early SDL 3 preview with SDL 2's SDL_GL_DeleteContext,
// told by SDL 3's SDL_CreateWindowWithProperties instead.
#include <stddef.h>
#include <stdio.h>

static int window_object, context_object;
static void *current_window, *current_context;
static int surface_lost;
static char error[160];
static int has_error;

static void set_error(const char *message) {
    snprintf(error, sizeof error, "%s", message);
    has_error = 1;
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
    return has_error ? error : "";
}

void SDL_ClearError(void) {
    has_error = 0;
}

#if defined(FAKE_SDL3_PREVIEW)
_Bool SDL_GL_DeleteContext(void *context) {
    return context == &context_object;
}

void *SDL_CreateWindowWithProperties(unsigned properties) {
    return properties != 0 ? &window_object : NULL;
}
#elif defined(FAKE_SDL3)
_Bool SDL_GL_DestroyContext(void *context) {
    return context == &context_object;
}
#endif

#ifdef FAKE_SDL3

_Bool SDL_GL_SwapWindow(void *window) {
#else
void SDL_GL_SwapWindow(void *window) {
#endif
    if (window != &window_object) {
        set_error("Invalid window");
    } else if (current_window != window) {
        set_error("The specified window has not been made current");
    } else if (surface_lost) {
        set_error("unable to show color buffer in an OS-native window");
    }
#ifdef FAKE_SDL3
    return !has_error;
#endif
}
