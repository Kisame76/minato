// The register frame that syscall_entry.S builds on the guest's stack for every Linux syscall.
// The dispatcher reads the arguments from it and writes the result into x[0]; later syscalls
// that change registers of the guest (clone, rt_sigreturn) will change the frame the same way.
#pragma once

#include "APKRuntime/apkrt.h"

#include <cstddef>
#include <cstdint>

extern "C" {

struct apkrt_syscall_frame {
    std::uint64_t x[32];  // x0-x30; x[31] is not restored, the dispatcher keeps x0 from before there
    std::uint64_t nzcv;
    std::uint64_t fpsr;
    __uint128_t q[32];
};
static_assert(offsetof(apkrt_syscall_frame, nzcv) == 256, "layout used by syscall_entry.S");
static_assert(offsetof(apkrt_syscall_frame, q) == 272, "layout used by syscall_entry.S");
static_assert(sizeof(apkrt_syscall_frame) == 784, "FRAME_SIZE in syscall_entry.S");

/// Called by syscall_entry.S: runs the Linux syscall in x[8] with the arguments in x[0]-x[5].
/// Returns the calling guest thread's signals::ThreadSignals when the exit is to look for signals
/// (signals::at_syscall_exit), else null.
void *apkrt_syscall_dispatch(apkrt_syscall_frame *frame);

/// Called by syscall_entry.S when its exit found a deliverable signal (signals::deliver_at_syscall_exit).
[[noreturn]] void apkrt_syscall_deliver(apkrt_syscall_frame *frame);

/// syscall_entry.S. Its address goes into the runtime slots of every container.
void apkrt_syscall_entry(void);

/// Labels in syscall_entry.S that the runtime signal's handler knows (ADR-0025): the entry is in
/// the runtime from apkrt_syscall_entered on, and from apkrt_syscall_returning up to
/// apkrt_syscall_returned the exit counts as returned, with apkrt_syscall_return its final ret.
extern const std::uint32_t apkrt_syscall_entered[];
extern const std::uint32_t apkrt_syscall_returning[];
extern const std::uint32_t apkrt_syscall_return[];
extern const std::uint32_t apkrt_syscall_returned[];

/// syscall_entry.S: libdl's functions for apkrt_loader, as syscalls of the runtime's own
/// (loader_syscalls.h). They all end at the ret apkrt_loader_return.
void *apkrt_loader_dlopen(const char *name, int flags, const void *caller, int *error_number);
void *apkrt_loader_dlsym(void *handle, const char *symbol, const char *version, const void *caller);
int apkrt_loader_dlclose(void *handle);
const char *apkrt_loader_dlerror(void);
int apkrt_loader_dladdr(const void *address, void *info);
void *apkrt_loader_dlopen_in(const char *name, int flags, const void *caller, int *error_number,
                             const char *search_path);
extern const std::uint32_t apkrt_loader_return[];
/// Labels around the stubs (ADR-0025): from apkrt_loader_stubs up to apkrt_loader_back the thread
/// is on its way into the loader, from there to apkrt_loader_end on its way back to the caller.
extern const std::uint32_t apkrt_loader_stubs[];
extern const std::uint32_t apkrt_loader_back[];
extern const std::uint32_t apkrt_loader_end[];

/// syscall_entry.S: apkrt_call_guest (APKRuntime/apkrt.h) calls guest code as guest code
/// (ADR-0025); its words up to here are runtime code.
extern const std::uint32_t apkrt_call_guest_end[];

/// syscall_entry.S: the entry for calls from guest code into host code (ADR-0030, decision 1), which
/// the front libs' stubs reach through slot 0 of their tables, with the host function in x16. Not
/// callable from C. Its labels, for the runtime signal's handler: in host code from
/// apkrt_host_call_entered on; from apkrt_host_call_returning up to apkrt_host_call_returned the exit
/// counts as returned, with apkrt_host_call_return its final ret.
void apkrt_host_call(void);
extern const std::uint32_t apkrt_host_call_entered[];
extern const std::uint32_t apkrt_host_call_returning[];
extern const std::uint32_t apkrt_host_call_return[];
extern const std::uint32_t apkrt_host_call_returned[];


/// syscall_entry.S: _setjmp and _longjmp for the host side of guest threads.
struct apkrt_host_context {
    std::uint64_t words[22];  // x19-x30, sp, padding, d8-d15
};
__attribute__((returns_twice)) int apkrt_save_host(apkrt_host_context *context);
[[noreturn]] void apkrt_return_to_host(const apkrt_host_context *context);
}
