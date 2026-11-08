// memory-churn-test: memory that is allocated and released again in rounds must not add up. A game that loads and
// frees large blocks each level is killed by iOS's memory limit on a phone where Android keeps it flat, if one of the
// ways to release memory leaves it held. Each scenario runs rounds of one way and prints "flat" when the process's
// resident size (field 24 of /proc/self/stat, which Android and Minato both give) after the last round is under 48 MiB
// above its size after the third; the madvise scenarios also print whether the pages left the resident size
// straight after the advice, which is Android's answer (the same on both is the point, not which one it is). A
// scenario that grows prints its numbers in MiB, so that a diff shows them. Android 15's output is the expected one.
// Expected output: fixtures/expected/memory-churn-test.out
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MADV_FREE
#define MADV_FREE 8
#endif

enum { kRounds = 12, kWarm = 3 };
static const size_t kMiB = 1 << 20;
static const long kSlack = 48;  // MiB

// The process's resident size in MiB.
static long rss(void) {
    char text[1024];
    FILE *file = fopen("/proc/self/stat", "r");
    if (file == NULL) return -1;
    size_t size = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[size] = 0;
    const char *after_name = strrchr(text, ')');
    long pages = -1;
    if (after_name == NULL ||
        sscanf(after_name + 1,
               " %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %*lu %*lu %*ld %*ld %*ld %*ld %*ld %*ld %*llu %*lu"
               " %ld", &pages) != 1) {
        return -1;
    }
    return (long)((unsigned long)pages * (unsigned long)sysconf(_SC_PAGESIZE) / kMiB);
}

static void touch(void *memory, size_t size) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t at = 0; at < size; at += page) ((volatile unsigned char *)memory)[at] = (unsigned char)(at >> 12);
}

static void verdict(const char *name, long warm, long end) {
    if (end - warm < kSlack) {
        printf("%s: flat\n", name);
    } else {
        printf("%s: grows from %ld MiB to %ld MiB over %d rounds\n", name, warm, end, kRounds - kWarm);
    }
}

// Blocks of 1 to 64 MiB through malloc and free.
static void malloc_free(void) {
    long warm = 0;
    for (int round = 0; round < kRounds; ++round) {
        void *blocks[7];
        for (int index = 0; index < 7; ++index) {
            blocks[index] = malloc(kMiB << index);
            if (blocks[index] == NULL) {
                printf("malloc_free: malloc failed\n");
                return;
            }
            touch(blocks[index], kMiB << index);
        }
        for (int index = 0; index < 7; ++index) free(blocks[index]);
        if (round == kWarm - 1) warm = rss();
    }
    verdict("malloc_free", warm, rss());
}

// A 128 MiB mapping unmapped in eight pieces, last piece first.
static void mmap_pieces(void) {
    long warm = 0;
    for (int round = 0; round < kRounds; ++round) {
        char *memory = mmap(NULL, 128 * kMiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            printf("mmap_pieces: mmap failed\n");
            return;
        }
        touch(memory, 128 * kMiB);
        for (int piece = 7; piece >= 0; --piece) munmap(memory + (size_t)piece * 16 * kMiB, 16 * kMiB);
        if (round == kWarm - 1) warm = rss();
    }
    verdict("mmap_pieces", warm, rss());
}

// One 128 MiB mapping touched, advised away and touched again. `advice` is MADV_DONTNEED or MADV_FREE.
static void advise(const char *name, int advice) {
    char *memory = mmap(NULL, 128 * kMiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        printf("%s: mmap failed\n", name);
        return;
    }
    long warm = 0, base = rss(), after = 0;
    for (int round = 0; round < kRounds; ++round) {
        touch(memory, 128 * kMiB);
        if (madvise(memory, 128 * kMiB, advice) != 0) {
            printf("%s: madvise failed: %s\n", name, strerror(errno));
            munmap(memory, 128 * kMiB);
            return;
        }
        after = rss();
        if (round == kWarm - 1) warm = after;
    }
    verdict(name, warm, after);
    printf("%s: pages %s\n", name, after - base < 32 ? "left the resident size at once" : "stay resident");
    munmap(memory, 128 * kMiB);
}

// A file mapped MAP_PRIVATE and written to, every page of it, then unmapped.
static void private_file(void) {
    const char *path = "memory-churn-test.tmp";
    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)(128 * kMiB)) != 0) {
        printf("private_file: no file: %s\n", strerror(errno));
        return;
    }
    long warm = 0;
    for (int round = 0; round < kRounds; ++round) {
        char *memory = mmap(NULL, 128 * kMiB, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (memory == MAP_FAILED) {
            printf("private_file: mmap failed\n");
            break;
        }
        touch(memory, 128 * kMiB);
        munmap(memory, 128 * kMiB);
        if (round == kWarm - 1) warm = rss();
    }
    verdict("private_file", warm, rss());
    close(fd);
    unlink(path);
}

// Memory advised away is not touched again: a second block takes its place. Android's resident size ends at one block;
// where the advice only hints (Darwin's), it ends at both.
static void advise_then_other(const char *name, int advice) {
    char *first = mmap(NULL, 128 * kMiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *second = mmap(NULL, 128 * kMiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (first == MAP_FAILED || second == MAP_FAILED) {
        printf("%s: mmap failed\n", name);
        return;
    }
    const long base = rss();
    touch(first, 128 * kMiB);
    madvise(first, 128 * kMiB, advice);
    touch(second, 128 * kMiB);
    const long held = rss() - base;
    printf("%s: second block after the first was advised away holds %s\n", name,
           held < 128 + kSlack ? "one block" : "both blocks");
    munmap(first, 128 * kMiB);
    munmap(second, 128 * kMiB);
}

// A freed block of malloc, given two seconds, as scudo's release to the OS takes.
static void free_returns(void) {
    const long base = rss();
    void *block = malloc(128 * kMiB);
    if (block == NULL) {
        printf("free_returns: malloc failed\n");
        return;
    }
    touch(block, 128 * kMiB);
    free(block);
    sleep(2);
    printf("free_returns: a freed 128 MiB block is %s\n", rss() - base < kSlack ? "returned" : "kept");
}

// With a scenario's name as the argument only that scenario runs (tools/memory-churn-footprint-check.sh reads the
// process's footprint peak after each, which is what iOS counts); "idle" runs none.
static int wanted(int count, char **names, const char *scenario) {
    return count < 2 || strcmp(names[1], scenario) == 0;
}

int main(int count, char **names) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (rss() < 0) {
        printf("no resident size\n");
        return 1;
    }
    if (wanted(count, names, "malloc_free")) malloc_free();
    if (wanted(count, names, "mmap_pieces")) mmap_pieces();
    if (wanted(count, names, "madvise_dontneed")) advise("madvise_dontneed", MADV_DONTNEED);
    if (wanted(count, names, "madvise_free")) advise("madvise_free", MADV_FREE);
    if (wanted(count, names, "dontneed_other_range")) advise_then_other("dontneed_other_range", MADV_DONTNEED);
    if (wanted(count, names, "free_other_range")) advise_then_other("free_other_range", MADV_FREE);
    if (wanted(count, names, "free_returns")) free_returns();
    if (wanted(count, names, "private_file")) private_file();
    return 0;
}
