// libminato-sdl.so, the SDL swap override, two ways to the same wrapper of SDL_GL_SwapWindow:
// - SDL 2's dynamic API (SDL_dynapi.c, on by default on Android): every public SDL function is a stub that loads its
//   target from SDL's jump table, which SDL fills at its first call from the library the environment variable
//   SDL_DYNAMIC_API names, through that library's SDL_DYNAPI_entry. The runtime names this library there, so
//   SDL_DYNAPI_entry below gets the asking SDL's table: it lets that SDL fill the table itself, through the SDL's own
//   SDL_DYNAPI_entry (which checks the version and the size and reads no variable), then replaces the one entry of
//   SDL_GL_SwapWindow. That reaches SDL linked into an app's own library, whose calls never go through an import
//   (Ren'Py's librenpython.so).
// - By name, for an SDL 2 without the dynamic API that comes as its own library: the loader preloads this library and
//   resolves SDL_GL_SwapWindow, by import and by dlsym alike, to minato_sdl_swap_window wherever the library that
//   defines it also defines SDL_GL_GetCurrentContext, writing the function it stands for into minato_sdl_swap_original
//   and SDL_GL_GetCurrentContext into minato_sdl_current_context.
// SDL 2 on Android backs its context up at a pause one event pump after it queues the background events
// (SDL_androidevents.c, Android_PumpEvents_NonBlocking: SDL_GL_MakeCurrent(window, NULL), which leaves no window
// current either). An app that draws in between fails SDL's own check before EGL: SDL_GL_SwapWindow, which returns
// nothing in SDL 2, sets the thread's error to "The specified window has not been made current" (SDL's tag
// release-2.0.20, the SDL that Ren'Py 8.5.3 builds, renpy-build's tasks/sdl2.py: src/video/SDL_video.c
// SDL_GL_SwapWindow; src/SDL_error.c SDL_SetError into SDL_GetErrBuf, a buffer per thread, src/thread/SDL_thread.c).
// pygame_sdl2 reads that text, not a return value (display.pyx Window.flip at its tag renpy-8.5.3.26051504:
// SDL_ClearError, SDL_GL_SwapWindow, raise if SDL_GetError is not empty), and Ren'Py takes the raise for a lost display
// ("Flip failed"): it makes a new context without a surface and loads its whole scene into it in the background, and
// Android's onStop waits for it. In that window alone, while no context is current and the activity is paused, the
// wrapper does not call SDL: the swap that would fail is a dropped frame, the thread's error stays as it was before the
// call, and the wrapper answers 0. The frame is not shown either way. Every other swap is SDL's, with SDL's answer.
// Where both ways meet (an import of an SDL whose stub goes through the replaced entry), the outer wrapper sees the
// same state and drops the frame itself. SDL 3 is left alone: it reads SDL3_DYNAMIC_API, and by name an SDL that
// defines SDL_GL_DestroyContext or SDL_CreateWindowWithProperties keeps every swap (SDL 3's names, both in
// SDL_dynapi_procs.h from SDL's tag preview-3.1.3 on; neither in SDL 2 up to release-2.32.0, which has
// SDL_GL_DeleteContext; an early SDL 3 preview may lack the first), as its bool answer and error text are its app's to
// read.
#include <dlfcn.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

// SDL 2's swap returns nothing and SDL 3's a bool: the wrapper passes on whatever the function left in the return
// register, which only SDL 3's callers read.
typedef int (*SwapWindow)(void *window);
typedef void *(*CurrentContext)(void);
typedef int32_t (*DynapiEntry)(uint32_t apiver, void *table, uint32_t tablesize);

// The runtime's: 1 from the activity's onPause until its onResume. Weak, so that a system without it links and the
// wrapper only passes SDL's answer on.
int minato_activity_paused(void) __attribute__((weak));

// The loader's by-name override.
SwapWindow minato_sdl_swap_original;
CurrentContext minato_sdl_current_context;

// The dynamic API's: the entry SDL had for SDL_GL_SwapWindow and its SDL_GL_GetCurrentContext. One SDL per process:
// a second SDL that asks keeps its table as it fills it.
static SwapWindow dynapi_original;
static CurrentContext dynapi_current_context;

static int dropped_frame(CurrentContext current_context) {
    return current_context != NULL && current_context() == NULL && minato_activity_paused != NULL &&
           minato_activity_paused() != 0;
}

// Whether the SDL behind `sdl` has one of SDL 3's own names.
static int has_sdl3_names(void *sdl) {
    return dlsym(sdl, "SDL_GL_DestroyContext") != NULL || dlsym(sdl, "SDL_CreateWindowWithProperties") != NULL;
}

