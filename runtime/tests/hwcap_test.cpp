// Checks the CPU features the guest is told (ADR-0047 a and b2): the table from a device's sysctl names to Linux's
// HWCAP and HWCAP2 bits, against the real dumps of the Mac (M4 Pro) and the iPhone 15 Pro Max (A17 Pro), which must
// give decision 2's bits the ADR measured with, the same on both, and decision 3's as each device has them; each
// reported bit from each of its names alone; nothing from the features never reported (decision 4); and the
// device's own read, kept for the process.
#include "hwcap.h"

#include "check.h"

#include <sys/sysctl.h>

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

using apkrt::hwcap::Bits;

// The names that are 1 in the Mac's sysctl (macOS 27, M4 Pro; build/reports/hwcap-iphone-2026-10-03/mac-sysctl.txt).
const std::vector<std::string> mac = {
    "hw.optional.arm.FEAT_CRC32", "hw.optional.arm.FEAT_FlagM", "hw.optional.arm.FEAT_FlagM2",
    "hw.optional.arm.FEAT_FHM", "hw.optional.arm.FEAT_DotProd", "hw.optional.arm.FEAT_SHA3",
    "hw.optional.arm.FEAT_RDM", "hw.optional.arm.FEAT_LSE", "hw.optional.arm.FEAT_SHA256",
    "hw.optional.arm.FEAT_SHA512", "hw.optional.arm.FEAT_SHA1", "hw.optional.arm.FEAT_AES",
    "hw.optional.arm.FEAT_PMULL", "hw.optional.arm.FEAT_SB", "hw.optional.arm.FEAT_FRINTTS",
    "hw.optional.arm.FEAT_PACIMP", "hw.optional.arm.FEAT_LRCPC", "hw.optional.arm.FEAT_LRCPC2",
    "hw.optional.arm.FEAT_FCMA", "hw.optional.arm.FEAT_JSCVT", "hw.optional.arm.FEAT_PAuth",
    "hw.optional.arm.FEAT_PAuth2", "hw.optional.arm.FEAT_FPAC", "hw.optional.arm.FEAT_FPACCOMBINE",
    "hw.optional.arm.FEAT_DPB", "hw.optional.arm.FEAT_DPB2", "hw.optional.arm.FEAT_BF16",
    "hw.optional.arm.FEAT_I8MM", "hw.optional.arm.FEAT_WFxT", "hw.optional.arm.FEAT_RPRES",
    "hw.optional.arm.FEAT_ECV", "hw.optional.arm.FEAT_AFP", "hw.optional.arm.FEAT_LSE2",
    "hw.optional.arm.FEAT_CSV2", "hw.optional.arm.FEAT_CSV3", "hw.optional.arm.FEAT_DIT",
    "hw.optional.arm.AdvSIMD", "hw.optional.arm.AdvSIMD_HPFPCvt", "hw.optional.arm.FEAT_FP16",
    "hw.optional.arm.FEAT_BTI", "hw.optional.arm.FEAT_SME", "hw.optional.arm.FEAT_SME2",
    "hw.optional.arm.SME_F32F32", "hw.optional.arm.SME_BI32I32", "hw.optional.arm.SME_B16F32",
    "hw.optional.arm.SME_F16F32", "hw.optional.arm.SME_I8I32", "hw.optional.arm.SME_I16I32",
    "hw.optional.arm.FEAT_SME_F64F64", "hw.optional.arm.FEAT_SME_I16I64", "hw.optional.arm.FP_SyncExceptions",
    "hw.optional.floatingpoint", "hw.optional.neon", "hw.optional.neon_hpfp", "hw.optional.neon_fp16",
    "hw.optional.armv8_crc32", "hw.optional.armv8_gpi", "hw.optional.armv8_1_atomics", "hw.optional.armv8_2_fhm",
    "hw.optional.armv8_2_sha512", "hw.optional.armv8_2_sha3", "hw.optional.armv8_3_compnum",
    "hw.optional.ucnormal_mem", "hw.optional.arm64",
};

