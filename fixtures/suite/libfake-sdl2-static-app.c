// The app's half of libfake-sdl2-static.so: its flip calls SDL_GL_SwapWindow's stub directly, in the same library,
// as pygame_sdl2's display.flip in Ren'Py's librenpython.so; no import override reaches the call.
void *fake_sdl_static_window(void);
int SDL_GL_SwapWindow(void *window);

int fake_app_flip(void) {
    return SDL_GL_SwapWindow(fake_sdl_static_window());
}
