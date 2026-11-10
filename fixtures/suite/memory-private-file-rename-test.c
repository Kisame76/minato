// memory-private-file-rename-test: safety of MADV_DONTNEED on a written MAP_PRIVATE file mapping when the path is
// replaced under it. A private mapping of file A (0x11), written over (0x22); then file B (0x33) is renamed over A's
// path, so the path now names a different inode while the mapping still refers to A. After MADV_DONTNEED the mapping
// must NEVER show B's bytes: on Linux it shows A's inode (the private copies drop), and Minato keeps the written bytes
// (it reopens by path, sees a different inode, and leaves the mapping as is) -- neither is file B. The test prints only
// that safety property, so its output is the same on Android and on Minato (the 0x11-vs-0x22 difference is an ADR-0007
// deviation, recorded there, not asserted here).
// Expected output: fixtures/expected/memory-private-file-rename-test.out
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int write_file(const char *path, unsigned char byte, size_t size) {
    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    unsigned char *bytes = malloc(size);
    if (bytes == NULL) {
        close(fd);
        return -1;
    }
    memset(bytes, byte, size);
    const ssize_t written = write(fd, bytes, size);
    free(bytes);
    close(fd);
    return written == (ssize_t)size ? 0 : -1;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const char *path = "memory-private-file-rename-test.tmp";
    const char *other = "memory-private-file-rename-test-other.tmp";
    if (write_file(path, 0x11, page) != 0 || write_file(other, 0x33, page) != 0) {
        printf("no files\n");
        return 1;
    }
    const int fd = open(path, O_RDWR);
    volatile unsigned char *memory = fd < 0 ? MAP_FAILED : mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (memory == MAP_FAILED) {
        printf("no mapping\n");
        return 1;
    }
    memory[0] = 0x22;                       // the private copy the mapping now holds
    if (rename(other, path) != 0) {         // the path names file B (0x33) now; the mapping still refers to A
        printf("no rename\n");
        return 1;
    }
    madvise((void *)memory, page, MADV_DONTNEED);
    printf("shows the other file: %s\n", memory[0] == 0x33 ? "true" : "false");
    munmap((void *)memory, page);
    close(fd);
    unlink(path);
    return 0;
}
