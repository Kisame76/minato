// libruns-lib.so for runs-test, linked with runs-lib.ld: after its data come a second code segment, a read-only one
// and a writable one, as the Unity test APK v2's libil2cpp.so has them (ADR-0008 amendment). Code and data on both sides
// reach each other the ways compiled code does: ADRP with ADD or LDR, the GOT, pointers relocated in RELRO, a
// constructor and TLS.
#define LATE_TEXT __attribute__((section(".text.after"), noinline))
#define LATE_RODATA __attribute__((section(".rodata.after")))
#define LATE_DATA __attribute__((section(".data.after")))

int runs_counter = 7;
LATE_DATA int runs_late_counter = 11;
LATE_RODATA const char runs_late_text[] = "read-only data after the data";

__thread int runs_thread_value = 5;

static int twice(int value) {
    return 2 * value;
}

LATE_TEXT static int late_square(int value) {
    return value * value;
}

// Relocated at load, then read-only (RELRO): pointers into the code and the read-only data after the data.
int (*const runs_operations[])(int) = {twice, late_square};
const char *const runs_texts[] = {"early", runs_late_text};

static int constructed;

LATE_TEXT __attribute__((constructor)) static void construct(void) {
    constructed = 42;
}

int runs_constructed(void) {
    return constructed;
}

LATE_TEXT int runs_late_next(void) {
    ++runs_counter;
    return ++runs_late_counter;
}

int runs_apply(int index, int value) {
    return runs_operations[index](value);
}

const char *runs_text(int index) {
    return runs_texts[index];
}

LATE_TEXT int *runs_thread_address(void) {
    return &runs_thread_value;
}
