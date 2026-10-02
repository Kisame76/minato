#!/usr/bin/env python3
"""hwui through our loader (ADR-0043 a1, graphics.hwui_load): apk-run runs the fixture hwui-load as
/system/bin/hwui-load, a program in linkerconfig's [system] section, with the files where Android 15 has them:
  - libhwui.so and the libraries of the set it needs (tools/build-hwui.sh) in /system/lib64, with bionic's;
  - beside them libbase.so, libziparchive.so, libexpat.so and the NDK's libc++_shared.so from ART's build, where
    Android 15 has the platform's own copies (and libc++.so);
  - ART's libraries in /apex/com.android.art/lib64, where the program's namespace reaches ICU and libnativehelper
    through its links (ADR-0040 decision 2).
hwui-load must print exactly <expected> and exit with 0: dlopen loads libhwui.so with what it needs and runs their
constructors, dlsym finds init_android_graphics and register_android_graphics_classes, and init_android_graphics
returns. Prints "skipped: ..." while a part is not built.

Usage: hwui_load_run.py <fixtures-bin> <hwui-lib> <art-lib> <bionic-lib> <bionic-system> <apk-convert> <apk-run>
                        <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

ART_LIBRARIES = "/apex/com.android.art/lib64"
# The libraries of ART's build that hwui's set needs from /system/lib64.
PLATFORM_COPIES = ["libbase.so", "libziparchive.so", "libexpat.so", "libc++_shared.so"]


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def libraries(folder):
    return sorted(name for name in os.listdir(folder) if name.endswith(".so"))


def main():
    if len(sys.argv) != 10:
        raise SystemExit(__doc__)
    fixtures, hwui, art, bionic_lib, bionic_system, apk_convert, apk_run, work, expected = sys.argv[1:]
    for path, how in ((os.path.join(fixtures, "hwui-load"), "tools/build-fixtures.sh"),
                      (os.path.join(hwui, "libhwui.so"), "tools/build-hwui.sh"),
                      (os.path.join(art, "libbase.so"), "tools/build-art.sh")):
        if not os.path.exists(path):
            print(f"skipped: {path} is not built ({how})")
            return

    # The containers stay from run to run; runs side by side convert one at a time.
    system = os.path.join(work, "system-lib64")
    art_lib64 = os.path.join(work, "art-lib64")
    program = os.path.join(work, "bin", "hwui-load.dylib")
    for folder in (system, art_lib64, os.path.dirname(program)):
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for folder, names in ((bionic_lib, libraries(bionic_lib)), (hwui, libraries(hwui)), (art, PLATFORM_COPIES)):
            for name in names:
                convert(apk_convert, os.path.join(folder, name), os.path.join(system, name + ".dylib"))
        for name in libraries(art):
            convert(apk_convert, os.path.join(art, name), os.path.join(art_lib64, name + ".dylib"))
        convert(apk_convert, os.path.join(fixtures, "hwui-load"), program)

    command = [apk_run, "--mount", f"/system={bionic_system}",
               "--libraries", f"/system/lib64={system}", "--libraries", f"{ART_LIBRARIES}={art_lib64}",
               "--cwd", "/", "--argv0", "/system/bin/hwui-load", program]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=300)
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL: no end within {timeout.timeout} s")
        sys.exit(1)
    with open(expected) as handle:
        wanted = handle.read()
    if result.returncode == 0 and result.stdout == wanted:
        print("hwui-load opened libhwui.so with what it needs, found its two functions and set up Skia")
        return
    print(f"FAIL: exit status {result.returncode}")
    print(f"stdout: [{result.stdout}]")
    print(f"expected: [{wanted}]")
    print("stderr, last 40 lines:")
    print("\n".join(result.stderr.splitlines()[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
