/* Test 2 of ADR-0030 (thunk-test, R7): calls every stub that tools/frontgen writes for the front libs.

   Each call puts the arguments where AAPCS64 puts them, with garbage in their unused bits and in the unused
   bytes of their stack slots, and garbage in every other argument register, in x8-x17 and in v16-v31
   (thunk_call, thunk_test.S). The table's slot 0 holds a stand-in for the runtime's entry, a plain tail
   call to x16 as the entry is on a host thread (decision 1), until the runtime binds the tables itself
   (syscall 4101, test 4). The stub's own slot holds thunk_record, which records what arrived and returns
   known values; every other slot counts a wrong call.

   Everything must arrive as Apple's ABI expects: arguments under 32 bits extended by their type (in the
   low 32 bits), stack arguments at Apple's places, allocators NULL, the rest bit for bit. A stub uses no
   register besides x16, x17 and the arguments it changes (decision 2), so every other one of x0-x15 and
   v0-v31 must arrive as the caller left it. The results in x0, x1 and v0-v3 must come back, the
   callee-saved registers and the stack pointer be kept. Each stub is called twice, once with the sign bit
   of every small argument set and once with it clear.

   The calls come from thunk_calls.c, which graphics/tests/thunk_calls.py generates from the C headers,
   not from the XML the stubs are made of. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "thunk_test.h"
#include "egl_table.h"
#include "gles_table.h"
#include "vulkan_table.h"

_Static_assert(offsetof(struct thunk_in, q) == 64 && offsetof(struct thunk_in, stack) == 192 &&
               offsetof(struct thunk_in, garbage_x) == 320 && offsetof(struct thunk_in, garbage_q) == 400 &&
               offsetof(struct thunk_in, saved_x) == 656 && offsetof(struct thunk_in, saved_d) == 744 &&
               offsetof(struct thunk_in, stub) == 808, "thunk_in as thunk_test.S reads it");
_Static_assert(offsetof(struct thunk_out, q) == 16 && offsetof(struct thunk_out, saved_x) == 80 &&
               offsetof(struct thunk_out, saved_d) == 168 && offsetof(struct thunk_out, sp_moved) == 232,
               "thunk_out as thunk_test.S writes it");
_Static_assert(offsetof(struct thunk_record, q) == 128 && offsetof(struct thunk_record, stack) == 640 &&
               offsetof(struct thunk_record, calls) == 768 && sizeof(struct thunk_record) == 784,
               "thunk_record as thunk_test.S writes it");

extern void *minato_vulkan_table[MINATO_VULKAN_TABLE_LENGTH] THUNK_HIDDEN;
extern void *minato_gles_table[MINATO_GLES_TABLE_LENGTH] THUNK_HIDDEN;
extern void *minato_egl_table[MINATO_EGL_TABLE_LENGTH] THUNK_HIDDEN;

static const struct {
    const char *name, *host;
    void **table;
    unsigned length;
} libs[THUNK_LIBS] = {
    [THUNK_VULKAN] = {"vulkan", "MoltenVK's", minato_vulkan_table, MINATO_VULKAN_TABLE_LENGTH},
    [THUNK_GLES] = {"gles", "ANGLE's", minato_gles_table, MINATO_GLES_TABLE_LENGTH},
    [THUNK_EGL] = {"egl", "ANGLE's", minato_egl_table, MINATO_EGL_TABLE_LENGTH},
};

static uint64_t seed = 0x9e3779b97f4a7c15u;

/* xorshift64*: garbage that is the same in every run. */
static uint64_t garbage(void) {
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return seed * 0x2545f4914f6cdd1du;
}

static unsigned failures;

static void fail(const struct thunk_call *call, const char *what, uint64_t got, uint64_t expected) {
    if (++failures <= 40) {
        printf("%s: %s is 0x%016llx, expected 0x%016llx\n", call->name, what, (unsigned long long)got,
               (unsigned long long)expected);
    }
}

