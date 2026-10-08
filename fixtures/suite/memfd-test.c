// memfd-test: memfd_create and its seals as Android 15's kernel (Linux 6.6) has them. libcutils' ashmem takes this
// path once sys.use_memfd is on (ASharedMemory, SharedMemory, MemoryFile, CursorWindow's window): the flags and the
// name, the file it makes (fstat, /proc/self/fd, /proc/self/maps), its size, reads, writes and shared mappings, each
// seal with its checks, a reopen through /proc/self/fd and a dup, which share the seals, and the files that have none.
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/memfd.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// A size every page size divides: 4 KiB on the emulator, 16 KiB on the iPhone.
#define SIZE 65536

static const char *error_name(int error) {
    switch (error) {
        case EPERM: return "EPERM";
        case ENOENT: return "ENOENT";
        case EBADF: return "EBADF";
        case ENOMEM: return "ENOMEM";
        case EACCES: return "EACCES";
        case EFAULT: return "EFAULT";
        case EBUSY: return "EBUSY";
        case EINVAL: return "EINVAL";
        case ENODEV: return "ENODEV";
        case ENOSYS: return "ENOSYS";
        case EOPNOTSUPP: return "EOPNOTSUPP";
        default: {
            static char other[24];
            snprintf(other, sizeof other, "errno %d", error);
            return other;
        }
    }
}

// The call's answer: its value, or -1 and errno's name.
static void show(const char *label, long result) {
    if (result < 0) {
        printf("%s: -1 %s\n", label, error_name(errno));
    } else {
        printf("%s: %ld\n", label, result);
    }
}

static int create(const char *name, unsigned flags) {
    return (int)syscall(__NR_memfd_create, name, flags);
}

// Whether memfd_create takes `name` and `flags`.
static void show_create(const char *label, const char *name, unsigned flags) {
    const int fd = create(name, flags);
    show(label, fd < 0 ? -1 : 0);
    if (fd >= 0) close(fd);
}

// mmap of SIZE bytes of `fd` from its start: "ok", or the error. The mapping stays.
static char *show_map(const char *label, int protection, int flags, int fd) {
    void *mapping = mmap(NULL, SIZE, protection, flags, fd, 0);
    if (mapping == MAP_FAILED) {
        printf("%s: -1 %s\n", label, error_name(errno));
        return NULL;
    }
    printf("%s: ok\n", label);
    return mapping;
}

// What the link of /proc/self/fd shows for `fd`.
static void show_link(const char *label, int fd) {
    char link[64], target[512];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    const ssize_t length = readlink(link, target, sizeof target - 1);
    if (length < 0) {
        printf("%s: -1 %s\n", label, error_name(errno));
        return;
    }
    target[length] = '\0';
    printf("%s: %s\n", label, target);
}

// The permissions and the name /proc/self/maps gives the mapping that starts at `mapping`.
static void show_maps(const char *label, const void *mapping) {
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[1024];
    while (maps != NULL && fgets(line, sizeof line, maps) != NULL) {
        unsigned long start = 0, end = 0;
        char permissions[8] = "", name[512] = "";
        // start-end permissions offset device inode name
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &start, &end, permissions, name) >= 3 &&
            start == (unsigned long)mapping) {
            printf("%s: %s %s\n", label, permissions, name);
            fclose(maps);
            return;
        }
    }
    if (maps != NULL) fclose(maps);
    printf("%s: not found\n", label);
}

static void show_stat(const char *label, int fd) {
    struct stat status;
    if (fstat(fd, &status) != 0) {
        printf("%s: -1 %s\n", label, error_name(errno));
        return;
    }
    printf("%s: regular %d, mode %o, links %lu, size %lld, the process's uid %d\n", label,
           S_ISREG(status.st_mode) ? 1 : 0, (unsigned)(status.st_mode & 07777), (unsigned long)status.st_nlink,
           (long long)status.st_size, status.st_uid == getuid() ? 1 : 0);
}

static long seals_of(int fd) {
    return fcntl(fd, F_GET_SEALS);
}

static long add_seals(int fd, int seals) {
    return fcntl(fd, F_ADD_SEALS, seals);
}

