// memory-munmap-fill-test: an allocator that gives memory back in 4 KiB steps (as Mono and il2cpp memory pools do)
// must not leak on 16 KiB host pages. When the last 4 KiB piece of a host page is unmapped the whole page is returned,
// so the resident size falls to about what it was. Android 15 (4 KiB pages) frees each page at once; both end with the
// pages gone. Android 15's output is the expected one.
// Expected output: fixtures/expected/memory-munmap-fill-test.out
#include "memory_helpers.h"

static void munmap_in_pieces(void) {
    const size_t size = 64 * kMiB;
    char *memory = anonymous(size, PROT_READ | PROT_WRITE, 0);
    const long before = rss();
    touch(memory, size);
    const char *status = "ok";
    for (size_t at = 0; at < size; at += 4096) {
        if (munmap(memory + at, 4096) != 0) {
            status = name_of(errno);
            break;
        }
    }
    printf("munmap_in_4096_steps: %s, pages %s\n", status, left(before, rss()));
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (rss() < 0) {
        printf("no resident size\n");
        return 1;
    }
    munmap_in_pieces();
    return 0;
}
