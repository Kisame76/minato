#!/usr/bin/env python3
"""The CPU features a guest program learns (ADR-0047 a): apk-run runs the fixture hwcap-test, which prints
getauxval's AT_HWCAP and AT_HWCAP2 and what its own ifunc resolver got from the linker. All must be the bits this
Mac's sysctl gives under ADR-0047's policy, which this script maps with its own copy of decision 2's table, not the
runtime's (runtime/src/hwcap.cpp): the resolver's hwcap with _IFUNC_ARG_HWCAP set, and its __ifunc_arg_t of 24
bytes with both words. Prints "skipped: ..." while a part is not built.

Usage: hwcap_check.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work>
"""
import fcntl
import os
import subprocess
import sys

ARM = "hw.optional.arm."
# Decision 2: each Linux bit (arch/arm64's uapi asm/hwcap.h) with the sysctl names that give it.
HWCAP = {
    0: ["hw.optional.floatingpoint"],                                        # FP
    1: [ARM + "AdvSIMD", "hw.optional.AdvSIMD", "hw.optional.neon"],         # ASIMD
    3: [ARM + "FEAT_AES"], 4: [ARM + "FEAT_PMULL"], 5: [ARM + "FEAT_SHA1"],  # AES, PMULL, SHA1
    6: [ARM + "FEAT_SHA256"],                                                # SHA2
    7: [ARM + "FEAT_CRC32", "hw.optional.armv8_crc32"],                      # CRC32
    8: [ARM + "FEAT_LSE", "hw.optional.armv8_1_atomics"],                    # ATOMICS
    9: [ARM + "FEAT_FP16", "hw.optional.neon_fp16"],                         # FPHP
    10: [ARM + "FEAT_FP16", "hw.optional.neon_fp16"],                        # ASIMDHP
    12: [ARM + "FEAT_RDM"], 13: [ARM + "FEAT_JSCVT"],                        # ASIMDRDM, JSCVT
    14: [ARM + "FEAT_FCMA", "hw.optional.armv8_3_compnum"],                  # FCMA
    15: [ARM + "FEAT_LRCPC"], 16: [ARM + "FEAT_DPB"],                        # LRCPC, DCPOP
    17: [ARM + "FEAT_SHA3", "hw.optional.armv8_2_sha3"],                     # SHA3
    20: [ARM + "FEAT_DotProd"],                                              # ASIMDDP
    21: [ARM + "FEAT_SHA512", "hw.optional.armv8_2_sha512"],                 # SHA512
    23: [ARM + "FEAT_FHM", "hw.optional.armv8_2_fhm"],                       # ASIMDFHM
    24: [ARM + "FEAT_DIT"], 25: [ARM + "FEAT_LSE2"], 26: [ARM + "FEAT_LRCPC2"],  # DIT, USCAT, ILRCPC
    27: [ARM + "FEAT_FlagM"], 29: [ARM + "FEAT_SB"],                         # FLAGM, SB
}
HWCAP2 = {
    0: [ARM + "FEAT_DPB2"], 7: [ARM + "FEAT_FlagM2"], 8: [ARM + "FEAT_FRINTTS"],  # DCPODP, FLAGM2, FRINT
    13: [ARM + "FEAT_I8MM"], 14: [ARM + "FEAT_BF16"], 21: [ARM + "FEAT_RPRES"],   # I8MM, BF16, RPRES
    32: [ARM + "FEAT_EBF16"], 34: [ARM + "FEAT_CSSC"], 44: [ARM + "FEAT_HBC"],     # EBF16, CSSC, HBC
    49: [ARM + "FEAT_LUT"], 50: [ARM + "FEAT_FAMINMAX"],                           # LUT, FAMINMAX
}
IFUNC_ARG_HWCAP = 1 << 62


def sysctl_ones():
    """The hw.optional names that are 1 on this Mac."""
    result = subprocess.run(["/usr/sbin/sysctl", "hw.optional"], capture_output=True, text=True, check=True)
    ones = set()
    for line in result.stdout.splitlines():
        name, _, value = line.partition(":")
        if value.strip() == "1":
            ones.add(name.strip())
    return ones


def bits(table, ones):
    return sum(1 << number for number, names in table.items() if any(name in ones for name in names))


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def main():
    if len(sys.argv) != 7:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work = sys.argv[1:]
    fixture = os.path.join(fixtures, "hwcap-test")
    if not os.path.exists(fixture):
        print(f"skipped: {fixture} is not built (tools/build-fixtures.sh)")
        return

    # The containers stay from run to run; runs side by side convert one at a time.
    system = os.path.join(work, "system-lib64")
    program = os.path.join(work, "bin", "hwcap-test.dylib")
    for folder in (system, os.path.dirname(program)):
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        convert(apk_convert, fixture, program)

    command = [apk_run, "--mount", f"/system={bionic_system}", "--libraries", f"/system/lib64={system}", "--cwd", "/",
               "--argv0", "/data/local/tmp/hwcap-test", program]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL: no end within {timeout.timeout} s")
        sys.exit(1)
    ones = sysctl_ones()
    hwcap, hwcap2 = bits(HWCAP, ones), bits(HWCAP2, ones)
    wanted = [f"AT_HWCAP 0x{hwcap:x}", f"AT_HWCAP2 0x{hwcap2:x}",
              f"ifunc resolver: hwcap 0x{hwcap:x}, _IFUNC_ARG_HWCAP set",
              f"ifunc argument: size 24, hwcap 0x{hwcap:x}, hwcap2 0x{hwcap2:x}", "probe: 1"]
    got = result.stdout.splitlines()
    if result.returncode == 0 and got == wanted:
        print(f"hwcap-test learns this Mac's features: AT_HWCAP 0x{hwcap:x}, AT_HWCAP2 0x{hwcap2:x}, also in its ifunc "
              "resolver")
        return
    print(f"FAIL: exit status {result.returncode}")
    print("wanted:\n  " + "\n  ".join(wanted))
    print("got:\n  " + "\n  ".join(got))
    print("stderr, last 20 lines:")
    print("\n".join(result.stderr.splitlines()[-20:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
