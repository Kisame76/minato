#!/usr/bin/env bash
# Compares every guest library that Minato builds with the names of its API 35 stub in the NDK (the names
# `llvm-nm -D --defined-only` reads, without version suffixes): a name of the stub that the library lacks is WRONG; a
# library with more names than its stub is fine. Prints OK, WRONG or SKIP per library and a total.
#   OK     the library exports every name of its stub
#   WRONG  names of the stub are missing (the first ones are printed)
#   SKIP   the library is not built (in none of the folders below), or there is no NDK: never counted as OK
# Exit 0 when every library is OK, 1 when any is WRONG, 77 when none is WRONG but any is SKIP.
# Left out: libc++.so. It is the platform's own C++ runtime library; Minato builds no libc++.so, apps bring
# libc++_shared.so or link libc++ statically, so there is nothing of Minato's to compare.
# Usage: tools/ndk-names-check.sh
#   ANDROID_NDK        the NDK, default r30 in ~/Library/Android/sdk/ndk
#   NDK_NAMES_LIBDIRS  folders of built libraries, colon separated; default build/bionic/lib, build/bionic-app/lib,
#                      build/art/lib, build/hwui/lib and build/graphics/guest/bin, where the bionic, ART, hwui and
#                      front builds put them
# Python 3.9 (/usr/bin/python3) is enough. Read only.
root="$(cd "$(dirname "$0")/.." && pwd)"
ndk="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
defaults="$root/build/bionic/lib:$root/build/bionic-app/lib:$root/build/art/lib:$root/build/hwui/lib"
dirs="${NDK_NAMES_LIBDIRS:-$defaults:$root/build/graphics/guest/bin}"
exec /usr/bin/python3 - "$ndk" "$dirs" <<'PYTHON'
import os
import subprocess
import sys

ndk, dirs = sys.argv[1], [d for d in sys.argv[2].split(":") if d]
toolchain = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "darwin-x86_64")
nm = os.path.join(toolchain, "bin", "llvm-nm")
stubs = os.path.join(toolchain, "sysroot", "usr", "lib", "aarch64-linux-android", "35")
LEFT_OUT = {"libc++.so"}

if not os.path.exists(nm) or not os.path.isdir(stubs):
    print("SKIP  no NDK at %s (llvm-nm and the API 35 stubs are needed)" % ndk)
    print("0 OK, 0 WRONG, 1 SKIP")
    sys.exit(77)


def names_of(path):
    out = subprocess.run([nm, "-D", "--defined-only", path], check=True, capture_output=True, text=True).stdout
    return {line.split()[-1].split("@")[0] for line in out.splitlines() if line.strip()}


rows = []
for stub in sorted(os.listdir(stubs)):
    if not stub.endswith(".so") or stub in LEFT_OUT:
        continue
    built = next((os.path.join(d, stub) for d in dirs if os.path.isfile(os.path.join(d, stub))), None)
    if built is None:
        rows.append(("SKIP", stub, "not built (looked in %d folders)" % len(dirs)))
        continue
    wanted, have = names_of(os.path.join(stubs, stub)), names_of(built)
    missing = sorted(wanted - have)
    if missing:
        shown = ", ".join(missing[:8]) + (", ..." if len(missing) > 8 else "")
        rows.append(("WRONG", stub, "%d of %d names missing: %s" % (len(missing), len(wanted), shown)))
    else:
        rows.append(("OK", stub, "all %d names of the stub are there" % len(wanted)))

width = max((len(name) for _, name, _ in rows), default=0)
for status, name, note in rows:
    print("%-5s %-*s  %s" % (status, width, name, note))
count = {s: sum(1 for r in rows if r[0] == s) for s in ("OK", "WRONG", "SKIP")}
print("%d OK, %d WRONG, %d SKIP" % (count["OK"], count["WRONG"], count["SKIP"]))
sys.exit(1 if count["WRONG"] else 77 if count["SKIP"] else 0)
PYTHON
