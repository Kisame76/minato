// memory-mmap-file-piece-test: a MAP_PRIVATE FILE mapping of a 4 KiB piece into a live host page reads the file's
// bytes, leaving the neighbours. A 16 KiB-page runtime cannot point a sub-page at the pager while a neighbour shares
// the host page, so it copies the file's bytes in (a private copy, as MAP_PRIVATE allows); the neighbours keep their
// own bytes. Android 15 (4 KiB pages) maps the file into the one page and the neighbour pages keep theirs. Android
// 15's output is the expected one.
// Expected output: fixtures/expected/memory-mmap-file-piece-test.out
#include "memory_helpers.h"

static void file_piece(void) {
    const size_t host = 16 * 1024;
    int fd = (int)syscall(__NR_memfd_create, "file-piece", 0);
    if (fd < 0 || ftruncate(fd, 4096) != 0) {
        printf("no memfd\n");
        return;
    }
    unsigned char ones[4096];
    memset(ones, 0x11, sizeof ones);
    if (pwrite(fd, ones, sizeof ones, 0) != (ssize_t)sizeof ones) {
        printf("no write\n");
        return;
    }
    unsigned char *region = (unsigned char *)anonymous(host, PROT_READ | PROT_WRITE, 0);
    memset(region, 0x22, host);
    munmap(region + 4096, 4096);  // free the middle piece; the host page stays, the neighbours keep 0x22
    void *at = mmap(region + 4096, 4096, PROT_READ, MAP_FIXED | MAP_PRIVATE, fd, 0);
    if (at != region + 4096) {
        printf("file_piece: the map failed %s\n", name_of(errno));
        return;
    }
    printf("file_piece: the piece reads %02x, the neighbours %02x %02x\n", region[4096], region[0], region[8192]);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    file_piece();
    return 0;
}
