// sharedmem-test: the NDK's ASharedMemory (API 26) as an app uses it: a region of a size, its size, two shared
// mappings and a read of it, then setProt to read-only, after which no writable shared mapping comes and the old one
// still writes; the answers for a size of 0, a widening, and descriptors that are no region. Android 15 makes the
// region with ashmem, Minato with memfd_create, as libcutils' memfd path does: each line here is the same for both.
#include <android/sharedmem.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// A size every page size divides: 4 KiB on the emulator, 16 KiB on the iPhone.
#define SIZE 65536

static const char *error_name(int error) {
    switch (error) {
        case EPERM: return "EPERM";
        case EBADF: return "EBADF";
        case ENOMEM: return "ENOMEM";
        case EACCES: return "EACCES";
        case EINVAL: return "EINVAL";
        case ENOSYS: return "ENOSYS";
        case ENOTTY: return "ENOTTY";
        default: {
            static char other[24];
            snprintf(other, sizeof other, "errno %d", error);
            return other;
        }
    }
}

// mmap of SIZE bytes of `fd`: "ok", or the error. The mapping stays.
static char *show_map(const char *label, int protection, int fd) {
    void *mapping = mmap(NULL, SIZE, protection, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        printf("%s: -1 %s\n", label, error_name(errno));
        return NULL;
    }
    printf("%s: ok\n", label);
    return mapping;
}

static void show_result(const char *label, int result) {
    if (result < 0) {
        printf("%s: %d %s\n", label, result, error_name(errno));
    } else {
        printf("%s: %d\n", label, result);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("a size of 0: %d\n", ASharedMemory_create("minato-empty", 0));
    const int fd = ASharedMemory_create("minato-shared", SIZE);
    printf("create: %s\n", fd >= 0 ? "ok" : error_name(errno));
    printf("close-on-exec: %d\n", fcntl(fd, F_GETFD) & FD_CLOEXEC);
    printf("its size: %zu\n", ASharedMemory_getSize(fd));
    int ends[2];
    pipe(ends);
    printf("a pipe's size: %zu\n", ASharedMemory_getSize(ends[0]));
    printf("the size of -1: %zu\n", ASharedMemory_getSize(-1));

    char *writable = show_map("a shared writable mapping", PROT_READ | PROT_WRITE, fd);
    char *readable = show_map("a shared read-only mapping", PROT_READ, fd);
    char byte = 0;
    if (writable != NULL && readable != NULL) {
        writable[10] = 'a';
        printf("the other mapping sees its write: %c\n", readable[10]);
        lseek(fd, 10, SEEK_SET);
        printf("a read: %zd %c\n", read(fd, &byte, 1), byte);
    }
    show_result("setProt to read and write", ASharedMemory_setProt(fd, PROT_READ | PROT_WRITE));
    show_result("setProt to read-only", ASharedMemory_setProt(fd, PROT_READ));
    show_map("a new shared writable mapping", PROT_READ | PROT_WRITE, fd);
    char *only = show_map("a new shared read-only mapping", PROT_READ, fd);
    if (only != NULL) {
        printf("it reads: %c\n", only[10]);
        show_result("mprotect to writable", mprotect(only, SIZE, PROT_READ | PROT_WRITE));
    }
    if (writable != NULL && only != NULL) {
        writable[20] = 'b';
        printf("the old writable mapping still writes: %c\n", only[20]);
    }
    errno = 0;
    show_result("setProt to read and write again", ASharedMemory_setProt(fd, PROT_READ | PROT_WRITE));
    printf("its size then: %zu\n", ASharedMemory_getSize(fd));

    const int second = ASharedMemory_create(NULL, 16384);
    printf("a second one without a name: %s, size %zu\n", second >= 0 ? "ok" : error_name(errno),
           ASharedMemory_getSize(second));
    close(second);
    close(fd);
    printf("the size after close: %zu\n", ASharedMemory_getSize(fd));
    return 0;
}
