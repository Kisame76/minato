/* ADR-0043 b1: libnativewindow's ANativeWindow_lock and ANativeWindow_unlockAndPost against graphics-run's fake
   buffers (window_host.m, MINATO_WINDOW_BUFFERS=fake) on the window host's window, ID 1, of 64 x 48 pixels.
   - Android's answers: INVALID_OPERATION (-ENOSYS) for a post without a lock and for a second lock.
   - The buffer: setBuffersGeometry's size and format, else the window's size and NativeActivity's RGBX_8888; the
     whole buffer as the dirty rectangle without one; the buffers' answers in place of the app's (the fake's: the
     whole buffer for a new geometry, else the app's rectangle within the buffer).
   - A format the buffers refuse leaves the window unlocked; something else than a window, or no buffer to fill,
     answers -EINVAL.
   - The present's gate: the window host keeps it closed until 200 ms after the first lock, so the first post waits.
   The fake prints what reaches it to stderr, with the first pixel the app wrote. */
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

static void show(const char *what, int32_t result, const ANativeWindow_Buffer *buffer, const ARect *dirty) {
    printf("%s: %d", what, result);
    if (result == 0) {
        printf(", %dx%d stride %d format %d", buffer->width, buffer->height, buffer->stride, buffer->format);
        if (dirty != NULL) {
            printf(", dirty %d %d %d %d", dirty->left, dirty->top, dirty->right, dirty->bottom);
        }
    }
    printf("\n");
}

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(void) {
    create_window create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    ANativeWindow *window = create(1, 64, 48);
    ANativeWindow_Buffer buffer;

    printf("unlockAndPost without a lock: %d\n", ANativeWindow_unlockAndPost(window));
    int32_t result = ANativeWindow_lock(window, &buffer, NULL);
    show("lock", result, &buffer, NULL);
    printf("lock again: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    if (result == 0) {
        ((uint32_t *)buffer.bits)[0] = 0x11223344;
    }
    const int64_t start = now_ms();
    printf("unlockAndPost: %d\n", ANativeWindow_unlockAndPost(window));
    printf("the first post waited for the gate: %s\n", now_ms() - start >= 100 ? "yes" : "no");
    printf("unlockAndPost again: %d\n", ANativeWindow_unlockAndPost(window));

    // setBuffersGeometry's size and format, with a dirty rectangle: the buffers answer the whole buffer for a new
    // geometry, and then the rectangle within the buffer.
    ANativeWindow_setBuffersGeometry(window, 50, 20, WINDOW_FORMAT_RGB_565);
    ARect dirty = {.left = 10, .top = 5, .right = 30, .bottom = 15};
    result = ANativeWindow_lock(window, &buffer, &dirty);
    show("lock 50x20 RGB_565 with a dirty rectangle", result, &buffer, &dirty);
    if (result == 0) {
        ((uint16_t *)buffer.bits)[0] = 0xf800;
    }
    printf("unlockAndPost: %d\n", ANativeWindow_unlockAndPost(window));
    dirty = (ARect){.left = 10, .top = 5, .right = 60, .bottom = 15};
    result = ANativeWindow_lock(window, &buffer, &dirty);
    show("lock again with a rectangle beyond its right edge", result, &buffer, &dirty);
    printf("unlockAndPost: %d\n", ANativeWindow_unlockAndPost(window));

    // The format alone, at the window's size.
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    result = ANativeWindow_lock(window, &buffer, NULL);
    show("lock RGBA_8888", result, &buffer, NULL);
    printf("unlockAndPost: %d\n", ANativeWindow_unlockAndPost(window));

    // A format the buffers refuse.
    ANativeWindow_setBuffersGeometry(window, 0, 0, AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT);
    printf("lock RGBA_FP16: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    printf("unlockAndPost: %d\n", ANativeWindow_unlockAndPost(window));

    // Not a window, and no buffer to fill.
    int not_a_window[16] = {0};
    printf("lock of something else: %d\n", ANativeWindow_lock((ANativeWindow *)not_a_window, &buffer, NULL));
    printf("unlockAndPost of something else: %d\n", ANativeWindow_unlockAndPost((ANativeWindow *)not_a_window));
    ANativeWindow_setBuffersGeometry(window, 0, 0, 0);
    printf("lock without a buffer: %d\n", ANativeWindow_lock(window, NULL, NULL));
    printf("unlockAndPost after it: %d\n", ANativeWindow_unlockAndPost(window));
    ANativeWindow_release(window);
    return 0;
}
