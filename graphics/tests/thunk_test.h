/* Test 2 of ADR-0030 (thunk-test): what thunk_test.c, thunk_test.S and the generated thunk_calls.c share.
   thunk_test.S depends on the offsets, which thunk_test.c checks. */
#ifndef THUNK_TEST_H
#define THUNK_TEST_H

#include <stdint.h>

#define THUNK_HIDDEN __attribute__((visibility("hidden")))

enum { THUNK_VULKAN, THUNK_GLES, THUNK_EGL, THUNK_LIBS };
enum { THUNK_NDK, THUNK_HOST };
enum { THUNK_INT, THUNK_FLOAT, THUNK_POINTER, THUNK_ALLOCATOR };
enum { THUNK_X, THUNK_V, THUNK_STACK };

/* One argument as the C headers declare it: its kind, size and signedness, and its register (the same in
   both ABIs) or its offsets in AAPCS64's stack area and in Apple's. */
struct thunk_arg {
    uint8_t kind, size, is_signed, place, index, aapcs, apple;
};

/* One stub: the function's name, the stub's address, its front lib and slot, where the declaration came
   from, and its arguments in thunk_args. */
struct thunk_call {
    const char *name;
    void (*stub)(void);
    uint8_t lib, source;
    uint16_t slot, first, count;
};

extern const struct thunk_arg thunk_args[] THUNK_HIDDEN;
extern const struct thunk_call thunk_calls[] THUNK_HIDDEN;
extern const unsigned thunk_call_count THUNK_HIDDEN;

/* What thunk_call loads before it calls a stub. */
struct thunk_in {
    uint64_t x[8];                      /*   0: x0-x7 */
    _Alignas(16) uint64_t q[8][2];      /*  64: v0-v7, the low half first */
    uint64_t stack[16];                 /* 192: the stack area, AAPCS64's slots from its start */
    uint64_t garbage_x[10];             /* 320: x8-x17 */
    _Alignas(16) uint64_t garbage_q[16][2]; /* 400: v16-v31 */
    uint64_t saved_x[11];               /* 656: x19-x29 */
    uint64_t saved_d[8];                /* 744: d8-d15 */
    void (*stub)(void);                 /* 808 */
};

/* What thunk_call stores after the stub returned. */
struct thunk_out {
    uint64_t x[2];                      /*   0: x0 and x1 */
    _Alignas(16) uint64_t q[4][2];      /*  16: v0-v3 */
    uint64_t saved_x[11];               /*  80: x19-x29 */
    uint64_t saved_d[8];                /* 168: d8-d15 */
    int64_t sp_moved;                   /* 232: the stack pointer after the call minus the one before */
};

/* What thunk_record found on its entry. */
struct thunk_record {
    uint64_t x[16];                     /*   0: x0-x15 */
    _Alignas(16) uint64_t q[32][2];     /* 128: v0-v31 */
    uint64_t stack[16];                 /* 640: the 128 bytes from its stack pointer on */
    uint64_t calls;                     /* 768 */
};

void thunk_call(const struct thunk_in *in, struct thunk_out *out) THUNK_HIDDEN;
void thunk_entry(void) THUNK_HIDDEN;
void thunk_record(void) THUNK_HIDDEN;
void thunk_wrong(void) THUNK_HIDDEN;

extern struct thunk_record thunk_recorded THUNK_HIDDEN;
extern uint64_t thunk_results[10] THUNK_HIDDEN;     /* what thunk_record returns: x0, x1, then v0-v3 */
extern uint64_t thunk_entered THUNK_HIDDEN;         /* set by thunk_entry */
extern uint64_t thunk_wrong_calls THUNK_HIDDEN;     /* counted by thunk_wrong */

#endif
