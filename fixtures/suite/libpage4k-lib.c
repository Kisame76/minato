// libpage4k-lib.so for page4k-test, linked for 4 KiB pages: its code and its writable data meet in one
// 16 KiB page, so the converter moves its whole image by a multiple of 4 KiB (ADR-0007 decision 2, as
// amended). It reaches its data the ways compiled code does: ADRP with ADD or LDR, the GOT, pointers
// relocated in RELRO, TLS descriptors and a constructor.
int page4k_counter = 7;

static int twice(int value) {
    return 2 * value;
}

static int square(int value) {
    return value * value;
}

// Relocated at load, then read-only (RELRO): function pointers and pointers into .rodata.
int (*const page4k_operations[])(int) = {twice, square};
const char *const page4k_names[] = {"alpha", "beta", "gamma"};

__thread int page4k_thread_value = 5;

static int constructed;

__attribute__((constructor)) static void construct(void) {
    constructed = 42;
}

int page4k_constructed(void) {
    return constructed;
}

int page4k_next(void) {
    return ++page4k_counter;
}

int page4k_apply(int index, int value) {
    return page4k_operations[index](value);
}

const char *page4k_name(int index) {
    return page4k_names[index];
}

int *page4k_thread_address(void) {
    return &page4k_thread_value;
}
