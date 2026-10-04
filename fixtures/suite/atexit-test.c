// atexit-test: 2,000 handlers registered with __cxa_atexit, as a large library's static destructors register, and
// their run at exit in the reverse order. bionic keeps the handlers in a read-only array of whole pages and grows it
// with mremap(MREMAP_MAYMOVE): twice on 16 KiB pages, four times on Android's 4 KiB (ADR-0021, Godot's stop 8).
// Expected output: fixtures/expected/atexit-test.out
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int __cxa_atexit(void (*function)(void *), void *argument, void *dso);
extern void *__dso_handle;

enum { handler_count = 2000 };

static int registered;
static int next_index;  // the index the next handler must have: the last one registered runs first
static int ran;
static int in_order;

static void handler(void *argument) {
    const int index = (int)(intptr_t)argument;
    if (index == next_index) ++in_order;
    --next_index;
    if (++ran == registered) {
        // The last handler to run reports. stdio may be flushed and closed by now, so it writes directly.
        char line[96];
        const int length = snprintf(line, sizeof line, "%d handlers ran, %d in reverse order\n", ran, in_order);
        if (length > 0) (void)write(STDOUT_FILENO, line, (size_t)length);
    }
}

int main(void) {
    int refused = 0;
    for (int index = 0; index < handler_count; ++index) {
        if (__cxa_atexit(handler, (void *)(intptr_t)index, &__dso_handle) == 0) {
            ++registered;
        } else {
            ++refused;
        }
    }
    // Once the array cannot grow, every later registration fails, so those registered are 0 to registered - 1.
    next_index = registered - 1;
    printf("%d handlers registered, %d refused\n", registered, refused);
    fflush(stdout);
    return 0;
}
