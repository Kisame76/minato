// libminato-sdl.so, the SDL swap override: the loader preloads it when it is among the system's libraries and
// resolves SDL_GL_SwapWindow, by import and by dlsym alike, to the wrapper below wherever the library that defines it
// also defines SDL_GL_GetCurrentContext (SDL 2), writing the function it stands for into minato_sdl_swap_original and
// SDL_GL_GetCurrentContext into minato_sdl_current_context.
// SDL 2 on Android backs its context up at a pause one event pump after it queues the background events
// (SDL_androidevents.c, Android_PumpEvents_NonBlocking: SDL_GL_MakeCurrent(window, NULL)). An app that draws in
// between gets -1 from SDL_GL_SwapWindow, from SDL's own check before EGL ("The specified window has not been made
// current"), and may take it for a lost display: Ren'Py then makes a new context without a surface and loads its whole
// scene into it in the background, and Android's onStop waits for it. In that window alone, a swap that failed while
// no context is current and the activity is paused answers 0, a dropped frame; the frame is not shown either way.
// Every other answer is SDL's. SDL 3 returns a bool from the same name: no context current means it failed there too,
// and 0 is its false.
#include <stddef.h>

typedef int (*SwapWindow)(void *window);
typedef void *(*CurrentContext)(void);

// The runtime's: 1 from the activity's onPause until its onResume. Weak, so that a system without it links and the
// wrapper only passes SDL's answer on.
int minato_activity_paused(void) __attribute__((weak));

SwapWindow minato_sdl_swap_original;
CurrentContext minato_sdl_current_context;

int minato_sdl_swap_window(void *window) {
    int answer = minato_sdl_swap_original(window);
    if (answer < 0 && minato_sdl_current_context != NULL && minato_sdl_current_context() == NULL &&
        minato_activity_paused != NULL && minato_activity_paused() != 0) {
        return 0;
    }
    return answer;
}
