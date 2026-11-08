// memory-release-test: ways programs give memory back or hold it that Minato does as Android 15 does: MADV_REMOVE
// on anonymous memory (EINVAL), MADV_COLD, what MADV_DONTNEED leaves to read (zeros for private anonymous memory, the
// written bytes for shared anonymous memory and a memfd), mremap shrinking and growing, a munmap of one page out of a
// larger mapping and a mprotect of 4096 bytes, a garbage collector's reservation (a large PROT_NONE MAP_NORESERVE
// mapping with parts made readable, touched and decommitted by mmap over it, as Boehm, Mono and il2cpp do), and
// mprotect
// PROT_NONE on touched memory (the contents come back). Each prints the result of the call and, where Android's
// answer is about memory, whether the pages left the process's resident size straight after (field 24 of
// /proc/self/stat).
// Android 15's output is the expected one. The ways Minato still differs are in memory-munmap-piece-test,
// memory-remove-memfd-test, memory-pageout-test and memory-private-file-test. With a scenario's name as the argument
// only that scenario runs, and "list" prints the names, for tools/memory-churn-footprint-check.sh.
// Expected output: fixtures/expected/memory-release-test.out
#include "memory_helpers.h"

static void remove_anonymous(void) {
    char *memory = anonymous(16 * kMiB, PROT_READ | PROT_WRITE, 0);
    touch(memory, 16 * kMiB);
    const int result = madvise(memory, 16 * kMiB, MADV_REMOVE);
    printf("madvise_remove_anonymous: %s\n", result == 0 ? "ok" : name_of(errno));
    munmap(memory, 16 * kMiB);
}

static void cold(void) {
    char *memory = anonymous(64 * kMiB, PROT_READ | PROT_WRITE, 0);
    const long before = rss();
    touch(memory, 64 * kMiB);
    const int cold = madvise(memory, 64 * kMiB, MADV_COLD);
    printf("madvise_cold: %s, pages %s\n", cold == 0 ? "ok" : name_of(errno), left(before, rss()));
    munmap(memory, 64 * kMiB);
}

static void print_bytes(const char *name, const volatile unsigned char *memory, size_t page) {
    printf("%s: first bytes of 4 pages %02x %02x %02x %02x\n", name, memory[0], memory[page], memory[2 * page],
           memory[3 * page]);
}

static void fill_first_bytes(unsigned char *memory, size_t page, unsigned char value) {
    for (int index = 0; index < 4; ++index) memory[(size_t)index * page] = value;
}

static void dontneed_private_anonymous(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *memory = anonymous(4 * page, PROT_READ | PROT_WRITE, 0);
    fill_first_bytes(memory, page, 0x55);
    madvise(memory, 4 * page, MADV_DONTNEED);
    print_bytes("dontneed_private_anonymous", memory, page);
    munmap(memory, 4 * page);
}

static void dontneed_shared_anonymous(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *memory = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    fill_first_bytes(memory, page, 0x33);
    madvise(memory, 4 * page, MADV_DONTNEED);
    print_bytes("dontneed_shared_anonymous", memory, page);
    munmap(memory, 4 * page);
}

static void dontneed_memfd(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const int fd = (int)syscall(279 /* memfd_create on arm64 */, "memory-release-test", 0);
    if (fd < 0 || ftruncate(fd, (off_t)(4 * page)) != 0) {
        printf("dontneed_memfd: no memfd: %s\n", name_of(errno));
        return;
    }
    unsigned char *memory = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    fill_first_bytes(memory, page, 0x44);
    madvise(memory, 4 * page, MADV_DONTNEED);
    print_bytes("dontneed_memfd", memory, page);
    munmap(memory, 4 * page);
    close(fd);
}

static void remap(void) {
    char *memory = anonymous(128 * kMiB, PROT_READ | PROT_WRITE, 0);
    const long before = rss();
    touch(memory, 128 * kMiB);
    void *shrunk = mremap(memory, 128 * kMiB, 32 * kMiB, 0);
    printf("mremap_shrink: %s, the cut-off 96 MiB %s\n", shrunk != MAP_FAILED ? "ok" : name_of(errno),
           rss() - before < 128 - 64 ? "left the resident size" : "stay resident");
    if (shrunk == MAP_FAILED) return;
    void *grown = mremap(shrunk, 32 * kMiB, 128 * kMiB, MREMAP_MAYMOVE);
    if (grown == MAP_FAILED) {
        printf("mremap_grow: %s\n", name_of(errno));
        munmap(shrunk, 32 * kMiB);
        return;
    }
    touch(grown, 128 * kMiB);
    printf("mremap_grow: ok, whole block usable %s\n",
           readable((const unsigned char *)grown + 127 * kMiB) ? "yes" : "no");
    munmap(grown, 128 * kMiB);
}

