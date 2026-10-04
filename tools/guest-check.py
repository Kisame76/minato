#!/usr/bin/env python3
"""Checks the guest system in a built Minato.app (ADR-0039 c4): everything a Java app's start needs under Guest/, as
tools/embed-guest-system.sh lays it out and game mode mounts it, beside what a NativeActivity app's start needs; and
every container in Guest/ passes `codesign -v`.

  bin/              activity-host and app_process64, the programs /system/bin/activity-host and app_process64
  lib64/            /system/lib64: bionic, libandroid, the front libs, libandroid_runtime.so, hwui's set and ART's
                    copies for it (graphics/tests/hwui_load_run.py's PLATFORM_COPIES)
  system/           /system's files: framework.jar and the framework-res folder in framework/ (ADR-0039, decision 8),
                    platform.xml in etc/permissions/, public.libraries.txt, the time zones and the fonts
  apex/             /apex's files: the boot jars that BOOTCLASSPATH names (runtime/src/environment.cpp), ICU's data
                    and the time zones
  apex-lib64/       the APEXes' lib64: libart.so in ART's, libjavacrypto.so in Conscrypt's

A missing path, an empty file and a container that fails `codesign -v` are named; the run ends with 1 then.
Runs on /usr/bin/python3 (3.9).
Usage: tools/guest-check.py <Minato.app>
"""
import os
import subprocess
import sys

HWUI = ["libcutils.so", "libft2.so", "libharfbuzz_ng.so", "libharfbuzz_subset.so", "libhwui.so", "libimage_io.so",
        "libjpeg.so", "libjpegdecoder.so", "libjpegencoder.so", "libminikin.so", "libpng.so", "libultrahdr.so",
        "server_configurable_flags.so"]
ART_COPIES = ["libbase.so", "libziparchive.so", "libexpat.so", "libc++_shared.so"]
FRONT_LIBS = ["libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so"]
BIONIC = ["libc.so", "libdl.so", "libm.so", "liblog.so", "libandroid.so", "libnativewindow.so", "libz.so"]
ART_JARS = ["core-oj.jar", "core-libart.jar", "okhttp.jar", "bouncycastle.jar", "core-icu4j.jar"]

REQUIRED = (
    ["bin/activity-host.dylib", "bin/app_process64.dylib"]
    + [f"lib64/{name}.dylib" for name in BIONIC + FRONT_LIBS + ["libandroid_runtime.so"] + HWUI + ART_COPIES]
    + ["system/framework/framework.jar", "system/framework/framework-res/AndroidManifest.xml",
       "system/framework/framework-res/resources.arsc", "system/etc/permissions/platform.xml",
       "system/etc/public.libraries.txt", "system/usr/share/zoneinfo/tzdata"]
    + [f"apex/com.android.art/javalib/{jar}" for jar in ART_JARS]
    + ["apex/com.android.conscrypt/javalib/conscrypt.jar", "apex/com.android.i18n/etc/icu/icudt75l.dat",
       "apex/com.android.tzdata/etc/tz/tzdata", "apex-lib64/com.android.art/libart.so.dylib",
       "apex-lib64/com.android.conscrypt/libjavacrypto.so.dylib"]
)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    guest = os.path.join(sys.argv[1], "Guest")
    problems = []
    for path in REQUIRED:
        full = os.path.join(guest, path)
        if not os.path.isfile(full):
            problems.append(f"missing: Guest/{path}")
        elif os.path.getsize(full) == 0:
            problems.append(f"empty: Guest/{path}")
    containers = sorted(os.path.join(folder, name) for folder, _, names in os.walk(guest) for name in names
                        if name.endswith(".dylib"))
    for container in containers:
        result = subprocess.run(["codesign", "-v", container], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True)
        if result.returncode != 0:
            problems.append(f"codesign -v fails for Guest/{os.path.relpath(container, guest)}: {result.stdout.strip()}")
    for problem in problems:
        print(problem)
    files = sum(len(names) for _, _, names in os.walk(guest))
    if problems:
        print(f"guest check: {len(problems)} problems in {files} files")
        sys.exit(1)
    print(f"guest check: {len(REQUIRED)} paths of the starts there, {len(containers)} containers signed, {files} files")


if __name__ == "__main__":
    main()
