#!/usr/bin/env python3
"""The NDK libraries the guest's bionic build brings from AOSP's own modules (the compatibility plan's gap 14): each
built library exports exactly the names of the NDK's API 35 stub, with their versions, as Android 15's does, and the
guest's public.libraries.txt lists it, as Android 15's does, so that an app's library may link it. A library missing
from a built bionic fails; without the NDK or a built bionic the test is skipped.

Usage: ndk_libraries_test.py <bionic lib directory> <bionic system directory> [<NDK directory>]
"""
import os
import subprocess
import sys

API = 35
TRIPLE = "aarch64-linux-android"
# bionic's libstdc++.so: operator new and delete, __cxa_guard_* and __cxa_pure_virtual (bionic/libc/Android.bp).
LIBRARIES = ["libstdc++"]


def defined_symbols(nm, path):
    output = subprocess.run([nm, "-D", "--defined-only", path], check=True, capture_output=True, text=True).stdout
    return {line.split()[-1] for line in output.splitlines()}


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    built, system = sys.argv[1], sys.argv[2]
    ndk = sys.argv[3] if len(sys.argv) == 4 else os.environ.get("ANDROID_NDK") or os.path.expanduser(
        "~/Library/Android/sdk/ndk/30.0.16248370")
    toolchain = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "darwin-x86_64")
    nm = os.path.join(toolchain, "bin", "llvm-nm")
    stubs = os.path.join(toolchain, "sysroot", "usr", "lib", TRIPLE, str(API))
    if not os.path.exists(nm) or not os.path.isdir(stubs):
        print(f"skipped: no NDK at {ndk}")
        return
    if not os.path.exists(os.path.join(built, "libc.so")):
        print(f"skipped: no bionic build in {built} (tools/build-bionic.sh)")
        return
    public_libraries = os.path.join(system, "etc", "public.libraries.txt")
    with open(public_libraries, encoding="utf-8") as handle:
        public = {line.split()[0] for line in handle if line.strip() and not line.startswith("#")}
    problems, checked = [], []
    for library in LIBRARIES:
        if f"{library}.so" not in public:
            problems.append(f"{public_libraries} does not list {library}.so")
        path = os.path.join(built, f"{library}.so")
        if not os.path.exists(path):
            problems.append(f"{library}.so is missing from {built}")
            continue
        expected = defined_symbols(nm, os.path.join(stubs, f"{library}.so"))
        exported = defined_symbols(nm, path)
        problems += [f"{library}.so does not export {name}" for name in sorted(expected - exported)]
        problems += [f"{library}.so exports {name}, which Android's does not" for name in sorted(exported - expected)]
        checked.append(f"{library}.so ({len(expected & exported)} names)")
    if problems:
        print("\n".join(problems))
        raise SystemExit(1)
    print("exports as the NDK's: " + ", ".join(checked))


if __name__ == "__main__":
    main()
