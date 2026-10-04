/* Test 5 of ADR-0029 (graphics.gles_thunk): the GLES stubs that extend small arguments or repack stack
   arguments, as the built front libs hand them out: through libGLESv2's, libGLESv3's and libGLESv1_CM's
   (ADR-0041 a1) exports, and through eglGetProcAddress for those no library exports. Each is called twice by
   test 2's caller (thunk_call), with garbage in the unused bits of its arguments and of their stack slots,
   once with the sign bit of every small argument set and once clear. The runtime binds the tables to
   fake-angle (fake_angle.py), whose functions record the argument registers and the stack area as they
   arrive behind the runtime's entry and return the record in x0 and its results in x1 and v0-v3.

   Everything must arrive as Apple's ABI expects: arguments under 32 bits extended by their type, stack
   arguments at Apple's places, the rest bit for bit; the results must come back, the callee-saved registers
   and the stack pointer be kept. Registers that carry no argument are the entry's to use. */
#include <EGL/egl.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "thunk_test.h"

struct gles_thunk {
    const char *name;
    unsigned libraries; /* 1 libGLESv2, 2 libGLESv3, 4 libGLESv1_CM */
    unsigned repack;    /* 0: its stub extends small arguments */
    unsigned first, count;
};
extern const struct gles_thunk gles_thunks[] THUNK_HIDDEN;
extern const unsigned gles_thunk_count THUNK_HIDDEN;

/* fake-angle's record (fake_angle.py). */
struct fake_record {
    uint64_t x[8];
    uint64_t q[8][2];
    uint64_t stack[16];
    uint64_t x1;
    uint64_t unused;
    uint64_t results[4][2];
    uint64_t calls;
    uint64_t magic;
};
_Static_assert(offsetof(struct fake_record, stack) == 192 && offsetof(struct fake_record, x1) == 320 &&
               offsetof(struct fake_record, results) == 336 && offsetof(struct fake_record, calls) == 400 &&
               offsetof(struct fake_record, magic) == 408, "fake_record as fake_angle.py writes it");
#define MAGIC 0x6d696e61746f4641ull

static uint64_t seed = 0x2545f4914f6cdd1du;

static uint64_t garbage(void) {
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return seed * 0x9e3779b97f4a7c15u;
}

static unsigned failures, calls;
static struct fake_record *record;

static void fail(const char *name, const char *how, const char *what, uint64_t got, uint64_t expected) {
    if (++failures <= 40) {
        printf("%s through %s: %s is 0x%016llx, expected 0x%016llx\n", name, how, what, (unsigned long long)got,
               (unsigned long long)expected);
    }
}

static uint32_t extended(uint64_t value, unsigned size, int is_signed) {
    const unsigned shift = 32 - 8 * size;
    const uint32_t low = (uint32_t)value << shift;
    return is_signed ? (uint32_t)((int32_t)low >> shift) : low >> shift;
}

