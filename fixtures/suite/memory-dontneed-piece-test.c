// memory-dontneed-piece-test: MADV_DONTNEED of a 4 KiB piece inside a 16 KiB host page zeroes exactly that piece (a
// private anonymous mapping reads zero after DONTNEED) and keeps the live neighbours' bytes. A 16 KiB-page runtime must
// zero byte-precisely, not with MADV_ZERO (which rounds to the whole host page and would wipe the neighbours). Android
// 15 (4 KiB pages) drops the one page (reads zero) and the neighbour pages keep their bytes. Android 15's output is the
// expected one.
// Expected output: fixtures/expected/memory-dontneed-piece-test.out
#include "memory_helpers.h"

static void dontneed_a_piece(void) {
    const size_t host = 16 * 1024;
    unsigned char *p = (unsigned char *)anonymous(host, PROT_READ | PROT_WRITE, 0);
    memset(p, 0x11, host);
    madvise(p + 4096, 4096, MADV_DONTNEED);
    printf("dontneed_piece: the piece reads %02x, the neighbours %02x %02x\n", p[4096], p[0], p[8192]);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    dontneed_a_piece();
    return 0;
}