/* A value of `size` bytes as Apple's callee expects it in a register: extended to 32 bits by its type. */
static uint32_t extended(uint64_t value, unsigned size, int is_signed) {
    const unsigned shift = 32 - 8 * size;
    const uint32_t low = (uint32_t)value << shift;
    return is_signed ? (uint32_t)((int32_t)low >> shift) : low >> shift;
}

static void call_once(const struct thunk_call *call, int sign_bit) {
    struct thunk_in in;
    struct thunk_out out;
    uint64_t *words = (uint64_t *)&in;
    for (size_t index = 0; index < offsetof(struct thunk_in, stub) / sizeof(uint64_t); index++) {
        words[index] = garbage();
    }
    const struct thunk_arg *args = &thunk_args[call->first];
    for (unsigned n = 0; n < call->count; n++) {
        const struct thunk_arg *arg = &args[n];
        uint64_t *word = arg->place == THUNK_X ? &in.x[arg->index]
                         : arg->place == THUNK_V ? &in.q[arg->index][0] : &in.stack[arg->aapcs / 8];
        if (arg->kind == THUNK_INT && arg->size < 4) {
            const uint64_t top = 1ull << (8 * arg->size - 1);
            *word = sign_bit ? *word | top : *word & ~top;
        }
    }
    in.stub = call->stub;
    for (unsigned index = 0; index < 10; index++) {
        thunk_results[index] = garbage();
    }
    void **table = libs[call->lib].table;
    table[call->slot] = (void *)thunk_record;
    memset(&thunk_recorded, 0, sizeof thunk_recorded);
    thunk_entered = 0;
    thunk_wrong_calls = 0;
    thunk_call(&in, &out);
    table[call->slot] = (void *)thunk_wrong;

    if (!thunk_entered) {
        fail(call, "the entry's use", 0, 1);
    }
    if (thunk_wrong_calls != 0 || thunk_recorded.calls != 1) {
        fail(call, "the calls of its own slot and of others", thunk_recorded.calls << 32 | thunk_wrong_calls,
             1ull << 32);
        return;
    }
    char what[64];
    uint32_t x_arguments = 0, v_arguments = 0;
    for (unsigned n = 0; n < call->count; n++) {
        const struct thunk_arg *arg = &args[n];
        snprintf(what, sizeof what, "argument %u", n + 1);
        x_arguments |= arg->place == THUNK_X ? 1u << arg->index : 0;
        v_arguments |= arg->place == THUNK_V ? 1u << arg->index : 0;
        if (arg->place == THUNK_X) {
            const uint64_t sent = in.x[arg->index], got = thunk_recorded.x[arg->index];
            if (arg->kind == THUNK_ALLOCATOR) {
                if (got != 0) {
                    fail(call, what, got, 0);
                }
            } else if (arg->kind == THUNK_INT && arg->size < 4) {
                if ((uint32_t)got != extended(sent, arg->size, arg->is_signed)) {
                    fail(call, what, (uint32_t)got, extended(sent, arg->size, arg->is_signed));
                }
            } else if (got != sent) {
                fail(call, what, got, sent);
            }
        } else if (arg->place == THUNK_V) {
            for (unsigned half = 0; half < 2; half++) {
                if (thunk_recorded.q[arg->index][half] != in.q[arg->index][half]) {
                    fail(call, what, thunk_recorded.q[arg->index][half], in.q[arg->index][half]);
                }
            }
        } else {
            uint64_t expected = arg->kind == THUNK_ALLOCATOR ? 0 : in.stack[arg->aapcs / 8], got = 0;
            memcpy(&got, (const unsigned char *)thunk_recorded.stack + arg->apple, arg->size);
            if (arg->size < 8) {
                expected &= (1ull << (8 * arg->size)) - 1;
            }
            if (got != expected) {
                fail(call, what, got, expected);
            }
        }
    }
    for (unsigned index = 0; index < 16; index++) {
        const uint64_t sent = index < 8 ? in.x[index] : in.garbage_x[index - 8];
        if (!(x_arguments >> index & 1) && thunk_recorded.x[index] != sent) {
            snprintf(what, sizeof what, "x%u, which carries no argument,", index);
            fail(call, what, thunk_recorded.x[index], sent);
        }
    }
    for (unsigned index = 0; index < 32; index++) {
        /* thunk_call loads d8-d15 as 64-bit values, which clears the upper halves of v8-v15. */
        const uint64_t *q = index < 8 ? in.q[index] : index >= 16 ? in.garbage_q[index - 16] : NULL;
        const uint64_t sent[2] = {q ? q[0] : in.saved_d[index - 8], q ? q[1] : 0};
        for (unsigned half = 0; half < 2 && !(v_arguments >> index & 1); half++) {
            if (thunk_recorded.q[index][half] != sent[half]) {
                snprintf(what, sizeof what, "v%u, which carries no argument,", index);
                fail(call, what, thunk_recorded.q[index][half], sent[half]);
            }
        }
    }
    for (unsigned index = 0; index < 2; index++) {
        if (out.x[index] != thunk_results[index]) {
            fail(call, index ? "x1 after the call" : "x0 after the call", out.x[index], thunk_results[index]);
        }
    }
    for (unsigned index = 0; index < 8; index++) {
        if (out.q[index / 2][index % 2] != thunk_results[2 + index]) {
            fail(call, "v0-v3 after the call", out.q[index / 2][index % 2], thunk_results[2 + index]);
        }
    }
    for (unsigned index = 0; index < 11; index++) {
        if (out.saved_x[index] != in.saved_x[index]) {
            snprintf(what, sizeof what, "x%u after the call", 19 + index);
            fail(call, what, out.saved_x[index], in.saved_x[index]);
        }
    }
    for (unsigned index = 0; index < 8; index++) {
        if (out.saved_d[index] != in.saved_d[index]) {
            snprintf(what, sizeof what, "d%u after the call", 8 + index);
            fail(call, what, out.saved_d[index], in.saved_d[index]);
        }
    }
    if (out.sp_moved != 0) {
        fail(call, "the stack pointer's move", (uint64_t)out.sp_moved, 0);
    }
}

