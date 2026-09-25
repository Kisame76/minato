// island-bench: what one pass through an island of the converter costs (ADR-0010, ADR-0020 X6).
// Each form runs in a loop from island-bench.S, once as the converter puts it into an island and
// once as a twin that it leaves alone; on Android neither is rewritten, so both take the same time
// there. A line gives the time per pass of the twin and of the form, each the fastest of 7 runs,
// and the difference in nanoseconds and in cycles; a chain of dependent ADDs gives the length of a
// cycle. Islands one after the other are the worst case: in real code the core overlaps an island
// with the work around it.
// The times differ from run to run, so fixtures/tests.txt marks the program `measures`. It checks
// that each form computes what its twin computes and otherwise exits with status 1.
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef uint64_t (*loop)(uint64_t rounds, uint64_t *cell);

uint64_t bench_cycles(uint64_t rounds, uint64_t *cell);
uint64_t bench_read_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_read_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_read_among_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_read_among_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_write_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_write_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_read_write_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_read_write_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_adrp_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_adrp_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_cbz_taken_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_cbz_taken_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_cbnz_not_taken_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_cbnz_not_taken_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_exclusive_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_exclusive_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_jump_x18(uint64_t rounds, uint64_t *cell);
uint64_t bench_jump_x17(uint64_t rounds, uint64_t *cell);
uint64_t bench_thread_pointer(uint64_t rounds, uint64_t *cell);
uint64_t bench_tpidrro(uint64_t rounds, uint64_t *cell);

// A function that clang guards with a stack protector: it loads the guard through the thread
// pointer at its start and compares it before it returns. It reads the thread pointer once and keeps
// it in a register; a function that makes a call in between reads it again. -fstack-protector-strong,
// the NDK's default, guards a function whose array's address escapes, as it does into a call in most
// code; an array that is only used within its bounds is not guarded. Its twin is the same function
// without the guard.
__attribute__((noinline)) static uint64_t guarded(uint64_t value) {
    uint8_t bytes[8];
    bytes[0] = (uint8_t)value;
    __asm__ volatile("" : : "r"(bytes) : "memory");
    return bytes[0] + 1u;
}

__attribute__((noinline, no_stack_protector)) static uint64_t unguarded(uint64_t value) {
    uint8_t bytes[8];
    bytes[0] = (uint8_t)value;
    __asm__ volatile("" : : "r"(bytes) : "memory");
    return bytes[0] + 1u;
}

static uint64_t call_guarded(uint64_t rounds, uint64_t *cell) {
    (void)cell;
    uint64_t sum = 0;
    for (uint64_t round = rounds; round != 0; --round) sum += guarded(round);
    return sum;
}

static uint64_t call_unguarded(uint64_t rounds, uint64_t *cell) {
    (void)cell;
    uint64_t sum = 0;
    for (uint64_t round = rounds; round != 0; --round) sum += unguarded(round);
    return sum;
}

struct form {
    const char *name;
    loop island;          // as the converter puts it into an island
    loop twin;            // what the converter leaves alone
    uint64_t copies;      // passes per round
    bool thread_pointer;  // the form returns the thread pointer, which its twin does not read
};

static const struct form forms[] = {
    {"x18 read: add x3, x3, x18", bench_read_x18, bench_read_x17, 16, false},
    {"x18 read, among 8 other ADDs", bench_read_among_x18, bench_read_among_x17, 16, false},
    {"x18 write: add x18, x2, #1", bench_write_x18, bench_write_x17, 16, false},
    {"x18 read and write: add x18, x18, #1", bench_read_write_x18, bench_read_write_x17, 16, false},
    {"x18 ADRP", bench_adrp_x18, bench_adrp_x17, 16, false},
    {"x18 CBZ, taken", bench_cbz_taken_x18, bench_cbz_taken_x17, 16, false},
    {"x18 CBNZ, not taken", bench_cbnz_not_taken_x18, bench_cbnz_not_taken_x17, 16, false},
    {"x18 LL/SC window: LDXR, ADD, STXR", bench_exclusive_x18, bench_exclusive_x17, 16, false},
    {"x18 jump table: LDRSW, ADD, BR", bench_jump_x18, bench_jump_x17, 4, false},
    {"thread pointer: mrs x3, TPIDR_EL0", bench_thread_pointer, bench_tpidrro, 16, true},
    {"call of a function with a stack protector", call_guarded, call_unguarded, 1, false},
};

enum { runs = 7 };
static const uint64_t long_enough = 2000000;  // nanoseconds per call

static uint64_t nanoseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

// The cell of the LL/SC windows, in a cache line of its own.
static _Alignas(64) uint64_t cell;

/// One call of `function` in nanoseconds; what it computed goes to `result`.
static uint64_t timed(loop function, uint64_t rounds, uint64_t *result) {
    cell = 0;
    const uint64_t start = nanoseconds();
    *result = function(rounds, &cell);
    return nanoseconds() - start;
}

/// The rounds that make one call of `function` take long enough.
static uint64_t rounds_for(loop function) {
    uint64_t rounds = 1024, result;
    while (timed(function, rounds, &result) < long_enough && rounds < UINT64_C(1) << 30) rounds *= 2;
    return rounds;
}

int main(void) {
    const uint64_t cycle_rounds = rounds_for(bench_cycles);
    uint64_t result, fastest = UINT64_MAX;
    for (int run = 0; run < runs; ++run) {
        const uint64_t time = timed(bench_cycles, cycle_rounds, &result);
        if (time < fastest) fastest = time;
    }
    const double cycle = (double)fastest / (double)(cycle_rounds * 64);
    printf("island-bench: one pass of each form and of its twin, the fastest of %d runs; a cycle is %.3f ns\n",
           runs, cycle);
    printf("%-42s %9s %9s %9s %7s\n", "form", "twin", "form", "extra", "cycles");

    int failed = 0;
    for (size_t index = 0; index < sizeof forms / sizeof forms[0]; ++index) {
        const struct form *form = &forms[index];
        const uint64_t rounds = rounds_for(form->island);
        uint64_t twin_time = UINT64_MAX, form_time = UINT64_MAX, twin_result = 0, form_result = 0;
        for (int run = 0; run < runs; ++run) {
            const uint64_t twin = timed(form->twin, rounds, &twin_result);
            if (twin < twin_time) twin_time = twin;
            const uint64_t time = timed(form->island, rounds, &form_result);
            if (time < form_time) form_time = time;
        }
        const uint64_t expected = form->thread_pointer ? (uint64_t)__builtin_thread_pointer() : twin_result;
        if (form_result != expected) {
            fprintf(stderr, "%s: the form computed 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", form->name,
                    form_result, expected);
            failed = 1;
        }
        const double passes = (double)(rounds * form->copies);
        const double twin = (double)twin_time / passes, time = (double)form_time / passes;
        printf("%-42s %6.2f ns %6.2f ns %6.2f ns %7.1f\n", form->name, twin, time, time - twin, (time - twin) / cycle);
    }
    return failed;
}
