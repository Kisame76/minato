/* For test 6 of ADR-0030 in graphics-run: the host's window as the app gives it (ADR-0028, decision 6).
   - With MINATO_WINDOW=<width>x<height>, before the program starts, it adds a window of that many pixels on
     a CAMetalLayer of as many points at scale 1 and opens its gate: the program's first window, ID 1.
   - With MINATO_WINDOW_SCRIPT=background:<n> too, a thread of its own waits until the window has <n>
     presents and then keeps the app's order at background (ADR-0028, decision 6): the background flag,
     "scheduled" with 1 s to go, the window's removal; 100 ms later the foreground. It prints each step to
     stderr, how many command buffers other threads than the guest's committed during "scheduled" (one per
     queue: graphics/host's), and how many guest threads committed between "scheduled" and the foreground,
     which must be none (the Metal hook counts both, test_host.h). */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "test_host.h"

atomic_int minato_test_counting, minato_test_guest_commits, minato_test_asking, minato_test_asker_commits;

static uint64_t window;
static unsigned long presents;

static void *background(void *unused) {
    (void)unused;
    while (apkrt_window_presents(window) < presents) {
        usleep(1000);
    }
    fprintf(stderr, "host: in the background\n");
    apkrt_set_background(1);
    atomic_store(&minato_test_asking, 1);
    const int scheduled = apkrt_wait_scheduled(1.0);
    atomic_store(&minato_test_asking, 0);
    fprintf(stderr, "host: \"scheduled\": %d, command buffers committed meanwhile by other threads than the guest's: %d\n",
            scheduled, atomic_load(&minato_test_asker_commits));
    atomic_store(&minato_test_counting, 1);
    fprintf(stderr, "host: the window removed: %d\n", apkrt_window_remove(window));
    usleep(100000);
    atomic_store(&minato_test_counting, 0);
    fprintf(stderr, "host: guest commits after \"scheduled\": %d\n", atomic_load(&minato_test_guest_commits));
    fprintf(stderr, "host: in the foreground\n");
    apkrt_set_background(0);
    return NULL;
}

__attribute__((constructor)) static void add_window(void) {
    const char *size = getenv("MINATO_WINDOW");
    int width = 0, height = 0;
    if (size == NULL) {
        return;
    }
    if (sscanf(size, "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
        fprintf(stderr, "window host: MINATO_WINDOW=%s is not <width>x<height>\n", size);
        exit(2);
    }
    @autoreleasepool {
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.contentsScale = 1;
        layer.bounds = CGRectMake(0, 0, width, height);
        window = apkrt_window_add((__bridge void *)layer, width, height);
    }
    if (window == 0 || apkrt_window_set_gate(window, 1) != 0) {
        fprintf(stderr, "window host: the runtime refused the window (%llu)\n", (unsigned long long)window);
        exit(2);
    }
    const char *script = getenv("MINATO_WINDOW_SCRIPT");
    if (script != NULL) {
        pthread_t thread;
        if (sscanf(script, "background:%lu", &presents) != 1) {
            fprintf(stderr, "window host: MINATO_WINDOW_SCRIPT=%s is not background:<presents>\n", script);
            exit(2);
        }
        pthread_create(&thread, NULL, background, NULL);
        pthread_detach(thread);
    }
}
