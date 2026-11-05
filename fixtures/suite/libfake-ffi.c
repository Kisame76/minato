// libfake-ffi.so and libfake-ffi-sym.so: stand in for a statically linked libffi (libffi closures without executable
// memory, ADR-0072) by its exported names and by the layout of its legacy closure path on arm64:
//   ffi_closure_alloc asks for RWX memory with mmap, as libffi's dlmmap does (Android grants it, Minato refuses it
//   with EACCES, so the call returns NULL there);
//   ffi_prep_closure_loc writes the 16-byte trampoline `ldr x16, [pc,#16]; adr x17, .; br x16` at the start of the
//   block, the target at +0x10 and cif, fun and user_data at +0x18, +0x20 and +0x28, as libffi 3.4 does;
//   the target reads them through x17, as ffi_closure_SYSV does, and calls fun(cif, &ret, &arg, user_data).
// The argument and the return value are one long each. libfake-ffi-sym.so is the same code linked with -Bsymbolic, and
// fake_ffi_make / fake_ffi_release call the three names from inside the library by direct branches with no relocation
// (the shape of Ren'Py's librenpython.so, where _ctypes calls the statically linked libffi): a hook by relocation
// cannot see those calls.
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

void *ffi_closure_alloc(size_t size, void **code);
void ffi_closure_free(void *closure);
int ffi_prep_closure_loc(void *closure, void *cif, void *fun, void *user_data, void *codeloc);

void fake_ffi_fun(void *cif, long *ret, long *arg, void *user_data) {
    (void)cif;
    *ret = *arg + (long)user_data;
}

__asm__(
    ".text\n"
    ".balign 16\n"
    ".type fake_ffi_entry,%function\n"
    "fake_ffi_entry:\n"
    "  stp x29, x30, [sp, #-32]!\n"
    "  mov x29, sp\n"
    "  str x0, [sp, #16]\n"
    "  ldr x0, [x17, #0x18]\n"  // cif
    "  add x1, sp, #24\n"       // &ret
    "  add x2, sp, #16\n"       // &arg
    "  ldr x3, [x17, #0x28]\n"  // user_data
    "  ldr x4, [x17, #0x20]\n"  // fun
    "  blr x4\n"
    "  ldr x0, [sp, #24]\n"
    "  ldp x29, x30, [sp], #32\n"
    "  ret\n");
extern char fake_ffi_entry[];

__attribute__((noinline)) void *ffi_closure_alloc(size_t size, void **code) {
    if (code == NULL) return NULL;
    if (size < 0x30) size = 0x30;
    void *block = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) return NULL;
    *code = block;
    return block;
}

__attribute__((noinline)) void ffi_closure_free(void *closure) {
    if (closure != NULL) munmap(closure, 4096);
}

__attribute__((noinline)) int ffi_prep_closure_loc(void *closure, void *cif, void *fun, void *user_data,
                                                   void *codeloc) {
    (void)codeloc;
    uint32_t *words = closure;
    words[0] = 0x58000090;  // ldr x16, [pc, #16]
    words[1] = 0x10fffff1;  // adr x17, .-4 (the block's start)
    words[2] = 0xd61f0200;  // br x16
    words[3] = 0;
    void **fields = closure;
    fields[2] = fake_ffi_entry;
    fields[3] = cif;
    fields[4] = fun;
    fields[5] = user_data;
    __builtin___clear_cache((char *)closure, (char *)closure + 0x30);
    return 0;
}

static void *writable_of[8];
static void *code_of[8];

__attribute__((noinline)) void *fake_ffi_make(long user) {
    void *code = NULL;
    void *writable = ffi_closure_alloc(0x30, &code);
    if (writable == NULL) return NULL;
    if (ffi_prep_closure_loc(writable, NULL, (void *)fake_ffi_fun, (void *)user, code) != 0) return NULL;
    for (int index = 0; index < 8; index++) {
        if (code_of[index] == NULL) {
            code_of[index] = code;
            writable_of[index] = writable;
            break;
        }
    }
    return code;
}

__attribute__((noinline)) void fake_ffi_release(void *code) {
    for (int index = 0; index < 8; index++) {
        if (code_of[index] == code) {
            ffi_closure_free(writable_of[index]);
            code_of[index] = NULL;
            return;
        }
    }
}
