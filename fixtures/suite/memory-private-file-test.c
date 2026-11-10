// memory-private-file-test: a MAP_PRIVATE|PROT_WRITE mapping of a file, written over and advised away with
// MADV_DONTNEED, reads the file's bytes again, not zeros and not the bytes written (Linux drops the private copies, the
// file's pages show through). Its own test, since the page cache of a private file mapping is what Minato's
// MADV_DONTNEED has to reopen the file for. Android 15's output is the expected one.
// Expected output: fixtures/expected/memory-private-file-test.out
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void print_bytes(const char *name, const volatile unsigned char *memory, size_t page) {
    printf("%s: first bytes of 4 pages %02x %02x %02x %02x\n", name, memory[0], memory[page], memory[2 * page],
           memory[3 * page]);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const char *path = "memory-private-file-test.tmp";
    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    unsigned char *bytes = malloc(4 * page);
    if (fd < 0 || bytes == NULL) {
        printf("no file\n");
        return 1;
    }
    memset(bytes, 0x11, 4 * page);
    if (write(fd, bytes, 4 * page) != (ssize_t)(4 * page)) printf("short write\n");
    free(bytes);
    unsigned char *memory = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (memory == MAP_FAILED) {
        printf("no mapping\n");
        return 1;
    }
    print_bytes("untouched", memory, page);
    for (int index = 0; index < 4; ++index) memory[(size_t)index * page] = 0x22;
    print_bytes("written over", memory, page);
    madvise(memory, 4 * page, MADV_DONTNEED);
    print_bytes("after MADV_DONTNEED", memory, page);
    munmap(memory, 4 * page);
    close(fd);
    unlink(path);
    return 0;
}