// Whether the SDL behind `function` is SDL 3; an SDL it cannot tell counts as SDL 3, which is left alone.
static int is_sdl3(void *function) {
    Dl_info info;
    if (dladdr(function, &info) == 0 || info.dli_fname == NULL) return 1;
    void *sdl = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    return sdl == NULL || has_sdl3_names(sdl);  // the handle stays open, as SDL is loaded
}

// The by-name SDL's version, asked at its first frame that would be dropped: -1 not yet, 0 SDL 2, 1 SDL 3.
static atomic_int by_name_sdl3 = -1;

int minato_sdl_swap_window(void *window) {
    if (dropped_frame(minato_sdl_current_context)) {
        int sdl3 = atomic_load_explicit(&by_name_sdl3, memory_order_relaxed);
        if (sdl3 < 0) {
            sdl3 = is_sdl3((void *)minato_sdl_swap_original);
            atomic_store_explicit(&by_name_sdl3, sdl3, memory_order_relaxed);
        }
        if (sdl3 == 0) return 0;
    }
    return minato_sdl_swap_original(window);
}

static int dynapi_swap_window(void *window) {
    return dropped_frame(dynapi_current_context) ? 0 : dynapi_original(window);
}

// The table entry that the dynamic API stub at `code` jumps through: ADRP xA, page; LDR xB, [xA, #offset]; BR xB, with
// an optional BTI C before them, as the compilers build SDL's stubs. 0 for any other shape.
static uintptr_t stub_slot(const uint32_t *code) {
    if (code[0] == 0xd503245fu) ++code;  // BTI C
    const uint32_t adrp = code[0], ldr = code[1], br = code[2];
    if ((adrp & 0x9f000000u) != 0x90000000u) return 0;
    const uint32_t base = adrp & 0x1fu;
    if ((ldr & 0xffc00000u) != 0xf9400000u || ((ldr >> 5) & 0x1fu) != base) return 0;  // LDR (64-bit, unsigned offset)
    if ((br & 0xfffffc1fu) != 0xd61f0000u || ((br >> 5) & 0x1fu) != (ldr & 0x1fu)) return 0;
    int64_t pages = (int64_t)((((adrp >> 5) & 0x7ffffu) << 2) | ((adrp >> 29) & 0x3u));
    if (pages & (INT64_C(1) << 20)) pages -= INT64_C(1) << 21;  // 21 bits, signed
    const uintptr_t page = ((uintptr_t)code & ~(uintptr_t)0xfff) + (uintptr_t)(pages * 4096);
    return page + (uintptr_t)((ldr >> 10) & 0xfffu) * 8u;
}

// Replaces SDL_GL_SwapWindow's entry in `table` of the SDL behind `sdl`, when its stub's entry lies in that table.
static void replace_swap(void *sdl, void *table, uint32_t tablesize) {
    if (dynapi_original != NULL) return;
    void *stub = dlsym(sdl, "SDL_GL_SwapWindow");
    // The loader's by-name override answers a dlsym of the name with the wrapper, and has just written the SDL's own
    // function, the stub, into minato_sdl_swap_original.
    if (stub == (void *)minato_sdl_swap_window) stub = (void *)minato_sdl_swap_original;
    CurrentContext current_context = (CurrentContext)dlsym(sdl, "SDL_GL_GetCurrentContext");
    // SDL 3 is left alone (it reads SDL3_DYNAMIC_API, so it should never ask).
    if (stub == NULL || current_context == NULL || has_sdl3_names(sdl)) return;
    const uintptr_t slot = stub_slot((const uint32_t *)stub);
    const uintptr_t start = (uintptr_t)table;
    if (tablesize < sizeof(SwapWindow) || slot < start || slot - start > tablesize - sizeof(SwapWindow) ||
        (slot - start) % sizeof(SwapWindow) != 0) {
        return;
    }
    SwapWindow *entry = (SwapWindow *)slot;
    dynapi_current_context = current_context;
    dynapi_original = *entry;
    *entry = dynapi_swap_window;
}

// SDL's call, from SDL_InitDynamicAPI of the SDL that owns `table`. The asking SDL is found by the table's address and
// opened by its own name, already loaded (RTLD_NOLOAD); its SDL_DYNAPI_entry is looked up in it, not globally, where
// this one would answer. Its answer is SDL's: below 0, SDL fills the table from its own functions.
int32_t SDL_DYNAPI_entry(uint32_t apiver, void *table, uint32_t tablesize) {
    Dl_info info;
    if (dladdr(table, &info) == 0 || info.dli_fname == NULL) return -1;
    void *sdl = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (sdl == NULL) return -1;
    DynapiEntry own = (DynapiEntry)dlsym(sdl, "SDL_DYNAPI_entry");
    if (own == NULL || own == SDL_DYNAPI_entry) return -1;
    const int32_t answer = own(apiver, table, tablesize);
    if (answer >= 0) replace_swap(sdl, table, tablesize);
    return answer;  // the handle stays open, as SDL keeps the library it loads
}
