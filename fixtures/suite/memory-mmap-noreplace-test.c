// memory-mmap-noreplace-test: MAP_FIXED_NOREPLACE must judge a 4 KiB piece by the guest's own mappings, not the 16 KiB
// host page. A piece freed inside a live host page is FREE (the map succeeds there); a live piece is occupied (EEXIST).
// Android 15 (4 KiB pages) answers the same -- the freed page is free, the live page is taken. Android 15's output is
// the expected one.
// Expected output: fixtures/expected/memory-mmap-noreplace-test.out
#include "memory_helpers.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static void noreplace_sees_pieces(void) {
    const size_t host = 16 * 1024;
    unsigned char *p = (unsigned char *)anonymous(host, PROT_READ | PROT_WRITE, 0);
    munmap(p + 4096, 4096);  // free piece 1; the host page stays while the neighbours live
    void *freed =
        mmap(p + 4096, 4096, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    errno = 0;
    void *live = mmap(p + 8192, 4096, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("noreplace: the freed piece %s, the live piece %s\n", freed == p + 4096 ? "placed" : "failed",
           live == MAP_FAILED ? name_of(errno) : "placed");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    noreplace_sees_pieces();
    return 0;
}
