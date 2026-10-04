// The CPU features the guest is told (ADR-0047): AT_HWCAP and AT_HWCAP2, which the auxiliary vector and the
// argument of every ifunc resolver carry. They are what the device has, read from its sysctl once, under the ADR's
// policy: the instruction set extensions that run at EL0 as any app code does (decision 2), and nothing that needs
// a check first (decision 3), the kernel's part (keys, PROT_BTI, signal frame state) or a trap on XNU (decision 4).
#pragma once

#include <cstdint>
#include <functional>

namespace apkrt::hwcap {

struct Bits {
    std::uint64_t hwcap;   // AT_HWCAP, Linux's HWCAP_* bits (arch/arm64/include/uapi/asm/hwcap.h)
    std::uint64_t hwcap2;  // AT_HWCAP2, its HWCAP2_* bits
};

/// The bits of a device whose sysctl `has` answers: true for a name that is 1 there (hw.optional.floatingpoint,
/// hw.optional.arm.FEAT_LSE, ...), false for one that is 0 or missing.
Bits from(const std::function<bool(const char *name)> &has);

/// This device's bits, read from its sysctl at the first call and kept for the process.
const Bits &guest();

}  // namespace apkrt::hwcap