// The iPhone's (iOS 27.0.1, A17 Pro, read by the app; iphone-sysctl.txt): the Mac's without SME, WFxT and
// FPACCOMBINE, with SSBS.
const std::vector<std::string> iphone = {
    "hw.cpusubfamily", "hw.optional.arm.FEAT_CRC32", "hw.optional.arm.FEAT_FlagM", "hw.optional.arm.FEAT_FlagM2",
    "hw.optional.arm.FEAT_FHM", "hw.optional.arm.FEAT_DotProd", "hw.optional.arm.FEAT_SHA3",
    "hw.optional.arm.FEAT_RDM", "hw.optional.arm.FEAT_LSE", "hw.optional.arm.FEAT_SHA256",
    "hw.optional.arm.FEAT_SHA512", "hw.optional.arm.FEAT_SHA1", "hw.optional.arm.FEAT_AES",
    "hw.optional.arm.FEAT_PMULL", "hw.optional.arm.FEAT_SB", "hw.optional.arm.FEAT_FRINTTS",
    "hw.optional.arm.FEAT_PACIMP", "hw.optional.arm.FEAT_LRCPC", "hw.optional.arm.FEAT_LRCPC2",
    "hw.optional.arm.FEAT_FCMA", "hw.optional.arm.FEAT_JSCVT", "hw.optional.arm.FEAT_PAuth",
    "hw.optional.arm.FEAT_PAuth2", "hw.optional.arm.FEAT_FPAC", "hw.optional.arm.FEAT_DPB",
    "hw.optional.arm.FEAT_DPB2", "hw.optional.arm.FEAT_BF16", "hw.optional.arm.FEAT_I8MM",
    "hw.optional.arm.FEAT_RPRES", "hw.optional.arm.FEAT_ECV", "hw.optional.arm.FEAT_AFP",
    "hw.optional.arm.FEAT_LSE2", "hw.optional.arm.FEAT_CSV2", "hw.optional.arm.FEAT_CSV3",
    "hw.optional.arm.FEAT_DIT", "hw.optional.arm.FEAT_SSBS", "hw.optional.arm.AdvSIMD",
    "hw.optional.arm.AdvSIMD_HPFPCvt", "hw.optional.arm.FEAT_FP16", "hw.optional.arm.FEAT_BTI",
    "hw.optional.arm.FP_SyncExceptions", "hw.optional.floatingpoint", "hw.optional.neon", "hw.optional.neon_hpfp",
    "hw.optional.neon_fp16", "hw.optional.armv8_crc32", "hw.optional.armv8_gpi", "hw.optional.armv8_1_atomics",
    "hw.optional.armv8_2_fhm", "hw.optional.armv8_2_sha512", "hw.optional.armv8_2_sha3",
    "hw.optional.armv8_3_compnum", "hw.optional.ucnormal_mem", "hw.optional.arm64",
};

Bits bits_of(const std::vector<std::string> &names) {
    const std::set<std::string> ones(names.begin(), names.end());
    return apkrt::hwcap::from([&](const char *name) { return ones.count(name) != 0; });
}

constexpr std::uint64_t bit(unsigned number) {
    return std::uint64_t{1} << number;
}

