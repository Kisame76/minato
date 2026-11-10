// memory-mmap-shared-neighbour-test: a sub-page anonymous MAP_FIXED must never write into a SHARED object. A 16 KiB
// memfd mapped MAP_SHARED (filled 0x11) has a second mapping of the same object; munmap a 4 KiB piece of the first
// mapping and MAP_FIXED anonymous over the piece. The memfd's bytes, read through the second mapping, must stay 0x11:
// Linux gives the piece a private anonymous page and leaves the object; a 16 KiB-page runtime must refuse the sub-page
// anon placement over a shared host page rather than memset it (which would zero the object). Both keep the memfd's
// bytes, so only those are printed. Android 15's output is the expected one.
// Expected output: fixtures/expected/memory-mmap-shared-neighbour-test.out
#include "memory_helpers.h"

static void shared_neighbour(void) {
    const size_t host = 16 * 1024;
    int fd = (int)syscall(__NR_memfd_create, "shared-neighbour", 0);
    if (fd < 0 || ftruncate(fd, (off_t)host) != 0) {
        printf("no memfd\n");
        return;
    }
    unsigned char *first = (unsigned char *)mmap(NULL, host, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char *second = (unsigned char *)mmap(NULL, host, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(first, 0x11, host);
    munmap(first + 4096, 4096);  // free a 4 KiB piece of the first mapping; the host page stays (shared neighbours)
    mmap(first + 4096, 4096, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("shared_neighbour: the memfd reads %02x\n", second[4096]);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    shared_neighbour();
    return 0;
}
