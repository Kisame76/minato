// Red-first for the ADR-0035 amendment (scudo RegionSizeLog 26 -> 27): a single size class must hold
// more than the old 64 MiB region. Every allocation here falls in one size class (a request of 784
// bytes plus scudo's 16-byte header is the 0x320 = 800-byte class, which Ren'Py 7.8.7 exhausted on
// the iPhone). It asks for 80 MiB of that class -- past a 64 MiB region (RegionSizeLog 26), within a
// 128 MiB one (27). With the 64 MiB region scudo cannot populate the class's region to hold it and
// logs "Can't populate more pages for size class 800" to stderr (the message Ren'Py hit), which fails
// the test; with the 128 MiB region it fits cleanly, scudo stays quiet, and the program prints "ok".
// The NULL guard stays as a second line of defence should a build instead refuse the allocation.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    enum { request = 784 };                       // + scudo's 16-byte header -> size class 0x320 (800 bytes)
    const size_t target = (size_t)80 << 20;       // 80 MiB of this one class
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
