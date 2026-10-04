#!/usr/bin/env python3
"""ADR-0047 b1's probe on the Mac: apk-run runs the fixture el0-check, which tries the instructions of decision 3's
features under a SIGILL handler (fixtures/suite/el0-check.c). Each line must be what this Mac's sysctl lets one expect:
ECV, AFP and WFXT work where hw.optional.arm.FEAT_ECV, FEAT_AFP and FEAT_WFxT are 1 and raise SIGILL or drop their bits
where they are 0, and without SSBS (FEAT_SSBS 0) its MRS raises SIGILL. A Mac with SSBS has its line shown, not
compared: b2 decides from the iPhone's. The runtime reports none of these features yet (decision 3).
Prints "skipped: ..." while the fixture is not built.

Usage: el0_check.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work>
"""
import fcntl
import os
import subprocess
import sys

from hwcap_check import convert, sysctl_ones

ARM = "hw.optional.arm."
# Each feature's line with its FEAT name at 1 and at 0.
LINES = {
    "ECV": {True: "ECV: CNTVCTSS_EL0 between two CNTVCT_EL0 reads, CNTPCTSS_EL0 reads",
            False: "ECV: CNTVCTSS_EL0 SIGILL, CNTPCTSS_EL0 SIGILL"},
    "AFP": {True: "AFP: FIZ, AH and NEP kept; FIZ flushes a denormal input; in a signal handler still set; "
                  "after it cleared them restored",
            False: "AFP: FIZ, AH and NEP dropped; FIZ flushes nothing; in a signal handler cleared; "
                   "after it cleared them lost"},
    "WFxT": {True: "WFXT: WFET returns, WFIT returns", False: "WFXT: WFET SIGILL, WFIT SIGILL"},
}


def main():
    if len(sys.argv) != 7:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work = sys.argv[1:]
    fixture = os.path.join(fixtures, "el0-check")
    if not os.path.exists(fixture):
        print(f"skipped: {fixture} is not built (tools/build-fixtures.sh)")
        return

    # The containers stay from run to run; runs side by side convert one at a time.
    system = os.path.join(work, "system-lib64")
    program = os.path.join(work, "bin", "el0-check.dylib")
    for folder in (system, os.path.dirname(program)):
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        convert(apk_convert, fixture, program)

    command = [apk_run, "--mount", f"/system={bionic_system}", "--libraries", f"/system/lib64={system}", "--cwd", "/",
               "--argv0", "/data/local/tmp/el0-check", program]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL: no end within {timeout.timeout} s")
        sys.exit(1)
    ones = sysctl_ones()
    has = {feature: ARM + "FEAT_" + feature in ones for feature in ("ECV", "AFP", "SSBS", "WFxT")}
    got = result.stdout.splitlines()
    ssbs = [line for line in got if line.startswith("SSBS:")]
    wanted = [LINES["ECV"][has["ECV"]], LINES["AFP"][has["AFP"]],
              ssbs[0] if has["SSBS"] and ssbs else "SSBS: MRS SIGILL", LINES["WFxT"][has["WFxT"]]]
    if result.returncode == 0 and got == wanted:
        print("el0-check finds what this Mac's sysctl says: "
              + ", ".join(f"{feature} {int(value)}" for feature, value in has.items())
              + ("; SSBS shown: " + ssbs[0] if has["SSBS"] and ssbs else ""))
        return
    print(f"FAIL: exit status {result.returncode}")
    print("wanted:\n  " + "\n  ".join(wanted))
    print("got:\n  " + "\n  ".join(got))
    print("stderr, last 20 lines:")
    print("\n".join(result.stderr.splitlines()[-20:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
