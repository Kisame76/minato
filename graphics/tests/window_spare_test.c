/* The hidden window's spare buffers (ADR-0043 b5, after step f): on graphics-run's window, ID 1, of 64 x 48 pixels, a
   post with a visible pixel shows, a transparent one hides the layer (the hide rule), and a second later the window
   keeps only that front buffer, which the window host's dump 1.5 s after the second post counts
   (MINATO_WINDOW_BUFFER_DUMP). Two seconds after the hidden post, a lock with a dirty rectangle gets a buffer again,
   with the rectangle as asked and the hidden post's pixels outside it (the copy-back from the front buffer kept), and
   its post with a visible pixel shows. */
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

static void clear(const ANativeWindow_Buffer *buffer, uint8_t value) {
    for (int32_t row = 0; row < buffer->height; row++) {
        memset((uint8_t *)buffer->bits + (size_t)row * (size_t)buffer->stride * 4, value, (size_t)buffer->width * 4);
    }
}

int main(void) {
    create_window create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    ANativeWindow *window = create(1, 64, 48);
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer buffer;
    printf("lock 1: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    clear(&buffer, 0x10);
    printf("post 1, shown: %d\n", ANativeWindow_unlockAndPost(window));
    printf("lock 2: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    clear(&buffer, 0);
    printf("post 2, transparent: %d\n", ANativeWindow_unlockAndPost(window));
    sleep(2);

    ARect dirty = {.left = 8, .top = 8, .right = 16, .bottom = 16};
    printf("lock 3: %d\n", ANativeWindow_lock(window, &buffer, &dirty));
    printf("its dirty rectangle: %d %d %d %d\n", dirty.left, dirty.top, dirty.right, dirty.bottom);
    int kept = 1;
    for (int32_t row = 0; row < buffer.height; row++) {
        for (int32_t column = 0; column < buffer.width; column++) {
            const uint8_t *pixel =
                (const uint8_t *)buffer.bits + ((size_t)row * (size_t)buffer.stride + (size_t)column) * 4;
            const int inside = column >= dirty.left && column < dirty.right && row >= dirty.top && row < dirty.bottom;
            kept = kept && (inside || (pixel[0] | pixel[1] | pixel[2] | pixel[3]) == 0);
        }
    }
    printf("outside it the hidden post's pixels: %s\n", kept ? "yes" : "no");
    for (int32_t row = dirty.top; row < dirty.bottom; row++) {
        memset((uint8_t *)buffer.bits + ((size_t)row * (size_t)buffer.stride + (size_t)dirty.left) * 4, 0x20,
               (size_t)(dirty.right - dirty.left) * 4);
    }
    printf("post 3, shown: %d\n", ANativeWindow_unlockAndPost(window));
    ANativeWindow_release(window);
    usleep(300000);
    return 0;
}
