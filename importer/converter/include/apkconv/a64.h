// Register use of A64 instructions: which general-purpose registers an instruction reads and
// writes, from its encoding (Arm ARM, chapter C4, "A64 Instruction Set Encoding"). The converter
// needs it for x18, which XNU sets to zero at every context switch (F4, ADR-0019).
//
// Covered are the base instruction set, every extension whose instructions name general-purpose
// registers (LSE, LSE128, LRCPC 1 to 3, PAuth and PAuth_LR, MTE, MOPS, CSSC, LS64, CRC32, TME,
// WFxT, GCS, THE, CMPBR, CPA, LSUI, SYSINSTR128, SYSREG128), and the FP and SIMD instructions that
// move values between general-purpose and SIMD registers. SVE and SME are left out: AT_HWCAP does
// not offer them, and the A17 Pro has neither (ADR-0019), so their instructions cannot run.
#pragma once

#include <cstdint>

namespace apk::a64 {

/// Bit n stands for xn (or wn), n = 0 to 30. Register number 31 never appears: it is sp or xzr,
/// depending on the instruction, and neither is a register whose value XNU could lose.
struct RegisterUse {
    std::uint32_t reads = 0;
    std::uint32_t writes = 0;

    bool operator==(const RegisterUse &) const = default;
};

/// The general-purpose registers `instruction` reads and writes, the implicit ones included: x30
/// of BL, x16 and x17 of PACIA1716, the eight registers of ST64B. An unallocated encoding uses
/// none, like SVE and SME: it cannot run.
RegisterUse register_use(std::uint32_t instruction);

}  // namespace apk::a64
