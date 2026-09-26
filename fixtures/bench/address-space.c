// address-space: how much address space a guest program has left, and what the allocator does with it
// (ADR-0035): more than 64 MiB of blocks of one size, which spill into larger classes once a region of
// 64 MiB is full; a block of every size class; whether a window of 2 GiB at a 4 GiB boundary still fits,
// as ART's heap window will need (ADR-0033, decision 3); and the largest reservation and all reservations
// left, probed as spike S2 does (ADR-0012). A benchmark (measures in fixtures/tests.txt): what it prints
// differs from place to place, and nothing compares it. The allocator's messages during the spill go to
// Android's log on Android and to stderr here (ADR-0035); they are caught and shown on stdout, so that
// stderr stays empty everywhere.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static const uint64_t mib = 1ull << 20, gib = 1ull << 30;

/// 80 MiB of blocks of 32 bytes, 48 with the allocator's header: more than its region of 64 MiB.
static void spill(void) {
    const size_t count = (size_t)(80 * mib / 48);
    void **const pointers = mmap(NULL, count * sizeof(void *), PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pointers == MAP_FAILED) {
        printf("spill: no memory for the list of blocks\n");
        return;
    }

    // The allocator's messages go to stderr here: into a file for the time of the spill.
    const int saved = dup(STDERR_FILENO);
    const int caught = open("address-space.stderr", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (saved >= 0 && caught >= 0) dup2(caught, STDERR_FILENO);

    size_t allocated = 0, changed = 0;
    for (; allocated < count; ++allocated) {
        unsigned char *const block = malloc(32);
        if (block == NULL) break;
        for (int index = 0; index < 32; ++index) block[index] = (unsigned char)(allocated * 13 + index);
        pointers[allocated] = block;
    }
    for (size_t number = 0; number < allocated; ++number) {
        const unsigned char *const block = pointers[number];
        for (int index = 0; index < 32; ++index) {
            if (block[index] != (unsigned char)(number * 13 + index)) {
                ++changed;
                break;
            }
        }
    }
    for (size_t number = 0; number < allocated; ++number) free(pointers[number]);
    munmap(pointers, count * sizeof(void *));

    if (saved >= 0 && caught >= 0) {
        dup2(saved, STDERR_FILENO);
        close(saved);
    }
    printf("spill: %zu of %zu blocks of 32 bytes allocated, %zu changed\n", allocated, count, changed);
    if (caught < 0) return;
    char text[4096];
    const ssize_t length = pread(caught, text, sizeof text - 1, 0);
    close(caught);
    unlink("address-space.stderr");
    if (length <= 0) {
        printf("  the allocator said nothing on stderr\n");
        return;
    }
    text[length] = '\0';
    char *rest = NULL;
    for (char *line = strtok_r(text, "\n", &rest); line != NULL; line = strtok_r(NULL, "\n", &rest)) {
        printf("  stderr: %s\n", line);
    }
}

/// A block of every size class of the primary, as memory-test allocates them; they stay in use.
static int every_class(void) {
    int sizes = 0, failed = 0;
    size_t size = 16;
    for (;;) {
        unsigned char *const block = malloc(size);
        if (block == NULL) {
            ++failed;
        } else {
            memset(block, 0x5a, size);
        }
        ++sizes;
        if (size == 65536) break;
        size += size / 16 > 16 ? size / 16 : 16;
        if (size > 65536) size = 65536;
    }
    printf("every class: %d sizes of 16 to 65536 bytes, %d not allocated\n", sizes, failed);
    return failed;
}

/// Whether a window of 2 GiB fits at an address aligned to 4 GiB, as memory-test looks for it.
static int window_fits(void) {
    for (uint64_t address = 4 * gib; address < (1ull << 40); address += 4 * gib) {
        void *const wanted = (void *)(uintptr_t)address;
        void *const result = mmap(wanted, 2 * gib, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (result != MAP_FAILED) munmap(result, 2 * gib);
        if (result == wanted) return 1;
    }
    return 0;
}

/// The largest reservation without access that still succeeds, to `step` precision, below `upper`.
static uint64_t largest_reservation(uint64_t step, uint64_t upper) {
    uint64_t low = 0, high = upper;
    while (high - low > step) {
        uint64_t middle = low + (high - low) / 2;
        middle -= middle % step;
        if (middle <= low) break;
        void *const region = mmap(NULL, middle, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (region == MAP_FAILED) {
            high = middle;
        } else {
            munmap(region, middle);
            low = middle;
        }
    }
    return low;
}

/// As spike S2: the largest reservation, then again and again in what is left, down to 4 steps; it
/// stops once it has 1 TiB in all, which only a Mac or Linux reaches.
static void reservations(void) {
    enum { max_regions = 1024 };
    static void *regions[max_regions];
    static uint64_t sizes[max_regions];
    const uint64_t step = 16 * mib, upper = 1ull << 40;
    uint64_t largest = 0, total = 0;
    int count = 0;
    while (count < max_regions && total < upper) {
        const uint64_t size = largest_reservation(step, upper);
        if (size < 4 * step) break;
        void *const region = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (region == MAP_FAILED) break;
        if (count == 0) largest = size;
        regions[count] = region;
        sizes[count] = size;
        ++count;
        total += size;
    }
    for (int number = 0; number < count; ++number) munmap(regions[number], sizes[number]);
    printf("reservations: the largest %.2f GiB, all %.2f GiB in %d regions%s\n", (double)largest / gib,
           (double)total / gib, count, total >= upper ? " (stopped past 1 TiB)" : "");
}

int main(void) {
    spill();
    every_class();
    printf("a 2 GiB window at a 4 GiB boundary, with every class in use: %s\n", window_fits() ? "fits" : "does not fit");
    reservations();
    return 0;
}
