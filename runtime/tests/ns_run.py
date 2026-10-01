#!/usr/bin/env python3
"""A program outside the APEXes and ART's folder (ADR-0039 a, ADR-0040 decision 2): apk-run runs the fixture
ns-test as /system/bin/ns-test, where Android 15 runs a program in linkerconfig's [system] section, with the
fixture library libns-lib.so in /apex/com.android.art/lib64 under the name libjdwp.so, one of the names that
section links to com_android_art, and libns-dep.so and libns-plugin.so beside it:
  - the program's dlopen of libns-dep.so, a library of that folder off the link list, fails as linker64 says;
  - its dlopen of libjdwp.so finds the library through the link, and the library finds what it needs
    (libns-dep.so) and what it opens (libns-plugin.so) in its own folder.
ns-test must print exactly <expected> and exit with 0. The expected output is ours: Android lets nothing be
installed into /system/bin or an APEX. Prints "skipped: ..." while a part is not built.

Usage: ns_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

ART_LIBRARIES = "/apex/com.android.art/lib64"
# The containers in ART's folder: the name the loader finds, and the fixture it is made from.
IN_ART_FOLDER = {"libjdwp.so": "libns-lib.so", "libns-dep.so": "libns-dep.so", "libns-plugin.so": "libns-plugin.so"}


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
    for name in ["ns-test"] + sorted(set(IN_ART_FOLDER.values())):
        if not os.path.exists(os.path.join(fixtures, name)):
            print(f"skipped: {os.path.join(fixtures, name)} is not built (tools/build-fixtures.sh)")
            return

    # The containers stay from run to run; runs side by side convert one at a time.
    system = os.path.join(work, "system-lib64")
    art = os.path.join(work, "art-lib64")
    program = os.path.join(work, "bin", "ns-test.dylib")
    for folder in (system, art, os.path.dirname(program)):
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        for name, fixture in IN_ART_FOLDER.items():
            convert(apk_convert, os.path.join(fixtures, fixture), os.path.join(art, name + ".dylib"))
        convert(apk_convert, os.path.join(fixtures, "ns-test"), program)

    command = [apk_run, "--mount", f"/system={bionic_system}",
               "--libraries", f"/system/lib64={system}", "--libraries", f"{ART_LIBRARIES}={art}",
               "--cwd", "/", "--argv0", "/system/bin/ns-test", program, "libns-dep.so", "libjdwp.so"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=120)
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL: no end within {timeout.timeout} s")
        sys.exit(1)
    with open(expected) as handle:
        wanted = handle.read()
    if result.returncode == 0 and result.stdout == wanted:
        print("ns-test found ART's folder through the link list, and its library what it needs and opens there")
        return
    print(f"FAIL: exit status {result.returncode}")
    print(f"stdout: [{result.stdout}]")
    print(f"expected: [{wanted}]")
    print("stderr, last 40 lines:")
    print("\n".join(result.stderr.splitlines()[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
