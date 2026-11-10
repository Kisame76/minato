// memory-munmap-piece-test: a munmap of 4096 bytes out of the middle of a 64 KiB mapping, as a program that
// hardcodes the page size as 4096 (Mono and il2cpp memory pools do) makes it. Android 15 does it (its pages are
// 4096 bytes) and the pieces next to it stay readable; where the pages are larger, a munmap of a piece smaller than a
// page must still do what a 4 KiB system does, or the memory stays mapped. Android 15's output is the expected one.
// Expected output: fixtures/expected/memory-munmap-piece-test.out
#include "memory_helpers.h"

static void munmap_4096_piece(void) {
    char *small = anonymous(64 * 1024, PROT_READ | PROT_WRITE, 0);
    touch(small, 64 * 1024);
    const int result = munmap(small + 4096, 4096);
    printf("munmap_4096_piece: %s, the pieces next to it readable %d%d\n", result == 0 ? "ok" : name_of(errno),
           readable((const unsigned char *)small), readable((const unsigned char *)small + 8192));
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (rss() < 0) {
        printf("no resident size\n");
        return 1;
    }
    munmap_4096_piece();
    return 0;
}
