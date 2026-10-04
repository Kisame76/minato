#include "guest_context.h"

#include <cstring>

namespace apkrt {
namespace {

// ldp x16, x30, [sp], #16 and b <label>: the end of every svc veneer (rewrite.cpp).
constexpr std::uint32_t veneer_restore = 0xa8c1'7bf0;
constexpr std::uint32_t branch_mask = 0xfc00'0000;
constexpr std::uint32_t branch = 0x1400'0000;

std::uint64_t read_fpcr() {
    std::uint64_t value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

constexpr std::uint64_t nzcv_bits = 0xf000'0000;
// PSTATE's SSBS and DIT, in their places of PSTATE and of the SSBS and DIT registers alike, and their AT_HWCAP bits
// (Linux's uapi asm/hwcap.h: HWCAP_SSBS, HWCAP_DIT).
constexpr std::uint64_t pstate_ssbs = std::uint64_t{1} << 12;
constexpr std::uint64_t pstate_dit = std::uint64_t{1} << 24;
constexpr std::uint64_t hwcap_ssbs = std::uint64_t{1} << 28;
constexpr std::uint64_t hwcap_dit = std::uint64_t{1} << 24;

/// PSTATE's bits among `kept` as they are now.
std::uint64_t live_bits(std::uint64_t kept) {
    std::uint64_t bits = 0;
    std::uint64_t value;
    if (kept & pstate_ssbs) {
        asm volatile("mrs %0, s3_3_c4_c2_6" : "=r"(value));  // SSBS
        bits |= value & pstate_ssbs;
    }
    if (kept & pstate_dit) {
        asm volatile("mrs %0, s3_3_c4_c2_5" : "=r"(value));  // DIT
        bits |= value & pstate_dit;
    }
    return bits;
}

/// Sets PSTATE's kept bits to those of `nzcv`, each only where it differs.
void set_live_bits(std::uint64_t nzcv) {
    const std::uint64_t kept = kept_pstate_bits();
    if (kept == 0) return;
    const std::uint64_t wanted = nzcv & kept;
    const std::uint64_t differing = wanted ^ live_bits(kept);
    if (differing & pstate_ssbs) asm volatile("msr s3_3_c4_c2_6, %0" : : "r"(wanted & pstate_ssbs));  // SSBS
    if (differing & pstate_dit) asm volatile("msr s3_3_c4_c2_5, %0" : : "r"(wanted & pstate_dit));    // DIT
}

}  // namespace

bool returns_through_veneer(const apkrt_syscall_frame &frame) {
    const auto *code = reinterpret_cast<const std::uint32_t *>(frame.x[30]);
    return code != nullptr && code[0] == veneer_restore && (code[1] & branch_mask) == branch;
}

GuestContext context_after_syscall(const apkrt_syscall_frame &frame, std::uint64_t sp) {
    GuestContext context{};
    std::memcpy(context.x, frame.x, sizeof context.x);
    context.sp = sp;
    context.pc = frame.x[30];
    context.nzcv = frame.nzcv | live_pstate_bits();  // runtime code leaves the kept bits as the guest had them
    context.fpsr = frame.fpsr;
    context.fpcr = read_fpcr();  // the syscall entry leaves FPCR alone, so it is still the guest's
    std::memcpy(context.q, frame.q, sizeof context.q);
    if (returns_through_veneer(frame)) {
        const auto *code = reinterpret_cast<const std::uint32_t *>(frame.x[30]);
        // imm26, sign-extended, in instructions.
        const auto offset = static_cast<std::int64_t>(static_cast<std::int32_t>(code[1] << 6) >> 6) * 4;
        const auto *saved = reinterpret_cast<const std::uint64_t *>(sp);  // the veneer's stp x16, x30
        context.x[16] = saved[0];
        context.x[30] = saved[1];
        context.sp = sp + 16;
        context.pc = frame.x[30] + 4 + static_cast<std::uint64_t>(offset);
        // The loader's stubs branch to their ret: after it, the state is the caller's.
        if (context.pc == reinterpret_cast<std::uint64_t>(apkrt_loader_return)) context.pc = context.x[30];
    }
    return context;
}

std::optional<GuestContext> context_at_syscall(const apkrt_syscall_frame &frame) {
    if (!returns_through_veneer(frame)) return std::nullopt;
    return context_after_syscall(frame, reinterpret_cast<std::uint64_t>(&frame + 1));
}

GuestContext context_from(const ucontext_t &uc) {
    const auto &machine = *uc.uc_mcontext;
    GuestContext context{};
    for (int index = 0; index < 29; ++index) context.x[index] = machine.__ss.__x[index];
    context.x[29] = machine.__ss.__fp;
    context.x[30] = machine.__ss.__lr;
    context.sp = machine.__ss.__sp;
    context.pc = machine.__ss.__pc;
    context.nzcv = machine.__ss.__cpsr & (nzcv_bits | kept_pstate_bits());
    std::memcpy(context.q, machine.__ns.__v, sizeof context.q);
    context.fpsr = machine.__ns.__fpsr;
    context.fpcr = machine.__ns.__fpcr;
    return context;
}

void apply(const GuestContext &context, ucontext_t &uc) {
    auto &machine = *uc.uc_mcontext;
    for (int index = 0; index < 29; ++index) machine.__ss.__x[index] = context.x[index];
    machine.__ss.__fp = context.x[29];
    machine.__ss.__lr = context.x[30];
    machine.__ss.__sp = context.sp;
    machine.__ss.__pc = context.pc;
    // Only the condition flags; XNU keeps the rest of the PSTATE of a user thread as it is.
    machine.__ss.__cpsr = static_cast<std::uint32_t>((machine.__ss.__cpsr & 0x0fff'ffffu) | (context.nzcv & 0xf000'0000u));
    std::memcpy(machine.__ns.__v, context.q, sizeof machine.__ns.__v);
    machine.__ns.__fpsr = static_cast<std::uint32_t>(context.fpsr);
    machine.__ns.__fpcr = static_cast<std::uint32_t>(context.fpcr);
    // "As it is" means as the handler leaves it, not as the signal context says: XNU's sigreturn takes the kept bits
    // from the thread, so they go into PSTATE here (ADR-0047 b2; a cpsr bit written above would be dropped).
    set_live_bits(context.nzcv);
}

std::uint64_t kept_pstate_bits(const hwcap::Bits &bits) {
    return ((bits.hwcap & hwcap_ssbs) != 0 ? pstate_ssbs : 0) | ((bits.hwcap & hwcap_dit) != 0 ? pstate_dit : 0);
}

std::uint64_t kept_pstate_bits() {
    static const std::uint64_t kept = kept_pstate_bits(hwcap::guest());
    return kept;
}

std::uint64_t live_pstate_bits() {
    return live_bits(kept_pstate_bits());
}

}  // namespace apkrt
