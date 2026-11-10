// memory-mmap-reuse-test: a guest that frees in 4 KiB steps maps back in 4 KiB steps -- after munmap(block, 4096) it
// does mmap(block, 4096, MAP_FIXED|MAP_PRIVATE|MAP_ANON) to reuse the block (Mono and il2cpp memory pools). On 16 KiB
// host pages the piece sits inside a page a neighbour keeps alive, so the runtime must place the fixed sub-page map and
// give it a fresh zero page, as Linux does, without disturbing the neighbours. Android 15 (4 KiB pages) maps a fresh
// zero page at the address; the larger-page runtime must match. Android 15's output is the expected one.
// Expected output: fixtures/expected/memory-mmap-reuse-test.out
#include "memory_helpers.h"

static void mmap_reuse_a_piece(void) {
    const size_t host = 16 * 1024;
    unsigned char *p = (unsigned char *)anonymous(host, PROT_READ | PROT_WRITE, 0);
    memset(p, 0x11, host);
    munmap(p + 4096, 4096);  // free the piece; the neighbours keep the host page alive on a 16 KiB-page runtime
    unsigned char *again = (unsigned char *)mmap(p + 4096, 4096, PROT_READ | PROT_WRITE,
                                                 MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (again != p + 4096) {
        printf("mmap_reuse: the remap failed %s\n", name_of(errno));
        return;
    }
    printf("mmap_reuse: the piece reads %02x, neighbours %02x %02x\n", again[0], p[0], p[8192]);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    mmap_reuse_a_piece();
    return 0;
}
