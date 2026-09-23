// The register frame that syscall_entry.S builds on the guest's stack for every Linux syscall.
// The dispatcher reads the arguments from it and writes the result into x[0]; later syscalls
// that change registers of the guest (clone, rt_sigreturn) will change the frame the same way.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

struct apkrt_syscall_frame {
    std::uint64_t x[32];  // x0-x30; x[31] is padding
    std::uint64_t nzcv;
    std::uint64_t fpsr;
    __uint128_t q[32];
};
static_assert(offsetof(apkrt_syscall_frame, nzcv) == 256, "layout used by syscall_entry.S");
static_assert(offsetof(apkrt_syscall_frame, q) == 272, "layout used by syscall_entry.S");
static_assert(sizeof(apkrt_syscall_frame) == 784, "FRAME_SIZE in syscall_entry.S");

/// Called by syscall_entry.S: runs the Linux syscall in x[8] with the arguments in x[0]-x[5].
void apkrt_syscall_dispatch(apkrt_syscall_frame *frame);

/// syscall_entry.S. Its address goes into the runtime slots of every container.
void apkrt_syscall_entry(void);

/// syscall_entry.S: switches to `stack_pointer`, clears every register and jumps to `entry`.
[[noreturn]] void apkrt_enter_guest(std::uint64_t entry, std::uint64_t stack_pointer);

/// syscall_entry.S: _setjmp and _longjmp for the host side of guest threads.
struct apkrt_host_context {
    std::uint64_t words[22];  // x19-x30, sp, padding, d8-d15
};
__attribute__((returns_twice)) int apkrt_save_host(apkrt_host_context *context);
[[noreturn]] void apkrt_return_to_host(const apkrt_host_context *context);
}