// Decision 2, and decision 3's bits that b1's checks at EL0 let in (b2), each Linux bit with every name a device may
// give it under (arch/arm64's uapi hwcap.h; the NDK's asm/hwcap.h has the same values).
struct Reported {
    const char *name;
    std::uint64_t hwcap, hwcap2;
};
const Reported reported[] = {
    {"hw.optional.floatingpoint", bit(0), 0},              // FP
    {"hw.optional.arm.AdvSIMD", bit(1), 0},                // ASIMD
    {"hw.optional.AdvSIMD", bit(1), 0},
    {"hw.optional.neon", bit(1), 0},
    {"hw.optional.arm.FEAT_AES", bit(3), 0},               // AES
    {"hw.optional.arm.FEAT_PMULL", bit(4), 0},             // PMULL
    {"hw.optional.arm.FEAT_SHA1", bit(5), 0},              // SHA1
    {"hw.optional.arm.FEAT_SHA256", bit(6), 0},            // SHA2
    {"hw.optional.arm.FEAT_CRC32", bit(7), 0},             // CRC32
    {"hw.optional.armv8_crc32", bit(7), 0},
    {"hw.optional.arm.FEAT_LSE", bit(8), 0},               // ATOMICS
    {"hw.optional.armv8_1_atomics", bit(8), 0},
    {"hw.optional.arm.FEAT_FP16", bit(9) | bit(10), 0},    // FPHP and ASIMDHP
    {"hw.optional.neon_fp16", bit(9) | bit(10), 0},
    {"hw.optional.arm.FEAT_RDM", bit(12), 0},              // ASIMDRDM
    {"hw.optional.arm.FEAT_JSCVT", bit(13), 0},            // JSCVT
    {"hw.optional.arm.FEAT_FCMA", bit(14), 0},             // FCMA
    {"hw.optional.armv8_3_compnum", bit(14), 0},
    {"hw.optional.arm.FEAT_LRCPC", bit(15), 0},            // LRCPC
    {"hw.optional.arm.FEAT_DPB", bit(16), 0},              // DCPOP
    {"hw.optional.arm.FEAT_SHA3", bit(17), 0},             // SHA3
    {"hw.optional.armv8_2_sha3", bit(17), 0},
    {"hw.optional.arm.FEAT_DotProd", bit(20), 0},          // ASIMDDP
    {"hw.optional.arm.FEAT_SHA512", bit(21), 0},           // SHA512
    {"hw.optional.armv8_2_sha512", bit(21), 0},
    {"hw.optional.arm.FEAT_FHM", bit(23), 0},              // ASIMDFHM
    {"hw.optional.armv8_2_fhm", bit(23), 0},
    {"hw.optional.arm.FEAT_DIT", bit(24), 0},              // DIT
    {"hw.optional.arm.FEAT_LSE2", bit(25), 0},             // USCAT
    {"hw.optional.arm.FEAT_LRCPC2", bit(26), 0},           // ILRCPC
    {"hw.optional.arm.FEAT_FlagM", bit(27), 0},            // FLAGM
    {"hw.optional.arm.FEAT_SSBS", bit(28), 0},             // SSBS
    {"hw.optional.arm.FEAT_SB", bit(29), 0},               // SB
    {"hw.optional.arm.FEAT_DPB2", 0, bit(0)},              // DCPODP
    {"hw.optional.arm.FEAT_FlagM2", 0, bit(7)},            // FLAGM2
    {"hw.optional.arm.FEAT_FRINTTS", 0, bit(8)},           // FRINT
    {"hw.optional.arm.FEAT_I8MM", 0, bit(13)},             // I8MM
    {"hw.optional.arm.FEAT_BF16", 0, bit(14)},             // BF16
    {"hw.optional.arm.FEAT_ECV", 0, bit(19)},              // ECV
    {"hw.optional.arm.FEAT_AFP", 0, bit(20)},              // AFP
    {"hw.optional.arm.FEAT_RPRES", 0, bit(21)},            // RPRES
    {"hw.optional.arm.FEAT_WFxT", 0, bit(31)},             // WFXT
    {"hw.optional.arm.FEAT_EBF16", 0, bit(32)},            // EBF16
    {"hw.optional.arm.FEAT_CSSC", 0, bit(34)},             // CSSC
    {"hw.optional.arm.FEAT_HBC", 0, bit(44)},              // HBC
    {"hw.optional.arm.FEAT_LUT", 0, bit(49)},              // LUT
    {"hw.optional.arm.FEAT_FAMINMAX", 0, bit(50)},         // FAMINMAX
};

TEST(the_mac_and_the_iphone_get_the_bits_the_adr_measured_with_and_their_own_of_decision_3) {
    // Decision 2's bits are the same on both, 0x2fb3f7fb and 0x206181. Decision 3's as b1 found them: ECV, AFP and
    // WFXT on the Mac; SSBS, ECV and AFP on the iPhone, which has no WFxT.
    const Bits on_the_mac = bits_of(mac);
    CHECK(on_the_mac.hwcap == 0x2fb3f7fb && on_the_mac.hwcap2 == 0x80386181);
    const Bits on_the_iphone = bits_of(iphone);
    CHECK(on_the_iphone.hwcap == 0x3fb3f7fb && on_the_iphone.hwcap2 == 0x386181);
}

