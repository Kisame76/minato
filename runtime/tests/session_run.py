#!/usr/bin/env python3
"""apk-run's --configuration (ADR-0039, config-test): a session begins before the program starts, so the fixture
session-test sees its configuration through the private syscall session_configuration and through libandroid's
minato_AConfiguration_fromSession. apk-run runs it four times:
  - with --configuration 1290x2796@3,de-DE: that screen, portrait from its shape, night NO, 120 Hz, and decision 9's
    AConfiguration from it (density 480, 430 x 932 dp);
  - with --configuration 2796x1290@3: landscape, and en-US without a locale;
  - without the option: no session, so both answer ENOENT;
  - with a malformed configuration: apk-run's usage, exit status 2.
The exit status is the program's 0 in the first three, with a session as without one.
Then kill-self-test, whose second thread sends SIGKILL to its own process (ADR-0028, changed 2026-10-07), by kill,
by tgkill to the main thread and by sigqueue: with a session the program ends at once through the session's end
handler with 137, as a shell shows the kill, and apk-run's handler exits with it; without one the host's SIGKILL ends
apk-run. Neither thread prints after the kill.
Prints "skipped: ..." while a part is not built.

Usage: session_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work>
"""
import fcntl
import os
import subprocess
import sys

EXPECTED = {
    "portrait": [
        "session_configuration: screen 1290 x 2796 px at scale 3.0, orientation 1, night 1, refresh 120, locale de-DE",
        "AConfiguration: density 480, screen 430 x 932 dp, smallest 430 dp, orientation 1, locale de-DE",
    ],
    "landscape": [
        "session_configuration: screen 2796 x 1290 px at scale 3.0, orientation 2, night 1, refresh 120, locale en-US",
        "AConfiguration: density 480, screen 932 x 430 dp, smallest 430 dp, orientation 2, locale en-US",
    ],
    "none": [
        "session_configuration: error 2",
        "AConfiguration: error 2",
    ],
}
# kill-self-test's lines before its kill (fixtures/expected/kill-self-test.out).
KILLED = [
    "kill-self-test: the main thread waits for the second thread",
    "kill-self-test: the second thread sends SIGKILL to its own process",
]


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
    for fixture in ("session-test", "kill-self-test"):
        if not os.path.exists(os.path.join(fixtures, fixture)):
            print(f"skipped: {os.path.join(fixtures, fixture)} is not built (tools/build-fixtures.sh)")
            return

    # The containers stay from run to run; runs side by side convert one at a time.
    system = os.path.join(work, "system-lib64")
    programs = {name: os.path.join(work, "bin", f"{name}.dylib") for name in ("session-test", "kill-self-test")}
    for folder in (system, os.path.join(work, "bin")):
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        for name, program in programs.items():
            convert(apk_convert, os.path.join(fixtures, name), program)

    def run(*options, fixture="session-test", arguments=()):
        command = [apk_run, *options, "--mount", f"/system={bionic_system}", "--libraries", f"/system/lib64={system}",
                   "--cwd", "/", "--argv0", f"/data/local/tmp/{fixture}", programs[fixture], *arguments]
        try:
            return subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)
        except subprocess.TimeoutExpired as timeout:
            print(f"FAIL: {fixture} {' '.join(options + tuple(arguments))}: no end within {timeout.timeout} s")
            sys.exit(1)

    failures = []
    for name, options in (("portrait", ["--configuration", "1290x2796@3,de-DE"]),
                          ("landscape", ["--configuration", "2796x1290@3"]),
                          ("none", [])):
        result = run(*options)
        if result.returncode != 0 or result.stdout.splitlines() != EXPECTED[name]:
            failures.append(f"{name}: exit status {result.returncode}, stdout {result.stdout.splitlines()}, "
                            f"stderr's end {result.stderr.splitlines()[-5:]}")
    malformed = run("--configuration", "1290x@3")
    if malformed.returncode != 2 or malformed.stdout:
        failures.append(f"malformed: exit status {malformed.returncode}, stdout {malformed.stdout.splitlines()}")
    # SIGKILL to its own process: the session's end with 137, or without a session the host's SIGKILL (-9 here).
    for options, call, status in ((["--configuration", "1290x2796@3"], "kill", 137),
                                  (["--configuration", "1290x2796@3"], "tgkill", 137),
                                  (["--configuration", "1290x2796@3"], "sigqueue", 137),
                                  ([], "kill", -9)):
        result = run(*options, fixture="kill-self-test", arguments=[call])
        if result.returncode != status or result.stdout.splitlines() != KILLED:
            failures.append(f"kill-self-test {call} {'with' if options else 'without'} a session: exit status "
                            f"{result.returncode}, not {status}; stdout {result.stdout.splitlines()}, "
                            f"stderr's end {result.stderr.splitlines()[-5:]}")
    if not failures:
        print("--configuration began the session whose configuration session-test saw, portrait and landscape; "
              "without it, ENOENT; malformed, usage. kill-self-test's SIGKILL to itself ended the program through "
              "the session's end handler with 137, by kill, tgkill and sigqueue, and without a session by the "
              "host's SIGKILL")
        return
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)


if __name__ == "__main__":
    main()
