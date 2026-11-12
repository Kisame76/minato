/* ADR-0043 b2: graphics/host's buffers for drawing by the CPU when the app goes to the background, on graphics-run's
   window, ID 1, of 64 x 48 pixels, with the window host's background script (MINATO_WINDOW_SCRIPT=background:3:100):
   after the window's third present the host sets the background flag, asks for "scheduled", stays 100 ms in the
   background, removes the window and comes back 100 ms later (window_host.m).
   - The program posts three frames, then one every 5 ms for 50 ms, in the background: those present nothing, and no
     guest thread may commit a command buffer after "scheduled" (the host's lines, from the Metal hook's count).
   - Then it holds a lock for 400 ms, over the window's removal: the buffer stays the program's, and its post
     answers -ENODEV, as does a lock after it. */
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

static int others;  // posts that answered neither 0 nor -ENODEV

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void frame(ANativeWindow *window, int number) {
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, NULL) != 0) {
        others++;
        return;
    }
    for (int32_t row = 0; row < buffer.height; row++) {
        memset((uint8_t *)buffer.bits + (size_t)row * (size_t)buffer.stride * 4, number & 0xff,
               (size_t)buffer.width * 4);
    }
    const int32_t posted = ANativeWindow_unlockAndPost(window);
    others += posted != 0 && posted != -ENODEV;
}

int main(void) {
    create_window create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    ANativeWindow *window = create(1, 64, 48);
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    int number = 0;
    while (number < 3) {
        frame(window, number++);
    }
    for (const int64_t start = now_ms(); now_ms() - start < 50; usleep(5000)) {
        frame(window, number++);
    }
    ANativeWindow_Buffer buffer;
    printf("a lock held over the window's removal: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    usleep(400000);
    printf("its post once the window is gone: %d\n", ANativeWindow_unlockAndPost(window));
    printf("a lock once the window is gone: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    /* Once the host's window is gone the held window keeps its last size and its requested format (the glue's
       documented behaviour, runtime/android/native_window.cpp): the size whatever the geometry, the format once set. */
    printf("getWidth once the window is gone: %d\n", ANativeWindow_getWidth(window));
    printf("getHeight once the window is gone: %d\n", ANativeWindow_getHeight(window));
    printf("getFormat once the window is gone: %d\n", ANativeWindow_getFormat(window));
    printf("posts before that answered neither 0 nor -ENODEV: %d\n", others);
    ANativeWindow_release(window);
    return 0;
}