TEST(each_reported_bit_comes_from_each_of_its_names_alone) {
    int wrong = 0;
    for (const Reported &entry : reported) {
        const Bits got = bits_of({entry.name});
        if (got.hwcap != entry.hwcap || got.hwcap2 != entry.hwcap2) {
            std::fprintf(stderr, "%s gives 0x%llx 0x%llx\n", entry.name, static_cast<unsigned long long>(got.hwcap),
                         static_cast<unsigned long long>(got.hwcap2));
            ++wrong;
        }
    }
    CHECK(wrong == 0);
}

TEST(the_features_never_reported_give_nothing) {
    // Decision 4: CPUID (no name), the pointer authentication keys, BTI, SVE and SME with their sub-bits, MTE, the
    // event stream (no name) and FPMR.
    const Bits got = bits_of({
        "hw.optional.arm.FEAT_PAuth", "hw.optional.arm.FEAT_PAuth2",
        "hw.optional.arm.FEAT_FPAC", "hw.optional.arm.FEAT_FPACCOMBINE", "hw.optional.arm.FEAT_PACIMP",
        "hw.optional.arm.FEAT_BTI", "hw.optional.arm.FEAT_SVE", "hw.optional.arm.FEAT_SVE2",
        "hw.optional.arm.FEAT_SME", "hw.optional.arm.FEAT_SME2", "hw.optional.arm.SME_F32F32",
        "hw.optional.arm.FEAT_SME_F64F64", "hw.optional.arm.FEAT_MTE", "hw.optional.arm.FEAT_MTE2",
        "hw.optional.arm.FEAT_MTE3", "hw.optional.arm.FEAT_FPMR", "hw.optional.arm.FEAT_FP8",
        "hw.optional.arm.FEAT_CSV2", "hw.optional.arm.FEAT_CSV3", "hw.optional.arm64",
    });
    CHECK(got.hwcap == 0 && got.hwcap2 == 0);
}

TEST(without_any_feature_nothing_is_reported) {
    const Bits got = bits_of({});
    CHECK(got.hwcap == 0 && got.hwcap2 == 0);
}

TEST(the_features_line_names_the_reported_bits_as_linux_s_cpuinfo_does) {
    // /proc/cpuinfo's Features line (ADR-0047 c): Linux's names, AT_HWCAP's bits from the lowest, then AT_HWCAP2's
    // (hwcap_str of arch/arm64/kernel/cpuinfo.c, 6.6 and, for lut and faminmax, 6.12). The Mac's and the iPhone's
    // lines differ in decision 3's bits.
    CHECK(apkrt::hwcap::features(bits_of(mac)) ==
          "fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp asimdrdm jscvt fcma lrcpc dcpop sha3 asimddp sha512 "
          "asimdfhm dit uscat ilrcpc flagm sb dcpodp flagm2 frint i8mm bf16 ecv afp rpres wfxt");
    CHECK(apkrt::hwcap::features(bits_of(iphone)) ==
          "fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp asimdrdm jscvt fcma lrcpc dcpop sha3 asimddp sha512 "
          "asimdfhm dit uscat ilrcpc flagm ssbs sb dcpodp flagm2 frint i8mm bf16 ecv afp rpres");
    Bits every{0, 0};
    for (const Reported &entry : reported) {
        every.hwcap |= entry.hwcap;
        every.hwcap2 |= entry.hwcap2;
    }
    CHECK(apkrt::hwcap::features(every) ==
          "fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp asimdrdm jscvt fcma lrcpc dcpop sha3 asimddp sha512 "
          "asimdfhm dit uscat ilrcpc flagm ssbs sb dcpodp flagm2 frint i8mm bf16 ecv afp rpres wfxt ebf16 cssc hbc lut "
          "faminmax");
    CHECK(apkrt::hwcap::features(Bits{0, 0}).empty());
}

TEST(the_device_s_own_bits_come_from_its_sysctl_once) {
    const Bits read = apkrt::hwcap::from([](const char *name) {
        int value = 0;
        std::size_t size = sizeof value;
        return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value == 1;
    });
    const Bits &guest = apkrt::hwcap::guest();
    CHECK(guest.hwcap == read.hwcap && guest.hwcap2 == read.hwcap2);
    CHECK(&apkrt::hwcap::guest() == &guest);
    // Every ARMv8 CPU has FP and Advanced SIMD, and the guest never runs without them.
    CHECK((guest.hwcap & 0b11) == 0b11);
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
