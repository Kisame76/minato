// Linux system calls without a libc, for the raw fixtures: arguments in x0 to x2, the number in
// x8, svc #0 (Linux AArch64 ABI).
#pragma once

#include <stddef.h>

static inline long raw_write(int fd, const void *buffer, size_t size) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buffer;
    register long x2 __asm__("x2") = (long)size;
    register long x8 __asm__("x8") = 64;  // __NR_write
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

__attribute__((noreturn)) static inline void raw_exit_group(int status) {
    register long x0 __asm__("x0") = status;
    register long x8 __asm__("x8") = 94;  // __NR_exit_group
    __asm__ volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    __builtin_unreachable();
}
