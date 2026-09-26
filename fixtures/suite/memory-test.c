// memory-test: the allocator and the address space as apps and ART need them (ADR-0035): a block of every
// size class of the allocator's primary and a few blocks of its secondary, each written and checked;
// then, while all of them are in use, a window of 2 GiB without access at an address aligned to 4 GiB, as
// ART's heap window will be (ADR-0033, decision 3); then RLIMIT_STACK, which bionic's
// pthread_getattr_np and ART ask for. Expected output: fixtures/expected/memory-test.out
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

struct block {
    unsigned char *address;
    size_t size;
};

enum { max_blocks = 256 };
static struct block blocks[max_blocks];
static int block_count;

/// A byte that depends on the block's size and on the place in it, so that blocks that overlap show.
static unsigned char pattern(size_t size, size_t index) {
    return (unsigned char)(size * 31 + index * 7 + (index >> 8));
}

static int allocate(size_t size) {
    if (block_count == max_blocks) return 0;
    unsigned char *const address = malloc(size);
    if (address == NULL) return 0;
    for (size_t index = 0; index < size; ++index) address[index] = pattern(size, index);
    blocks[block_count++] = (struct block){address, size};
    return 1;
}

/// The number of blocks from `first` on whose bytes changed since they were written.
static int changed(int first) {
    int count = 0;
    for (int number = first; number < block_count; ++number) {
        const struct block *const block = &blocks[number];
        for (size_t index = 0; index < block->size; ++index) {
            if (block->address[index] != pattern(block->size, index)) {
                ++count;
                break;
            }
        }
    }
    return count;
}

/// Every size class of the primary: from 16 bytes on in steps of 16, or of a sixteenth once that is more,
/// which is finer than any two neighboring classes, up to 64 KiB, the largest block the primary holds.
static void small_blocks(void) {
    int sizes = 0, failed = 0;
    size_t size = 16;
    for (;;) {
        failed += !allocate(size);
        ++sizes;
        if (size == 65536) break;
        size += size / 16 > 16 ? size / 16 : 16;
        if (size > 65536) size = 65536;
    }
    printf("blocks of 16 to 65536 bytes, %d sizes: %d not allocated, %d changed\n", sizes, failed, changed(0));
}

/// Blocks for the secondary, which maps each one by itself.
static void large_blocks(void) {
    static const size_t sizes[] = {65537, 256 << 10, 1 << 20, 16 << 20};
    const int first = block_count;
    int failed = 0;
    for (size_t number = 0; number < sizeof sizes / sizeof sizes[0]; ++number) failed += !allocate(sizes[number]);
    printf("blocks of 65537 bytes to 16 MiB, %zu sizes: %d not allocated, %d changed\n",
           sizeof sizes / sizeof sizes[0], failed, changed(first));
}

/// A window of 2 GiB without access at the first address aligned to 4 GiB where nothing lies yet. A kernel
/// without MAP_FIXED_NOREPLACE takes the address as a hint and may map elsewhere; that counts as taken.
static void *window(void) {
    const uint64_t four_gib = 1ull << 32, two_gib = 1ull << 31;
    for (uint64_t address = four_gib; address < (1ull << 40); address += four_gib) {
        void *const wanted = (void *)(uintptr_t)address;
        void *const result = mmap(wanted, two_gib, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (result == wanted) return result;
        if (result != MAP_FAILED) munmap(result, two_gib);
    }
    return NULL;
}

static void print_limit(const char *name, rlim_t value) {
    if (value == RLIM_INFINITY) {
        printf("%s unlimited", name);
    } else {
        printf("%s %llu", name, (unsigned long long)value);
    }
}

int main(void) {
    small_blocks();
    large_blocks();

    void *const reserved = window();
    printf("a 2 GiB window without access at a 4 GiB boundary, while the blocks are in use: %s\n",
           reserved != NULL ? "yes" : "no");
    printf("blocks changed by the window: %d\n", changed(0));
    if (reserved != NULL) munmap(reserved, 1ull << 31);

    for (int number = 0; number < block_count; ++number) free(blocks[number].address);
    printf("%d blocks freed\n", block_count);

    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) != 0) {
        printf("RLIMIT_STACK: -1 errno %d (%s)\n", errno, strerror(errno));
    } else {
        printf("RLIMIT_STACK: ");
        print_limit("soft", limit.rlim_cur);
        print_limit(", hard", limit.rlim_max);
        printf("\n");
    }
    return 0;
}
