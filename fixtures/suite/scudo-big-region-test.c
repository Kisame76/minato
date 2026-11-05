// Red-first for the two libc variants (ADR-0035, item 3): one size class must hold more than a 128 MiB region, so
// that a game needing it runs when the process has the address space for 256 MiB regions. Like scudo-region-test,
// every allocation falls in one size class -- 784 bytes plus scudo's 16-byte header is the 0x320 = 800-byte class --
// but it asks for 200 MiB of that class: past a 128 MiB region (RegionSizeLog 27, libc.so), within a 256 MiB one
// (RegionSizeLog 28, libc-bigregions.so, chosen at start when the address space allows it). With the 128 MiB region
// scudo cannot populate the class's region and logs "Can't populate more pages for size class 800" to stderr, which
// fails the test; with the 256 MiB region it fits and prints "ok". The NULL guard is a second line of defence.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    enum { request = 784 };                        // + scudo's 16-byte header -> size class 0x320 (800 bytes)
    const size_t target = (size_t)200 << 20;       // 200 MiB of this one class: over a 128 MiB region, under 256 MiB
    const size_t count = target / request + 1;
    void **blocks = calloc(count, sizeof *blocks);
    if (blocks == NULL) {
        fprintf(stderr, "no room for the pointer array\n");
        return 1;
    }
    for (size_t i = 0; i < count; ++i) {
        blocks[i] = malloc(request);
        if (blocks[i] == NULL) {
            fprintf(stderr, "malloc of the 800-byte size class returned NULL after %zu MiB\n",
                    (i * (size_t)request) >> 20);
            return 1;
        }
    }
    for (size_t i = 0; i < count; ++i) free(blocks[i]);
    free(blocks);
    printf("ok\n");
    return 0;
}
