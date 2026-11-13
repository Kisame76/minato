// The app's half of libfake-sdl2-static.so: its flip is pygame_sdl2's display.flip in Ren'Py's librenpython.so
// (display.pyx Window.flip): SDL_ClearError, then SDL_GL_SwapWindow's stub called directly, in the same library, where
// no import override reaches the call, then SDL_GetError: the error text, "" when the flip did not fail.
void *fake_sdl_static_window(void);
void SDL_GL_SwapWindow(void *window);
void SDL_ClearError(void);
const char *SDL_GetError(void);

const char *fake_app_flip(void) {
    SDL_ClearError();
    SDL_GL_SwapWindow(fake_sdl_static_window());
    return SDL_GetError();
}
