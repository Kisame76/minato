// libfake-sdl2-static.so: an app library with SDL 2 linked in, as Ren'Py's librenpython.so has it (the SDL swap
// override, runtime/android/sdl.c). SDL's public functions are its dynamic API's stubs, each a jump through one table
// (SDL_dynapi.c): the table starts with the _DEFAULT functions, the first call fills it, from the library that
// SDL_DYNAMIC_API names (dlopen, then its SDL_DYNAPI_entry) or else from this library's own _REAL functions; and the
// app's code in the same library calls the stubs directly, never through an import (fake_app_flip in
// libfake-sdl2-static-app.c, another file as pygame_sdl2's display.flip is; the library links with -Bsymbolic, so the
// call is a plain branch to the stub, as in Ren'Py's). The _REAL functions answer as libfake-sdl2.so's do (a
// shorter message for EGL's failure): the swap returns nothing and sets the error text when it fails, as SDL 2's.
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_DYNAPI_VERSION 1

typedef void *(*GetCurrentContextFn)(void);
typedef void (*SwapWindowFn)(void *window);
typedef const char *(*GetErrorFn)(void);
typedef int32_t (*EntryFn)(uint32_t apiver, void *table, uint32_t tablesize);

typedef struct {
    GetErrorFn SDL_GetError;
    GetCurrentContextFn SDL_GL_GetCurrentContext;
    SwapWindowFn SDL_GL_SwapWindow;
} JumpTable;

static int window_object, context_object;
static void *current_window, *current_context;
static int surface_lost;
static char error[160];
static int has_error;

static void set_error(const char *message) {
    snprintf(error, sizeof error, "%s", message);
    has_error = 1;
}

static const char *SDL_GetError_REAL(void) {
    return has_error ? error : "";
}

static void *SDL_GL_GetCurrentContext_REAL(void) {
    return current_context;
}

static void SDL_GL_SwapWindow_REAL(void *window) {
    if (window != &window_object) {
        set_error("Invalid window");
    } else if (current_window != window) {
        set_error("The specified window has not been made current");
    } else if (surface_lost) {
        set_error("unable to show color buffer");
    }
}

static const char *SDL_GetError_DEFAULT(void);
static void *SDL_GL_GetCurrentContext_DEFAULT(void);
static void SDL_GL_SwapWindow_DEFAULT(void *window);

// Named for the odd stub's assembly below; hidden like SDL's own static table.
__attribute__((visibility("hidden"))) JumpTable fake_sdl_jump_table = {
    SDL_GetError_DEFAULT, SDL_GL_GetCurrentContext_DEFAULT, SDL_GL_SwapWindow_DEFAULT};
_Static_assert(offsetof(JumpTable, SDL_GL_SwapWindow) == 16, "the odd stub loads the third entry");

static int32_t initialize_jumptable(uint32_t apiver, void *table, uint32_t tablesize) {
    if (apiver != SDL_DYNAPI_VERSION || tablesize > sizeof fake_sdl_jump_table) return -1;
    fake_sdl_jump_table.SDL_GetError = SDL_GetError_REAL;
    fake_sdl_jump_table.SDL_GL_GetCurrentContext = SDL_GL_GetCurrentContext_REAL;
    fake_sdl_jump_table.SDL_GL_SwapWindow = SDL_GL_SwapWindow_REAL;
    if (table != &fake_sdl_jump_table) memcpy(table, &fake_sdl_jump_table, tablesize);
    return 0;
}

int32_t SDL_DYNAPI_entry(uint32_t apiver, void *table, uint32_t tablesize) {
    return initialize_jumptable(apiver, table, tablesize);
}

static void init_dynamic_api(void) {
    static int initialized;
    if (initialized) return;
    initialized = 1;
    const char *name = getenv("SDL_DYNAMIC_API");
    EntryFn entry = NULL;
    if (name != NULL) {
        void *library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (library != NULL) entry = (EntryFn)dlsym(library, "SDL_DYNAPI_entry");
        if (entry == NULL) fprintf(stderr, "SDL Dynamic API Failure: couldn't load overriding SDL library\n");
    }
    if (entry != NULL && entry(SDL_DYNAPI_VERSION, &fake_sdl_jump_table, sizeof fake_sdl_jump_table) >= 0) return;
    if (initialize_jumptable(SDL_DYNAPI_VERSION, &fake_sdl_jump_table, sizeof fake_sdl_jump_table) < 0) abort();
}

static const char *SDL_GetError_DEFAULT(void) {
    init_dynamic_api();
    return fake_sdl_jump_table.SDL_GetError();
}

static void *SDL_GL_GetCurrentContext_DEFAULT(void) {
    init_dynamic_api();
    return fake_sdl_jump_table.SDL_GL_GetCurrentContext();
}

static void SDL_GL_SwapWindow_DEFAULT(void *window) {
    init_dynamic_api();
    fake_sdl_jump_table.SDL_GL_SwapWindow(window);
}

// The public stubs: one load and one jump each, as SDL's own.
const char *SDL_GetError(void) {
    return fake_sdl_jump_table.SDL_GetError();
}

void *SDL_GL_GetCurrentContext(void) {
    return fake_sdl_jump_table.SDL_GL_GetCurrentContext();
}

// Not through the table: the override never touches it.
void SDL_ClearError(void) {
    has_error = 0;
}

#ifdef FAKE_ODD_STUB
// libfake-sdl2-odd.so: a swap stub of another shape, its target loaded from the table's entry as SDL's stub loads it,
// but called with a frame (BLR) instead of jumped to (BR). The override leaves such a table as SDL fills it.
__attribute__((naked)) void SDL_GL_SwapWindow(void *window __attribute__((unused))) {
    __asm__("adrp x8, fake_sdl_jump_table\n"
            "ldr x9, [x8, #:lo12:fake_sdl_jump_table + 16]\n"
            "stp x29, x30, [sp, #-16]!\n"
            "mov x29, sp\n"
            "blr x9\n"
            "ldp x29, x30, [sp], #16\n"
            "ret\n");
}
#else
void SDL_GL_SwapWindow(void *window) {
    fake_sdl_jump_table.SDL_GL_SwapWindow(window);
}
#endif

// The test's levers (not SDL's): the window, its context current or none, EGL's surface lost.
void *fake_sdl_static_window(void) {
    return &window_object;
}

void fake_sdl_static_set_current(int current) {
    current_window = current ? &window_object : NULL;
    current_context = current ? &context_object : NULL;
}

void fake_sdl_static_set_surface_lost(int lost) {
    surface_lost = lost;
}
