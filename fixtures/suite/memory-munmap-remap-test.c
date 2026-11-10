// memory-munmap-remap-test: a guest unmaps some 4 KiB pieces of a host page but leaves one piece live, so the page is
// kept; then a fresh mapping is placed over the whole page (MAP_FIXED here; an mremap or a hint that lands there is the
// same) and makes every piece live again. Unmapping the one old piece that was still live must now free only that
// piece, not the whole page -- the fresh mapping's pieces must stay readable. Android 15 (4 KiB pages) frees each page
// on its own, so the others stay; where the pages are larger the runtime must match, or it drops live memory it had
// just handed out. Android 15's output is the expected one.
// Expected output: fixtures/expected/memory-munmap-remap-test.out
#include "memory_helpers.h"

static void remap_over_a_partial_page(void) {
    const size_t host = 16 * 1024;
    char *p = anonymous(host, PROT_READ | PROT_WRITE, 0);
    touch(p, host);
    munmap(p, 4096);          // piece 0
    munmap(p + 4096, 4096);   // piece 1
    munmap(p + 8192, 4096);   // piece 2; piece 3 stays live, so a 16 KiB-page runtime keeps the host page
    char *again = (char *)mmap(p, host, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (again != p) {
        printf("remap_over_partial: the remap failed %s\n", name_of(errno));
        return;
    }
    touch(p, host);           // the whole page is a fresh mapping now
    munmap(p + 12288, 4096);  // the one old piece that was still live
    printf("remap_over_partial: the remapped pieces readable %d%d%d\n", readable((const unsigned char *)p),
           readable((const unsigned char *)p + 4096), readable((const unsigned char *)p + 8192));
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (rss() < 0) {
        printf("no resident size\n");
        return 1;
    }
    remap_over_a_partial_page();
    return 0;
}