// One page out of the middle of a mapping, and a mprotect of 4096 bytes in a mapping of 64 KiB.
static void munmap_piece(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *memory = anonymous(4 * page, PROT_READ | PROT_WRITE, 0);
    touch(memory, 4 * page);
    int result = munmap(memory + page, page);
    printf("munmap_page: %s, neighbours readable %d%d, the gap %s\n", result == 0 ? "ok" : name_of(errno),
           readable((const unsigned char *)memory), readable((const unsigned char *)memory + 2 * page),
           readable((const unsigned char *)memory + page) ? "readable" : "unmapped");
    munmap(memory, page);
    munmap(memory + 2 * page, 2 * page);

    char *small = anonymous(64 * 1024, PROT_READ | PROT_WRITE, 0);
    touch(small, 64 * 1024);
    result = mprotect(small + 32768, 4096, PROT_READ);
    printf("mprotect_4096_piece: %s, the pieces next to it readable %d%d\n", result == 0 ? "ok" : name_of(errno),
           readable((const unsigned char *)small + 28672), readable((const unsigned char *)small + 36864));
}

// A reservation of 4 GiB, 128 MiB of it made usable, touched and decommitted by mmap over it, in rounds.
static void gc_reserve(void) {
    char *reserve = anonymous(4096 * kMiB, PROT_NONE, MAP_NORESERVE);
    if (reserve == MAP_FAILED) {
        printf("gc_reserve: reserve failed: %s\n", name_of(errno));
        return;
    }
    long warm = 0, base = rss(), after = 0;
    for (int round = 0; round < 12; ++round) {
        int result = 0;
        for (int part = 0; part < 8; ++part) {
            result |= mprotect(reserve + (size_t)part * 16 * kMiB, 16 * kMiB, PROT_READ | PROT_WRITE);
        }
        touch(reserve, 128 * kMiB);
        void *again = mmap(reserve, 128 * kMiB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                           -1, 0);
        if (result != 0 || again != (void *)reserve) {
            printf("gc_reserve: commit or decommit failed: %s\n", name_of(errno));
            return;
        }
        after = rss();
        if (round == 2) warm = after;
    }
    printf("gc_reserve: %s, decommitted pages %s\n", after - warm < 48 ? "flat" : "grows", left(base, after));
    munmap(reserve, 4096 * kMiB);
}

static void protect_none(void) {
    char *memory = anonymous(64 * kMiB, PROT_READ | PROT_WRITE, 0);
    touch(memory, 64 * kMiB);
    memory[4096 * 5] = 42;
    const int result = mprotect(memory, 64 * kMiB, PROT_NONE);
    mprotect(memory, 64 * kMiB, PROT_READ | PROT_WRITE);
    printf("mprotect_none: %s, contents back %s\n", result == 0 ? "ok" : name_of(errno),
           memory[4096 * 5] == 42 ? "yes" : "no");
    munmap(memory, 64 * kMiB);
}

struct scenario {
    const char *name;
    void (*run)(void);
};

static const struct scenario scenarios[] = {
    {"remove_anonymous", remove_anonymous}, {"cold", cold},
    {"dontneed_private_anonymous", dontneed_private_anonymous},
    {"dontneed_shared_anonymous", dontneed_shared_anonymous}, {"dontneed_memfd", dontneed_memfd}, {"remap", remap},
    {"munmap_piece", munmap_piece}, {"gc_reserve", gc_reserve}, {"protect_none", protect_none},
};

int main(int count, char **names) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (count > 1 && strcmp(names[1], "list") == 0) {
        for (size_t index = 0; index < sizeof scenarios / sizeof scenarios[0]; ++index) puts(scenarios[index].name);
        return 0;
    }
    if (rss() < 0) {
        printf("no resident size\n");
        return 1;
    }
    for (size_t index = 0; index < sizeof scenarios / sizeof scenarios[0]; ++index) {
        if (count < 2 || strcmp(names[1], scenarios[index].name) == 0) scenarios[index].run();
    }
    return 0;
}
