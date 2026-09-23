// libtls-lib.so for tls-test: thread-local variables in a shared library, which its own code reaches
// through TLS descriptors (R_AARCH64_TLSDESC, the NDK's default for -fPIC) and the program through
// the initial-exec model (R_AARCH64_TLS_TPREL64).
#include <stdint.h>

__thread int lib_counter = 11;
__thread int lib_zero;
_Alignas(64) __thread char lib_aligned[64] = "aligned";
// Weak and never defined; used only from the assembly below, so it is declared weak there too.
__asm__(".weak lib_undefined_weak");

int lib_increment(int amount) {
    lib_counter += amount;
    return lib_counter;
}

int *lib_counter_address(void) {
    return &lib_counter;
}

int lib_zero_value(void) {
    return lib_zero;
}

int lib_aligned_ok(void) {
    return ((uintptr_t)lib_aligned % 64) == 0 && lib_aligned[0] == 'a';
}

// The address of an undefined weak TLS variable is NULL. clang folds &lib_undefined_weak == 0 to
// false, so the TLS descriptor call is spelled out, exactly as the compiler emits it.
int lib_weak_is_null(void) {
    void *address;
    __asm__ volatile(
        "adrp x0, :tlsdesc:lib_undefined_weak\n"
        "ldr x1, [x0, :tlsdesc_lo12:lib_undefined_weak]\n"
        "add x0, x0, :tlsdesc_lo12:lib_undefined_weak\n"
        ".tlsdesccall lib_undefined_weak\n"
        "blr x1\n"
        "mrs x1, tpidr_el0\n"
        "add %0, x0, x1\n"
        : "=r"(address)
        :
        : "x0", "x1", "x30", "memory", "cc");
    return address == 0;
}