int main(void) {
    static unsigned char called[THUNK_LIBS][MINATO_GLES_TABLE_LENGTH];
    _Static_assert(MINATO_GLES_TABLE_LENGTH >= MINATO_VULKAN_TABLE_LENGTH &&
                   MINATO_GLES_TABLE_LENGTH >= MINATO_EGL_TABLE_LENGTH, "the largest table sizes called[]");
    unsigned from_host[THUNK_LIBS] = {0}, calls = 0;
    for (unsigned lib = 0; lib < THUNK_LIBS; lib++) {
        libs[lib].table[0] = (void *)thunk_entry;
        for (unsigned slot = 1; slot < libs[lib].length; slot++) {
            libs[lib].table[slot] = (void *)thunk_wrong;
        }
    }
    for (unsigned index = 0; index < thunk_call_count; index++) {
        const struct thunk_call *call = &thunk_calls[index];
        called[call->lib][call->slot]++;
        from_host[call->lib] += call->source == THUNK_HOST;
        for (int sign_bit = 1; sign_bit >= 0; sign_bit--) {
            call_once(call, sign_bit);
            calls++;
        }
    }
    for (unsigned lib = 0; lib < THUNK_LIBS; lib++) {
        unsigned stubs = 0;
        for (unsigned slot = 1; slot < libs[lib].length; slot++) {
            if (called[lib][slot] != 1) {
                printf("%s: slot %u has %u stubs, expected 1\n", libs[lib].name, slot, called[lib][slot]);
                failures++;
            }
            stubs += called[lib][slot];
        }
        printf("%s: %u stubs, %u declared in the NDK's headers and %u in %s\n", libs[lib].name, stubs,
               stubs - from_host[lib], from_host[lib], libs[lib].host);
    }
    if (failures) {
        printf("%u differences in %u calls\n", failures, calls);
        return 1;
    }
    printf("all %u calls arrived as Apple's ABI expects and returned the host function's results\n", calls);
    return 0;
}
