// memory-munmap-keep-test: a munmap of a 4 KiB piece inside a 16 KiB host page must not touch the live neighbours'
// content. A 16 KiB-page runtime keeps the host page while a neighbour lives, so it must leave the neighbours' bytes
// as they were (it zeroes nothing on unmap; a re-map of the piece is what gives a fresh zero page). Android 15 (4 KiB
// pages) unmaps the one page and the neighbour pages keep their bytes. Android 15's output is the expected one.
// Expected output: fixtures/expected/memory-munmap-keep-test.out
#include "memory_helpers.h"

static void munmap_keeps_neighbours(void) {
    const size_t host = 16 * 1024;
    unsigned char *p = (unsigned char *)anonymous(host, PROT_READ | PROT_WRITE, 0);
    memset(p, 0x11, host);
    munmap(p + 4096, 4096);  // free the middle piece; the neighbours stay mapped and must keep their bytes
    printf("munmap_keep: the neighbours read %02x %02x\n", p[0], p[8192]);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    munmap_keeps_neighbours();
    return 0;
}
