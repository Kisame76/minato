// The complete register state of a guest thread (M2 step 5c): the state at a syscall, read from the
// syscall frame and the veneer's return path, and the state in a Darwin signal context. Only the
// kernel can load all 31 registers, sp and pc at once, so the runtime moves a thread into a state
// through a signal the thread sends itself: when the handler returns, XNU restores the thread from
// the signal context, which the handler has rewritten (signals::resume, R9).
#pragma once

#include "syscall_frame.h"

#include <signal.h>
#include <sys/ucontext.h>

#include <cstdint>
#include <optional>

namespace apkrt {

struct GuestContext {
    std::uint64_t x[31];
    std::uint64_t sp;
    std::uint64_t pc;
    std::uint64_t nzcv;
    std::uint64_t fpsr;
    std::uint64_t fpcr;
    __uint128_t q[32];
};

/// The host signal the runtime sends a thread to run code in its signal context (signals.h).
/// Darwin's SIGURG: ignored by default, and free, because guest signals are emulated.
inline constexpr int runtime_signal = SIGURG;

/// The guest's state at the svc that entered `frame`, as the syscall will return to it: registers,
/// flags and SIMD from the frame (x0 as the frame has it), x16 and x30 from the veneer's save area
/// above the frame, sp as it was at the svc, pc the instruction after the svc (the target of the
/// veneer's final branch). Nullopt when the return address is not in a veneer.
std::optional<GuestContext> context_at_syscall(const apkrt_syscall_frame &frame);

/// The state a Darwin signal context holds, and writing a state into one.
GuestContext context_from(const ucontext_t &uc);
void apply(const GuestContext &context, ucontext_t &uc);

}  // namespace apkrt
