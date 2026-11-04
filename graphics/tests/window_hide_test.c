/* ADR-0043 b3: the hide rule on graphics-run's window, ID 1, of 64 x 48 pixels, whose layer the window host watches
   (MINATO_WINDOW_WATCH=hidden,contents, its lines on stderr: the layer's hidden and each buffer shown as a sublayer's
   contents) and leaves not opaque, or makes opaque (MINATO_WINDOW_OPAQUE=1), with the Metal hook printing any
   drawable presented (MINATO_METAL_HOOK=present:6), of which there are none. The posts:
   1. RGBA_8888, every pixel transparent: the layer hides, and nothing is shown;
   2. one pixel with alpha 1 (premultiplied): the layer shows, with this buffer;
   3. and 4. transparent again: it hides, once;
   5. RGBX_8888 with alpha 0 in memory, which has no alpha: it shows;
   6. RGBA_8888 transparent: it hides.
   On an opaque layer, nothing hides and every post shows its buffer, on black. */
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

static void post(ANativeWindow *window, int number, int32_t format, int visible_pixel) {
    ANativeWindow_setBuffersGeometry(window, 0, 0, format);
    ANativeWindow_Buffer buffer;
    const int32_t locked = ANativeWindow_lock(window, &buffer, NULL);
    if (locked != 0) {
        printf("post %d: lock %d\n", number, locked);
        return;
    }
    for (int32_t row = 0; row < buffer.height; row++) {
        memset((uint8_t *)buffer.bits + (size_t)row * (size_t)buffer.stride * 4, 0, (size_t)buffer.width * 4);
    }
    if (visible_pixel) {
        uint8_t *const last = (uint8_t *)buffer.bits + ((size_t)(buffer.height - 1) * (size_t)buffer.stride +
                                                        (size_t)(buffer.width - 1)) * 4;
        last[3] = 1;
    }
    printf("post %d: %d\n", number, ANativeWindow_unlockAndPost(window));
}

int main(void) {
    create_window create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    ANativeWindow *window = create(1, 64, 48);
    post(window, 1, WINDOW_FORMAT_RGBA_8888, 0);
    post(window, 2, WINDOW_FORMAT_RGBA_8888, 1);
    post(window, 3, WINDOW_FORMAT_RGBA_8888, 0);
    post(window, 4, WINDOW_FORMAT_RGBA_8888, 0);
    post(window, 5, WINDOW_FORMAT_RGBX_8888, 0);
    post(window, 6, WINDOW_FORMAT_RGBA_8888, 0);
    ANativeWindow_release(window);
    // The hook reads a drawable back when Metal presents it: a moment for the last one before the process ends.
    usleep(300000);
    return 0;
}
