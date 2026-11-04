/* ADR-0043 b2 at TRIM_MEMORY_UI_HIDDEN: graphics/host's buffers (window.m) let a window's buffers go when the system
   trims the app's memory at UI_HIDDEN, as Android's window gives its surface's buffers back once the app's UI is
   hidden; graphics-run's window, ID 1, of 64 x 48 pixels, whose host posts APKRT_EVENT_TRIM_MEMORY 20 once the window
   has two presents (MINATO_WINDOW_SCRIPT=trim:2).
   - The first lock answers the whole buffer as dirty, the second the rectangle asked for, as the window's buffers
     hold the first post.
   - After the trim a lock answers the whole buffer again: a new one, which holds nothing yet. Until the trim has come
     the test posts the rectangle again, for at most 10 s. */
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { width = 64, height = 48 };

typedef ANativeWindow *(*create_window)(uint64_t id, int32_t width, int32_t height);

/* Locks with `asked` (the whole buffer for NULL), fills what it answers and posts; the answer's rectangle. */
static ARect draw(ANativeWindow *window, const ARect *asked, int32_t *result) {
    ANativeWindow_Buffer buffer;
    ARect dirty = asked != NULL ? *asked : (ARect){0};
    *result = ANativeWindow_lock(window, &buffer, asked != NULL ? &dirty : NULL);
    if (*result != 0) {
        return dirty;
    }
    if (asked == NULL) {
        dirty = (ARect){.left = 0, .top = 0, .right = buffer.width, .bottom = buffer.height};
    }
    for (int y = dirty.top; y < dirty.bottom; y++) {
        memset((uint8_t *)buffer.bits + ((size_t)y * (size_t)buffer.stride + (size_t)dirty.left) * 4, 0x80,
               (size_t)(dirty.right - dirty.left) * 4);
    }
    *result = ANativeWindow_unlockAndPost(window);
    return dirty;
}

int main(void) {
    create_window create = (create_window)dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    ANativeWindow *window = create(1, width, height);
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);

    int32_t result;
    const ARect rect = {.left = 16, .top = 8, .right = 48, .bottom = 40};
    ARect dirty = draw(window, NULL, &result);
    printf("lock 1: dirty %d %d %d %d, post %d\n", dirty.left, dirty.top, dirty.right, dirty.bottom, result);
    dirty = draw(window, &rect, &result);
    printf("lock 2: dirty %d %d %d %d, post %d\n", dirty.left, dirty.top, dirty.right, dirty.bottom, result);
    int whole = 0;
    for (int attempt = 0; attempt < 1000 && !whole; attempt++) {
        dirty = draw(window, &rect, &result);
        whole = result == 0 && dirty.left == 0 && dirty.top == 0 && dirty.right == width && dirty.bottom == height;
        if (!whole) {
            usleep(10000);
        }
    }
    if (whole) {
        printf("after the trim: dirty %d %d %d %d, a new buffer\n", dirty.left, dirty.top, dirty.right, dirty.bottom);
    } else {
        printf("after the trim: dirty %d %d %d %d, still the old buffer after 10 s (%d)\n", dirty.left, dirty.top,
               dirty.right, dirty.bottom, result);
    }
    ANativeWindow_release(window);
    return 0;
}
