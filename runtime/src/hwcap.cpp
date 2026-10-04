#include "hwcap.h"

#include <sys/sysctl.h>

#include <cstddef>

namespace apkrt::hwcap {
namespace {

constexpr std::uint64_t bit(unsigned number) {
    return std::uint64_t{1} << number;
}

// One Linux bit, reported when the device answers 1 for any of its names: the hw.optional.arm.FEAT_* name, and the
// older ones macOS and iOS also give. Decision 2 of ADR-0047, with the bit values of Linux's uapi asm/hwcap.h.
struct Entry {
    const char *names[3];
    std::uint64_t hwcap;
    std::uint64_t hwcap2;
};

constexpr Entry table[] = {
    {{"hw.optional.floatingpoint"}, bit(0), 0},                                               // FP
    {{"hw.optional.arm.AdvSIMD", "hw.optional.AdvSIMD", "hw.optional.neon"}, bit(1), 0},      // ASIMD
    {{"hw.optional.arm.FEAT_AES"}, bit(3), 0},                                                // AES
    {{"hw.optional.arm.FEAT_PMULL"}, bit(4), 0},                                              // PMULL
    {{"hw.optional.arm.FEAT_SHA1"}, bit(5), 0},                                               // SHA1
    {{"hw.optional.arm.FEAT_SHA256"}, bit(6), 0},                                             // SHA2
    {{"hw.optional.arm.FEAT_CRC32", "hw.optional.armv8_crc32"}, bit(7), 0},                   // CRC32
    {{"hw.optional.arm.FEAT_LSE", "hw.optional.armv8_1_atomics"}, bit(8), 0},                 // ATOMICS
    {{"hw.optional.arm.FEAT_FP16", "hw.optional.neon_fp16"}, bit(9) | bit(10), 0},            // FPHP, ASIMDHP
    {{"hw.optional.arm.FEAT_RDM"}, bit(12), 0},                                               // ASIMDRDM
    {{"hw.optional.arm.FEAT_JSCVT"}, bit(13), 0},                                             // JSCVT
    {{"hw.optional.arm.FEAT_FCMA", "hw.optional.armv8_3_compnum"}, bit(14), 0},               // FCMA
    {{"hw.optional.arm.FEAT_LRCPC"}, bit(15), 0},                                             // LRCPC
    {{"hw.optional.arm.FEAT_DPB"}, bit(16), 0},                                               // DCPOP
    {{"hw.optional.arm.FEAT_SHA3", "hw.optional.armv8_2_sha3"}, bit(17), 0},                  // SHA3
    {{"hw.optional.arm.FEAT_DotProd"}, bit(20), 0},                                           // ASIMDDP
    {{"hw.optional.arm.FEAT_SHA512", "hw.optional.armv8_2_sha512"}, bit(21), 0},              // SHA512
    {{"hw.optional.arm.FEAT_FHM", "hw.optional.armv8_2_fhm"}, bit(23), 0},                    // ASIMDFHM
    {{"hw.optional.arm.FEAT_DIT"}, bit(24), 0},                                               // DIT
    {{"hw.optional.arm.FEAT_LSE2"}, bit(25), 0},                                              // USCAT
    {{"hw.optional.arm.FEAT_LRCPC2"}, bit(26), 0},                                            // ILRCPC
    {{"hw.optional.arm.FEAT_FlagM"}, bit(27), 0},                                             // FLAGM
    {{"hw.optional.arm.FEAT_SSBS"}, bit(28), 0},                                              // SSBS (b2)
    {{"hw.optional.arm.FEAT_SB"}, bit(29), 0},                                                // SB
    {{"hw.optional.arm.FEAT_DPB2"}, 0, bit(0)},                                               // DCPODP
    {{"hw.optional.arm.FEAT_FlagM2"}, 0, bit(7)},                                             // FLAGM2
    {{"hw.optional.arm.FEAT_FRINTTS"}, 0, bit(8)},                                            // FRINT
    {{"hw.optional.arm.FEAT_I8MM"}, 0, bit(13)},                                              // I8MM
    {{"hw.optional.arm.FEAT_BF16"}, 0, bit(14)},                                              // BF16
    {{"hw.optional.arm.FEAT_ECV"}, 0, bit(19)},                                               // ECV (b2)
    {{"hw.optional.arm.FEAT_AFP"}, 0, bit(20)},                                               // AFP (b2)
    {{"hw.optional.arm.FEAT_RPRES"}, 0, bit(21)},                                             // RPRES
    {{"hw.optional.arm.FEAT_WFxT"}, 0, bit(31)},                                              // WFXT (b2)
    {{"hw.optional.arm.FEAT_EBF16"}, 0, bit(32)},                                             // EBF16
    {{"hw.optional.arm.FEAT_CSSC"}, 0, bit(34)},                                              // CSSC
    {{"hw.optional.arm.FEAT_HBC"}, 0, bit(44)},                                               // HBC
    {{"hw.optional.arm.FEAT_LUT"}, 0, bit(49)},                                               // LUT
    {{"hw.optional.arm.FEAT_FAMINMAX"}, 0, bit(50)},                                          // FAMINMAX
};
// Decision 3's SSBS, ECV, AFP and WFXT came in with b1's checks at EL0, which each passed where the device's name is 1
// (b2); guest contexts and signal frames keep PSTATE.SSBS as they keep DIT (guest_context.h). Not in the table, never:
// CPUID, PACA, PACG, BTI, SVE, SME, MTE, EVTSTRM and FPMR (decision 4).

// The names that Linux's /proc/cpuinfo gives the table's bits on its Features line, in its order: AT_HWCAP's bits
// from the lowest, then AT_HWCAP2's (hwcap_str of arch/arm64/kernel/cpuinfo.c; lut and faminmax since 6.12).
struct LinuxName {
    std::uint64_t hwcap;
    std::uint64_t hwcap2;
    const char *name;
};

constexpr LinuxName linux_names[] = {
    {bit(0), 0, "fp"},        {bit(1), 0, "asimd"},     {bit(3), 0, "aes"},        {bit(4), 0, "pmull"},
    {bit(5), 0, "sha1"},      {bit(6), 0, "sha2"},      {bit(7), 0, "crc32"},      {bit(8), 0, "atomics"},
    {bit(9), 0, "fphp"},      {bit(10), 0, "asimdhp"},  {bit(12), 0, "asimdrdm"},  {bit(13), 0, "jscvt"},
    {bit(14), 0, "fcma"},     {bit(15), 0, "lrcpc"},    {bit(16), 0, "dcpop"},     {bit(17), 0, "sha3"},
    {bit(20), 0, "asimddp"},  {bit(21), 0, "sha512"},   {bit(23), 0, "asimdfhm"},  {bit(24), 0, "dit"},
    {bit(25), 0, "uscat"},    {bit(26), 0, "ilrcpc"},   {bit(27), 0, "flagm"},     {bit(28), 0, "ssbs"},
    {bit(29), 0, "sb"},       {0, bit(0), "dcpodp"},    {0, bit(7), "flagm2"},     {0, bit(8), "frint"},
    {0, bit(13), "i8mm"},     {0, bit(14), "bf16"},     {0, bit(19), "ecv"},       {0, bit(20), "afp"},
    {0, bit(21), "rpres"},    {0, bit(31), "wfxt"},     {0, bit(32), "ebf16"},     {0, bit(34), "cssc"},
    {0, bit(44), "hbc"},      {0, bit(49), "lut"},      {0, bit(50), "faminmax"},
};

}  // namespace

Bits from(const std::function<bool(const char *name)> &has) {
    Bits bits{0, 0};
    for (const Entry &entry : table) {
        for (const char *name : entry.names) {
            if (name != nullptr && has(name)) {
                bits.hwcap |= entry.hwcap;
                bits.hwcap2 |= entry.hwcap2;
                break;
            }
        }
    }
    return bits;
}

const Bits &guest() {
    static const Bits bits = from([](const char *name) {
        int value = 0;
        std::size_t size = sizeof value;
        return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value == 1;
    });
    return bits;
}

std::string features(const Bits &bits) {
    std::string line;
    for (const LinuxName &entry : linux_names) {
        if ((bits.hwcap & entry.hwcap) == 0 && (bits.hwcap2 & entry.hwcap2) == 0) continue;
        if (!line.empty()) line += ' ';
        line += entry.name;
    }
    return line;
}

}  // namespace apkrt::hwcap
