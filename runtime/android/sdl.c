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
// (SDL_androidevents.c, Android_PumpEvents_NonBlocking: SDL_GL_MakeCurrent(window, NULL)). An app that draws in
// between gets -1 from SDL_GL_SwapWindow, from SDL's own check before EGL ("The specified window has not been made
// current"), and may take it for a lost display: Ren'Py then makes a new context without a surface and loads its whole
// scene into it in the background, and Android's onStop waits for it. In that window alone, a swap that failed while
// no context is current and the activity is paused answers 0, a dropped frame; the frame is not shown either way.
// Every other answer is SDL's. Where both ways meet (an import of an SDL whose stub goes through the replaced entry),
// the outer wrapper gets the inner one's 0 and passes it on. SDL 3 reads SDL3_DYNAMIC_API, and its bool answers are
// not touched by name either: no context current means it failed there too, and 0 is its false.
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

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

static int dropped_frame(int answer, CurrentContext current_context) {
    return answer < 0 && current_context != NULL && current_context() == NULL && minato_activity_paused != NULL &&
           minato_activity_paused() != 0;
}

int minato_sdl_swap_window(void *window) {
    int answer = minato_sdl_swap_original(window);
    return dropped_frame(answer, minato_sdl_current_context) ? 0 : answer;
}

static int dynapi_swap_window(void *window) {
    int answer = dynapi_original(window);
    return dropped_frame(answer, dynapi_current_context) ? 0 : answer;
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
    if (stub == NULL || current_context == NULL) return;
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
