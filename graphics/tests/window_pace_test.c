/* ADR-0043 b5, detail 3: back-pressure on graphics/host's buffers (window.m), which show by contents and so have no
   drawable to wait for. As Android's dequeueBuffer blocks while no buffer is free, a lock waits until the display link
   has ticked since the window's last post. graphics-run's window, ID 1, of 64 x 48 pixels, whose host ticks at 100 Hz
   until the window has 30 presents and then stops (MINATO_WINDOW_TICKS=100:30).
   - 30 posts as fast as the locks let them: at most one a tick, so they take at least 25 periods of 10 ms.
   - 5 more with the display link stopped: a lock waits at most two periods after the last post, so they all post
     within 1 s, where a lock that waited for a tick would time out.
   With MINATO_WINDOW_RECORD=64:35 the window host checks the layer's records of the 35 posts 100 ms after the last
   (detail 5), for which the program waits 300 ms before its end. */
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { width = 64, height = 48 };

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

static long long now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* `count` posts of the whole buffer; how many of their locks and posts failed. */
static int posts(ANativeWindow *window, int count, int32_t results[]) {
    int failed = 0;
    for (int index = 0; index < count; index++) {
        ANativeWindow_Buffer buffer;
        results[index] = ANativeWindow_lock(window, &buffer, NULL);
        if (results[index] == 0) {
            memset(buffer.bits, index, (size_t)buffer.stride * (size_t)buffer.height * 4);
            results[index] = ANativeWindow_unlockAndPost(window);
        }
        failed += results[index] != 0;
    }
    return failed;
}

int main(void) {
    create_window create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    ANativeWindow *window = create(1, width, height);
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);

    int32_t results[30];
    long long started = now_ms();
    int failed = posts(window, 30, results);
    printf("30 posts: %d failed; they took at least 25 periods: %s\n", failed,
           now_ms() - started >= 250 ? "yes" : "no");
    started = now_ms();
    failed = posts(window, 5, results);
    printf("5 more posts with the display link stopped: %d %d %d %d %d; within 1 s: %s\n", results[0], results[1],
           results[2], results[3], results[4], failed == 0 && now_ms() - started < 1000 ? "yes" : "no");
    usleep(300000);
    ANativeWindow_release(window);
    return 0;
}
