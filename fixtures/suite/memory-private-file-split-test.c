// memory-private-file-split-test: MADV_DONTNEED picks the right record when an anonymous MAP_FIXED mapping has
// replaced part of a private file mapping. A 4-page file (0x11) is mapped MAP_PRIVATE and written over (0x22); then a
// MAP_FIXED | MAP_ANON mapping replaces the two middle pages (written 0x44). After MADV_DONTNEED of the whole range,
// the file ends read the file's bytes again (the private copies drop / Minato re-maps the file) and the anonymous
// middle reads zero -- proving the runtime no longer holds a stale file record over the middle (its record was
// trimmed when the anonymous mapping overwrote it; a stale one would wrongly re-map the middle to the file). Android
// 15 and Minato print the same line.
// Expected output: fixtures/expected/memory-private-file-split-test.out
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const char *path = "memory-private-file-split-test.tmp";
    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    unsigned char *bytes = malloc(4 * page);
    if (fd < 0 || bytes == NULL) {
        printf("no file\n");
        return 1;
    }
    memset(bytes, 0x11, 4 * page);
    if (write(fd, bytes, 4 * page) != (ssize_t)(4 * page)) printf("short write\n");
    free(bytes);
    volatile unsigned char *memory = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (memory == MAP_FAILED) {
        printf("no mapping\n");
        return 1;
    }
    for (int index = 0; index < 4; ++index) memory[(size_t)index * page] = 0x22;
    // The two middle pages become anonymous, overwriting that part of the file mapping in place.
    void *middle = (void *)(memory + page);
    if (mmap(middle, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
        printf("no overwrite\n");
        return 1;
    }
    memory[page] = 0x44;
    memory[2 * page] = 0x44;
    madvise((void *)memory, 4 * page, MADV_DONTNEED);
    printf("first bytes of 4 pages %02x %02x %02x %02x\n", memory[0], memory[page], memory[2 * page], memory[3 * page]);
    munmap((void *)memory, 4 * page);
    close(fd);
    unlink(path);
    return 0;
}
