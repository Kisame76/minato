#!/usr/bin/env python3
"""getaddrinfo without a reachable network (fixtures/suite/gai-offline-test.c): apk-run runs the fixture under
sandbox-exec with the corpus harness's profile (tools/corpus.sb), whose "no network" is the host's resolver out of
reach, as an app on a device in airplane mode finds it, and its output is compared with
fixtures/expected/gai-offline-test.out, which Android 15 printed on the AVD in airplane mode (the EAI code, its text
and errno, which libcore turns into UnknownHostException or, for EACCES and EPERM, into a SecurityException).
Prints "skipped: ..." while the fixture is not built or sandbox-exec is missing.

Usage: gai_offline_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PROFILE = os.path.join(REPO, "tools", "corpus.sb")


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def main():
    if len(sys.argv) != 8:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work, expected = sys.argv[1:]
    fixture = os.path.join(fixtures, "gai-offline-test")
    if not os.path.exists(fixture):
        print(f"skipped: {fixture} is not built (tools/build-fixtures.sh)")
        return
    sandbox = shutil.which("sandbox-exec")
    if sandbox is None:
        print("skipped: sandbox-exec is missing")
        return
    with open(expected) as file:
        lines = file.read().splitlines()
    system = os.path.join(work, "system-lib64")
    os.makedirs(system, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        convert(apk_convert, fixture, os.path.join(work, "gai-offline-test.dylib"))
    # apk-run's mounts call realpath(), which lstats every component of a path; corpus.sb denies the home folder, so
    # the repo's ancestor folders that lie under it (e.g. the parent of <repo folder>) must be allowed -- metadata only,
    # so the home folder's files stay unreadable. tools/corpus-run.py writes the same rule for the corpus harness;
    # mirror it here because gai_offline uses the static profile directly. (Without it, a lookup in a clone under the
    # home folder fails before the fixture runs, as every contributor's clone does.)
    home = os.path.expanduser("~")
    ancestors, folder = [], os.path.dirname(REPO)
    while folder.startswith(home):
        ancestors.append(folder)
        folder = os.path.dirname(folder)
    profile = os.path.join(work, "gai-offline.sb")
    with open(PROFILE) as source, open(profile, "w") as handle:
        handle.write(source.read())
        if ancestors:
            literals = " ".join(f'(literal "{a}")' for a in ancestors)
            handle.write(";; The repo's ancestor folders under the home folder, for realpath (it lstats each path "
                         "component); metadata only.\n"
                         f"(allow file-read-metadata {literals})\n")
    result = subprocess.run(
        [sandbox, "-D", f"HOME={home}", "-D", f"REPO={REPO}", "-D", f"WORK={os.path.abspath(work)}",
         "-f", profile, apk_run, "--mount", f"/system={bionic_system}", "--libraries", f"/system/lib64={system}",
         "--argv0", "./gai-offline-test", os.path.join(work, "gai-offline-test.dylib")],
        capture_output=True, text=True, errors="replace", timeout=120)
    if result.returncode != 0 or result.stdout.splitlines() != lines:
        print(f"FAIL: exit status {result.returncode}, stdout {result.stdout.splitlines()}, expected {lines}, "
              f"stderr's end {result.stderr.splitlines()[-5:]}")
        sys.exit(1)
    print("gai offline: getaddrinfo without a network answers as Android does")


if __name__ == "__main__":
    main()
