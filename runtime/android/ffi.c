// libminato-ffi.so, the guest half of libffi closures without executable memory (ADR-0072): the loader preloads it when
// it is among the system libraries and resolves ffi_closure_alloc, ffi_closure_free and ffi_prep_closure_loc, by
// import and by dlsym alike, to the wrappers below wherever the library that defines them defines the other two
// (linker::Override with requires_symbol), writing the library's ffi_prep_closure_loc into minato_ffi_prep_original.
//
// libffi makes a closure's code at run time: it writes a trampoline into memory it made executable, and a refusal
// of that mmap (what iOS gives an app without the JIT entitlement, fixed decision 2) makes ffi_closure_alloc return
// NULL, so that ctypes cannot create a callback. Here the code pointer of a closure is one of kSlots trampolines of
// this library, written when the library is built and signed with it:
//   bti jc; adrp x16, table_i; ldr x17, [x16, #lo12]; ldr x16, [x16, #lo12+8]; br x16
// which loads {closure, target} of its slot from minato_ffi_table (data, writable) and branches to the target.
// Nothing is generated at run time; only the two table words change. x17 is the writable block, which is what
// ffi_closure_SYSV needs: it reads cif, fun and user_data at [x17, #0x18..] and never the code address (libffi 3.4,
// sysv.S); x16 and x17 are the intra-procedure scratch registers libffi's own trampoline uses too.
//   ffi_closure_alloc(size, &code): a plain writable block (the closure) behind a 16-byte header that holds the slot;
//     *code is the slot's trampoline. NULL when all kSlots are in use (one log line).
//   ffi_prep_closure_loc(closure, cif, fun, user_data, codeloc): when codeloc is one of the trampolines, the library's
//     own function runs on the block (it does the ABI work: it fills the block and chooses the target by the cif's
//     flags), then the target it stored at +0x10 goes into the slot's table entry. Any other codeloc is left to it.
//   ffi_closure_free(closure): the slot goes back to the free list, the block is freed.
// A libffi that does not export the three names is not covered (no name to resolve); neither is ffi_prep_closure
// with memory the caller made executable.
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOTS 4096
#define STRIDE 32  // bytes of one trampoline
#define TARGET_OFFSET 0x10
#define BLOCK_MINIMUM 0x30
#define HEADER 16

typedef int (*PrepClosureLoc)(void *closure, void *cif, void *fun, void *user_data, void *codeloc);

PrepClosureLoc minato_ffi_prep_original;
// The original of the other two names is never called: a closure of ours is not the library's to free.
void *minato_ffi_alloc_original;
void *minato_ffi_free_original;

struct Slot {
    void *closure;
    void *target;
};
__attribute__((visibility("hidden"))) struct Slot minato_ffi_table[SLOTS];

// The trampolines: SLOTS of STRIDE bytes, each addressing its own table entry.
__asm__(
    ".text\n"
    ".balign 32\n"
    ".globl minato_ffi_trampolines\n"
    ".hidden minato_ffi_trampolines\n"
    ".type minato_ffi_trampolines,%function\n"
    "minato_ffi_trampolines:\n"
    ".set slot, 0\n"
    ".rept " "4096" "\n"
    "  bti jc\n"
    "  adrp x16, minato_ffi_table + slot * 16\n"
    "  ldr x17, [x16, :lo12:(minato_ffi_table + slot * 16)]\n"
    "  ldr x16, [x16, :lo12:(minato_ffi_table + slot * 16 + 8)]\n"
    "  br x16\n"
    "  nop\n"
    "  nop\n"
    "  nop\n"
    ".set slot, slot + 1\n"
    ".endr\n");
extern char minato_ffi_trampolines[];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t free_slots[SLOTS];
static int free_count = -1;  // -1 until the first use
static int warned;

static int take_slot(void) {
    int slot = -1;
    pthread_mutex_lock(&lock);
    if (free_count < 0) {
        for (int index = 0; index < SLOTS; index++) free_slots[index] = (uint16_t)(SLOTS - 1 - index);
        free_count = SLOTS;
    }
    if (free_count > 0) slot = free_slots[--free_count];
    int first_warning = slot < 0 && !warned;
    if (first_warning) warned = 1;
    pthread_mutex_unlock(&lock);
    if (first_warning) {
        // A line in liblog's format on stderr, without liblog: a library the preload needs is not in the global group.
        static const char line[] = "W/minato-ffi: all libffi closure trampolines are in use\n";
        (void)!write(STDERR_FILENO, line, sizeof line - 1);
    }
    return slot;
}

static void give_slot(int slot) {
    pthread_mutex_lock(&lock);
    free_slots[free_count++] = (uint16_t)slot;
    pthread_mutex_unlock(&lock);
}

void *minato_ffi_closure_alloc(size_t size, void **code) {
    if (code == NULL) return NULL;
    const int slot = take_slot();
    if (slot < 0) return NULL;
    if (size < BLOCK_MINIMUM) size = BLOCK_MINIMUM;
    char *block = calloc(1, HEADER + size);
    if (block == NULL) {
        give_slot(slot);
        return NULL;
    }
    *(int64_t *)block = slot;
    *code = minato_ffi_trampolines + (size_t)slot * STRIDE;
    return block + HEADER;
}

int minato_ffi_prep_closure_loc(void *closure, void *cif, void *fun, void *user_data, void *codeloc) {
    const char *code = codeloc;
    const size_t offset = (size_t)(code - minato_ffi_trampolines);
    if (code < minato_ffi_trampolines || offset >= (size_t)SLOTS * STRIDE || offset % STRIDE != 0) {
        return minato_ffi_prep_original(closure, cif, fun, user_data, codeloc);
    }
    const int status = minato_ffi_prep_original(closure, cif, fun, user_data, codeloc);
    if (status != 0) return status;
    struct Slot *slot = &minato_ffi_table[offset / STRIDE];
    __atomic_store_n(&slot->target, *(void **)((char *)closure + TARGET_OFFSET), __ATOMIC_RELEASE);
    __atomic_store_n(&slot->closure, closure, __ATOMIC_RELEASE);
    return 0;
}

void minato_ffi_closure_free(void *closure) {
    if (closure == NULL) return;
    char *block = (char *)closure - HEADER;
    const int slot = (int)*(int64_t *)block;
    __atomic_store_n(&minato_ffi_table[slot].closure, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&minato_ffi_table[slot].target, NULL, __ATOMIC_RELEASE);
    give_slot(slot);
    free(block);
}