static void flags_and_names(void) {
    char name[260];
    memset(name, 'n', sizeof name);
    name[249] = '\0';
    show_create("a name of 249 bytes", name, 0);
    name[249] = 'n';
    name[250] = '\0';
    show_create("a name of 250 bytes", name, 0);
    show("no name", syscall(__NR_memfd_create, NULL, 0));
    show_create("an unknown flag", "minato", 0x100);
    show_create("MFD_EXEC with MFD_NOEXEC_SEAL", "minato", MFD_EXEC | MFD_NOEXEC_SEAL);
    show_create("MFD_HUGETLB", "minato", MFD_HUGETLB);
    show_create("MFD_HUGETLB of 2 MB", "minato", MFD_HUGETLB | MFD_HUGE_2MB);
    show_create("a size without MFD_HUGETLB", "minato", MFD_HUGE_2MB);
    const int slash = create("with/slash", 0);
    show_link("a name with a slash", slash);
    close(slash);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    flags_and_names();

    // As libcutils makes one (memfd_create_region).
    const int fd = create("minato-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    show("memfd_create", fd < 0 ? -1 : 0);
    show("close-on-exec", fcntl(fd, F_GETFD) & FD_CLOEXEC);
    show("access mode", fcntl(fd, F_GETFL) & O_ACCMODE);
    show_stat("its file", fd);
    show_link("its link", fd);
    show("its seals", seals_of(fd));
    show("ftruncate to 64 KiB", ftruncate(fd, SIZE));
    char buffer[8] = "";
    show("pwrite at 0", pwrite(fd, "hello", 5, 0));
    show("pread at 0", pread(fd, buffer, 5, 0));
    printf("read: %s\n", buffer);
    show("lseek to the end", lseek(fd, 0, SEEK_END));
    show("write at the end", write(fd, "!", 1));
    show_stat("after it", fd);
    show("ftruncate back", ftruncate(fd, SIZE));

    // Two mappings and the file share their pages.
    char *shared = show_map("a shared writable mapping", PROT_READ | PROT_WRITE, MAP_SHARED, fd);
    char *seen = show_map("a shared read-only mapping", PROT_READ, MAP_SHARED, fd);
    if (shared != NULL && seen != NULL) {
        shared[100] = 'x';
        printf("the other mapping sees its write: %c\n", seen[100]);
        memset(buffer, 0, sizeof buffer);
        pread(fd, buffer, 1, 100);
        printf("pread sees it: %c\n", buffer[0]);
        pwrite(fd, "y", 1, 200);
        printf("both see a pwrite: %c %c\n", shared[200], seen[200]);
        show_maps("maps, the writable one", shared);
        show_maps("maps, the read-only one", seen);
        show("MADV_DONTNEED", madvise(shared, SIZE, MADV_DONTNEED));
        printf("kept after it: %c %c\n", shared[100], seen[100]);
    }

    // The seals in libcutils' order: the size first, then writes.
    show("an unknown seal", add_seals(fd, 0x40));
    show("F_SEAL_SHRINK", add_seals(fd, F_SEAL_SHRINK));
    show("shrinking", ftruncate(fd, SIZE / 2));
    show("growing", ftruncate(fd, 2 * SIZE));
    show("F_SEAL_GROW", add_seals(fd, F_SEAL_GROW));
    show("growing then", ftruncate(fd, 4 * SIZE));
    show("the same size", ftruncate(fd, 2 * SIZE));
    show("pwrite past the end", pwrite(fd, "z", 1, 2 * SIZE));
    show("pwrite across the end", pwrite(fd, "zz", 2, 2 * SIZE - 1));
    show("pwrite inside", pwrite(fd, "z", 1, 300));
    show("fallocate inside", fallocate(fd, 0, 0, SIZE));
    show("fallocate past the end", fallocate(fd, 0, 2 * SIZE, 4096));
    show("fallocate past the end, keeping the size", fallocate(fd, FALLOC_FL_KEEP_SIZE, 2 * SIZE, 4096));
    show("seals", seals_of(fd));

    show("F_SEAL_FUTURE_WRITE", add_seals(fd, F_SEAL_FUTURE_WRITE));
    lseek(fd, 0, SEEK_SET);
    show("write", write(fd, "z", 1));
    show("pwrite", pwrite(fd, "z", 1, 0));
    show("fallocate", fallocate(fd, 0, 0, SIZE));
    show("punching a hole", fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096));
    char *read_only = show_map("a new shared read-only mapping", PROT_READ, MAP_SHARED, fd);
    if (read_only != NULL) show("mprotect to writable", mprotect(read_only, SIZE, PROT_READ | PROT_WRITE));
    show_map("a new shared writable mapping", PROT_READ | PROT_WRITE, MAP_SHARED, fd);
    char *copy = show_map("a new private writable mapping", PROT_READ | PROT_WRITE, MAP_PRIVATE, fd);
    if (copy != NULL && seen != NULL) {
        copy[0] = 'q';
        printf("a private write stays private: %c %c\n", copy[0], seen[0]);
    }
    if (shared != NULL && seen != NULL) {
        shared[300] = 'w';
        printf("the old writable mapping still writes: %c\n", seen[300]);
    }
    show("F_SEAL_SEAL", add_seals(fd, F_SEAL_SEAL));
    show("F_SEAL_WRITE after it", add_seals(fd, F_SEAL_WRITE));
    show("seals", seals_of(fd));

    // A reopen through /proc/self/fd is another open file of the same memfd; a dup is the same open file.
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    const int reopened = open(link, O_RDONLY);
    show("reopened read-only", reopened < 0 ? -1 : 0);
    show("its seals", seals_of(reopened));
    show("a seal through it", add_seals(reopened, F_SEAL_GROW));
    struct stat first, other;
    fstat(fd, &first);
    fstat(reopened, &other);
    printf("the same file: %d\n", first.st_ino == other.st_ino && first.st_dev == other.st_dev);
    show_link("its link", reopened);
    show("lseek of the first to 10", lseek(fd, 10, SEEK_SET));
    show("the reopened one's offset", lseek(reopened, 0, SEEK_CUR));
    memset(buffer, 0, sizeof buffer);
    show("pread through it", pread(reopened, buffer, 5, 0));
    printf("read: %s\n", buffer);
    show("write through it", write(reopened, "z", 1));
    char *again = show_map("a shared read-only mapping of it", PROT_READ, MAP_SHARED, reopened);
    if (again != NULL) show("mprotect to writable", mprotect(again, SIZE, PROT_READ | PROT_WRITE));
    show_map("a shared writable mapping of it", PROT_READ | PROT_WRITE, MAP_SHARED, reopened);
    const int duplicate = dup(fd);
    show("a dup's seals", seals_of(duplicate));
    show("a dup's offset", lseek(duplicate, 0, SEEK_CUR));
    const int writable = open(link, O_RDWR);
    show("reopened writable", writable < 0 ? -1 : 0);
    show("write through that", write(writable, "z", 1));
    close(fd);
    close(duplicate);
    memset(buffer, 0, sizeof buffer);
    show("pread after the first one's close", pread(reopened, buffer, 5, 0));
    printf("read: %s\n", buffer);
    show("seals after it", seals_of(writable));
    close(reopened);
    close(writable);

    // F_SEAL_WRITE waits for no shared mapping of a writable open file to be left.
    const int sealed = create("minato-write", MFD_ALLOW_SEALING);
    show("another memfd", sealed < 0 ? -1 : 0);
    ftruncate(sealed, SIZE);
    char *mapping = show_map("a shared writable mapping", PROT_READ | PROT_WRITE, MAP_SHARED, sealed);
    show("F_SEAL_WRITE with it", add_seals(sealed, F_SEAL_WRITE));
    if (mapping != NULL) munmap(mapping, SIZE);
    mapping = show_map("a shared read-only mapping", PROT_READ, MAP_SHARED, sealed);
    show("F_SEAL_WRITE with that", add_seals(sealed, F_SEAL_WRITE));
    if (mapping != NULL) munmap(mapping, SIZE);
    show_map("a private writable mapping", PROT_READ | PROT_WRITE, MAP_PRIVATE, sealed);
    show("F_SEAL_WRITE with that", add_seals(sealed, F_SEAL_WRITE));
    show("pwrite then", pwrite(sealed, "z", 1, 0));
    show_map("a new shared read-only mapping", PROT_READ, MAP_SHARED, sealed);
    show_map("a new shared writable mapping", PROT_READ | PROT_WRITE, MAP_SHARED, sealed);
    show_map("a new private writable mapping", PROT_READ | PROT_WRITE, MAP_PRIVATE, sealed);
    show("growing", ftruncate(sealed, 2 * SIZE));
    snprintf(link, sizeof link, "/proc/self/fd/%d", sealed);
    const int sealed_reader = open(link, O_RDONLY);
    show_map("a shared read-only mapping of a read-only reopen", PROT_READ, MAP_SHARED, sealed_reader);
    show("seals", seals_of(sealed));
    close(sealed_reader);
    close(sealed);

    // Without MFD_ALLOW_SEALING, F_SEAL_SEAL comes first; MFD_NOEXEC_SEAL and MFD_EXEC set the mode.
    const int plain = create("minato-plain", 0);
    show("without MFD_ALLOW_SEALING: seals", seals_of(plain));
    show("a seal there", add_seals(plain, F_SEAL_GROW));
    show_stat("its file", plain);
    close(plain);
    const int no_exec = create("minato-noexec", MFD_NOEXEC_SEAL);
    show("MFD_NOEXEC_SEAL: seals", seals_of(no_exec));
    show_stat("its file", no_exec);
    show("a seal there", add_seals(no_exec, F_SEAL_GROW));
    close(no_exec);
    const int exec = create("minato-exec", MFD_EXEC | MFD_ALLOW_SEALING);
    show("MFD_EXEC: seals", seals_of(exec));
    show_stat("its file", exec);
    show("F_SEAL_EXEC", add_seals(exec, F_SEAL_EXEC));
    // Its mode has execute bits, so F_SEAL_EXEC brings the seals of writes and of the size along.
    show("seals then", seals_of(exec));
    close(exec);

    // Files that are no memfd have no seals.
    const int file = open("memfd-test.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
    show("a file's seals", seals_of(file));
    show("a seal on a file", add_seals(file, F_SEAL_SEAL));
    close(file);
    unlink("memfd-test.tmp");
    int ends[2];
    pipe(ends);
    show("a pipe's seals", seals_of(ends[0]));
    show("a seal on its read end", add_seals(ends[0], F_SEAL_SEAL));
    show("a seal on its write end", add_seals(ends[1], F_SEAL_SEAL));
    close(ends[0]);
    close(ends[1]);
    show("the seals of a closed number", seals_of(ends[0]));
    return 0;
}
