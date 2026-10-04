// The complete register state of a guest thread (M2 step 5c): the state at a syscall, read from the
// syscall frame and the veneer's return path, and the state in a Darwin signal context. Only the
// kernel can load all 31 registers, sp and pc at once, so the runtime moves a thread into a state
// through a signal the thread sends itself: when the handler returns, XNU restores the thread from
// the signal context, which the handler has rewritten (signals::resume, R9).
#pragma once

#include "hwcap.h"
#include "syscall_frame.h"

#include <signal.h>
#include <sys/ucontext.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace apkrt {

struct GuestContext {
    std::uint64_t x[31];
    std::uint64_t sp;
    std::uint64_t pc;
    std::uint64_t nzcv;  // NZCV, and the kept PSTATE bits (kept_pstate_bits) where the device has them
    std::uint64_t fpsr;
    std::uint64_t fpcr;
    __uint128_t q[32];
};
static_assert(offsetof(GuestContext, sp) == 248 && offsetof(GuestContext, pc) == 256 &&
                  offsetof(GuestContext, nzcv) == 264 && offsetof(GuestContext, fpsr) == 272 &&
                  offsetof(GuestContext, fpcr) == 280 && offsetof(GuestContext, q) == 288 &&
                  sizeof(GuestContext) == 800,
              "layout used by apkrt_call_guest in syscall_entry.S");

/// The host signal the runtime sends a thread to run code in its signal context (signals.h).
/// Darwin's SIGURG: ignored by default, and free, because guest signals are emulated.
inline constexpr int runtime_signal = SIGURG;

/// Whether the syscall that entered `frame` returns through a veneer (its return address holds the
/// veneer's last two instructions), so that context_at_syscall knows the guest's state.
bool returns_through_veneer(const apkrt_syscall_frame &frame);

/// The guest's state once the syscall entry has returned with the registers of `frame` (x0 the
/// result) and `sp` just above the frame: through a veneer, as the veneer leaves it (x16 and x30 from
/// its save area at sp, sp above that, pc the target of its final branch); else at the return address.
GuestContext context_after_syscall(const apkrt_syscall_frame &frame, std::uint64_t sp);

/// The guest's state at the svc that entered `frame`, as the syscall will return to it: registers,
/// flags and SIMD from the frame (x0 as the frame has it), x16 and x30 from the veneer's save area
/// above the frame, sp as it was at the svc, pc the instruction after the svc (the target of the
/// veneer's final branch). Nullopt when the return address is not in a veneer.
std::optional<GuestContext> context_at_syscall(const apkrt_syscall_frame &frame);

/// The state a Darwin signal context holds, and writing a state into one.
GuestContext context_from(const ucontext_t &uc);
void apply(const GuestContext &context, ucontext_t &uc);

/// The PSTATE bits besides NZCV that a GuestContext's nzcv word keeps, as Linux's signal frames keep the whole
/// PSTATE (ADR-0047 b2): the ones EL0 may write among the features the guest is told in `bits`, SSBS (bit 12) where
/// AT_HWCAP has SSBS and DIT (bit 24) where it has DIT. No MRS or MSR of either runs on a device without it.
std::uint64_t kept_pstate_bits(const hwcap::Bits &bits);
/// This device's (hwcap::guest()).
std::uint64_t kept_pstate_bits();
/// This device's kept bits as PSTATE has them now: for a context the runtime builds without them (a thread's start,
/// a call into guest code), which keeps them as they are. apply() sets them, by MSR where they differ.
std::uint64_t live_pstate_bits();

}  // namespace apkrt