static void call_once(const struct gles_thunk *thunk, void (*function)(void), const char *how, int sign_bit) {
    struct thunk_in in;
    struct thunk_out out;
    uint64_t *words = (uint64_t *)&in;
    for (size_t index = 0; index < offsetof(struct thunk_in, stub) / sizeof(uint64_t); index++) {
        words[index] = garbage();
    }
    const struct thunk_arg *args = &thunk_args[thunk->first];
    for (unsigned n = 0; n < thunk->count; n++) {
        uint64_t *word = args[n].place == THUNK_X ? &in.x[args[n].index]
                         : args[n].place == THUNK_V ? &in.q[args[n].index][0] : &in.stack[args[n].aapcs / 8];
        if (args[n].kind == THUNK_INT && args[n].size < 4) {
            const uint64_t top = 1ull << (8 * args[n].size - 1);
            *word = sign_bit ? *word | top : *word & ~top;
        }
    }
    in.stub = function;
    uint64_t calls_before = 0;
    if (record != NULL) {
        record->x1 = garbage();
        for (unsigned index = 0; index < 8; index++) {
            record->results[index / 2][index % 2] = garbage();
        }
        calls_before = record->calls;
    }
    thunk_call(&in, &out);
    calls++;
    if (record == NULL) {
        record = (struct fake_record *)(uintptr_t)out.x[0];
        if (record == NULL || record->magic != MAGIC) {
            fail(thunk->name, how, "x0, fake-angle's record,", out.x[0], 0);
            record = NULL;
        }
        return;
    }
    if (out.x[0] != (uint64_t)(uintptr_t)record || record->calls != calls_before + 1) {
        fail(thunk->name, how, "the record and its calls", out.x[0] ^ (record->calls - calls_before), 1);
        return;
    }
    char what[64];
    for (unsigned n = 0; n < thunk->count; n++) {
        const struct thunk_arg *arg = &args[n];
        snprintf(what, sizeof what, "argument %u", n + 1);
        if (arg->place == THUNK_X) {
            const uint64_t sent = in.x[arg->index], got = record->x[arg->index];
            if (arg->kind == THUNK_INT && arg->size < 4) {
                if ((uint32_t)got != extended(sent, arg->size, arg->is_signed)) {
                    fail(thunk->name, how, what, (uint32_t)got, extended(sent, arg->size, arg->is_signed));
                }
            } else if (got != sent) {
                fail(thunk->name, how, what, got, sent);
            }
        } else if (arg->place == THUNK_V) {
            for (unsigned half = 0; half < 2; half++) {
                if (record->q[arg->index][half] != in.q[arg->index][half]) {
                    fail(thunk->name, how, what, record->q[arg->index][half], in.q[arg->index][half]);
                }
            }
        } else {
            uint64_t expected = in.stack[arg->aapcs / 8], got = 0;
            memcpy(&got, (const unsigned char *)record->stack + arg->apple, arg->size);
            if (arg->size < 8) {
                expected &= (1ull << (8 * arg->size)) - 1;
            }
            if (got != expected) {
                fail(thunk->name, how, what, got, expected);
            }
        }
    }
    if (out.x[1] != record->x1) {
        fail(thunk->name, how, "x1 after the call", out.x[1], record->x1);
    }
    for (unsigned index = 0; index < 8; index++) {
        if (out.q[index / 2][index % 2] != record->results[index / 2][index % 2]) {
            fail(thunk->name, how, "v0-v3 after the call", out.q[index / 2][index % 2],
                 record->results[index / 2][index % 2]);
        }
    }
    for (unsigned index = 0; index < 11; index++) {
        if (out.saved_x[index] != in.saved_x[index]) {
            snprintf(what, sizeof what, "x%u after the call", 19 + index);
            fail(thunk->name, how, what, out.saved_x[index], in.saved_x[index]);
        }
    }
    for (unsigned index = 0; index < 8; index++) {
        if (out.saved_d[index] != in.saved_d[index]) {
            snprintf(what, sizeof what, "d%u after the call", 8 + index);
            fail(thunk->name, how, what, out.saved_d[index], in.saved_d[index]);
        }
    }
    if (out.sp_moved != 0) {
        fail(thunk->name, how, "the stack pointer's move", (uint64_t)out.sp_moved, 0);
    }
}

static void call(const struct gles_thunk *thunk, void (*function)(void), const char *how) {
    if (function == NULL) {
        fail(thunk->name, how, "the function", 0, 1);
        return;
    }
    for (int sign_bit = 1; sign_bit >= 0; sign_bit--) {
        call_once(thunk, function, how, sign_bit);
    }
}

int main(void) {
    void *libraries[3] = {dlopen("libGLESv2.so", RTLD_NOW), dlopen("libGLESv3.so", RTLD_NOW),
                          dlopen("libGLESv1_CM.so", RTLD_NOW)};
    const char *const names[3] = {"libGLESv2", "libGLESv3", "libGLESv1_CM"};
    unsigned exported[3] = {0, 0, 0}, handed_out = 0, repacks = 0;
    /* The first call finds fake-angle's record. */
    call_once(&gles_thunks[0], (void (*)(void))eglGetProcAddress(gles_thunks[0].name), "a first call", 1);
    if (record == NULL) {
        printf("no record from fake-angle\n");
        return 1;
    }
    calls = 0;
    for (unsigned index = 0; index < gles_thunk_count; index++) {
        const struct gles_thunk *thunk = &gles_thunks[index];
        repacks += thunk->repack;
        for (unsigned library = 0; library < 3; library++) {
            if (thunk->libraries & (1u << library)) {
                exported[library]++;
                call(thunk, (void (*)(void))dlsym(libraries[library], thunk->name), names[library]);
            }
        }
        if (thunk->libraries == 0) {
            handed_out++;
            call(thunk, (void (*)(void))eglGetProcAddress(thunk->name), "eglGetProcAddress");
        }
    }
    printf("%u thunks of GLES: %u extend small arguments, %u repack stack arguments\n", gles_thunk_count,
           gles_thunk_count - repacks, repacks);
    for (unsigned library = 0; library < 3; library++) {
        printf("%s: %u of them, called through its exports\n", names[library], exported[library]);
    }
    printf("eglGetProcAddress: %u of them that no library exports\n", handed_out);
    if (failures) {
        printf("%u differences in %u calls\n", failures, calls);
        return 1;
    }
    printf("all %u calls arrived as Apple's ABI expects and returned fake-angle's results\n", calls);
    return 0;
}
