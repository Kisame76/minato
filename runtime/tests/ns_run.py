#!/usr/bin/env python3
"""A program outside the APEXes and the folders of ART and Conscrypt (ADR-0039 a, ADR-0040 decision 2, ADR-0046 a):
apk-run runs the fixture ns-test as /system/bin/ns-test, where Android 15 runs a program in linkerconfig's [system]
section, three times:
  - with the fixture library libns-lib.so in /apex/com.android.art/lib64 under the name libjdwp.so, one of the names
    that section links to com_android_art, and libns-dep.so and libns-plugin.so beside it:
    - the program's dlopen of libns-dep.so, a library of that folder off the link list, fails as linker64 says;
    - its dlopen of libjdwp.so finds the library through the link, and the library finds what it needs
      (libns-dep.so) and what it opens (libns-plugin.so) in its own folder;
  - with the three in /apex/com.android.conscrypt/lib64, libns-lib.so under the name libjavacrypto.so, the library
    that conscrypt.jar loads:
    - the program's dlopen of libjavacrypto.so fails, as no link reaches that folder;
    - android_dlopen_ext opens it in the exported namespace com_android_conscrypt, where libnativeloader opens it
      for conscrypt.jar and ends the process if the namespace is missing, and the library finds what it needs and
      what it opens in its own folder;
  - with libns-plugin.so in an app's JNI folder, the lib/arm64 of Android 15's code path
    /data/app/~~<base64>==/<package>-<base64>==, which apk-run's --libraries and --mount must take whole, its '='
    included (ADR-0042 b1): the program's dlopen of the library by that path opens it.
Each run is a process of its own: the loader's one scope would give the second run's library the first run's
libraries by name (ADR-0033 decision 7). The runs must print exactly <expected> together and exit with 0. The
expected output is ours: Android lets nothing be installed into /system/bin or an APEX. Prints "skipped: ..." while
a part is not built.

Usage: ns_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

# An app's code path as Android 15 names it, with the halves of a SHA-256 in base64, each ending in "==".
APP_CODE = "/data/app/~~AAECAwQFBgcICQoLDA0ODw==/io.github.kisame76.minato.test.ns-EBESExQVFhcYGRobHB0eHw=="

# Each run: the folder in <work> that holds a library folder, that folder's guest path, the containers in it (the
# name the loader finds, and the fixture it is made from), ns-test's arguments, and a guest path that apk-run also
# mounts on the folder, or None.
RUNS = [
    ("art-lib64", "/apex/com.android.art/lib64",
     {"libjdwp.so": "libns-lib.so", "libns-dep.so": "libns-dep.so", "libns-plugin.so": "libns-plugin.so"},
     ["libns-dep.so", "libjdwp.so"], None),
    ("conscrypt-lib64", "/apex/com.android.conscrypt/lib64",
     {"libjavacrypto.so": "libns-lib.so", "libns-dep.so": "libns-dep.so", "libns-plugin.so": "libns-plugin.so"},
     ["libjavacrypto.so", "com_android_conscrypt:libjavacrypto.so"], None),
    ("app-lib", f"{APP_CODE}/lib/arm64", {"libns-plugin.so": "libns-plugin.so"},
     [f"{APP_CODE}/lib/arm64/libns-plugin.so"], APP_CODE),
]


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
    for name in ["ns-test"] + sorted({fixture for _, _, containers, _, _ in RUNS for fixture in containers.values()}):
        if not os.path.exists(os.path.join(fixtures, name)):
            print(f"skipped: {os.path.join(fixtures, name)} is not built (tools/build-fixtures.sh)")
            return

    # The containers stay from run to run; runs side by side convert one at a time.
    system = os.path.join(work, "system-lib64")
    program = os.path.join(work, "bin", "ns-test.dylib")
    for folder in [system, os.path.dirname(program)] + [os.path.join(work, run[0]) for run in RUNS]:
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        for folder, _, containers, _, _ in RUNS:
            for name, fixture in containers.items():
                convert(apk_convert, os.path.join(fixtures, fixture), os.path.join(work, folder, name + ".dylib"))
        convert(apk_convert, os.path.join(fixtures, "ns-test"), program)

    printed = ""
    for folder, libraries, _, arguments, mount in RUNS:
        command = [apk_run, "--mount", f"/system={bionic_system}",
                   "--libraries", f"/system/lib64={system}", "--libraries", f"{libraries}={os.path.join(work, folder)}",
                   "--cwd", "/", "--argv0", "/system/bin/ns-test", program] + arguments
        if mount is not None:
            command[1:1] = ["--mount", f"{mount}={os.path.join(work, folder)}"]
        try:
            result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)
        except subprocess.TimeoutExpired as timeout:
            print(f"FAIL: no end within {timeout.timeout} s")
            sys.exit(1)
        printed += result.stdout
        if result.returncode != 0:
            break
    with open(expected) as handle:
        wanted = handle.read()
    if result.returncode == 0 and printed == wanted:
        print("ns-test found ART's folder through the link list and Conscrypt's through its exported namespace, and "
              "the library there what it needs and opens beside it; and an app's library by its code path")
        return
    print(f"FAIL: exit status {result.returncode}")
    print(f"stdout: [{printed}]")
    print(f"expected: [{wanted}]")
    print("stderr, last 40 lines:")
    print("\n".join(result.stderr.splitlines()[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
