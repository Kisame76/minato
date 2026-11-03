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
returns. Before that, the set's files are checked (a2): every library must align its loaded segments to the guest's
16 KB pages, none to bionic's 2 MiB for huge pages, and libhwui.so must export its two functions but none of the
libpng and libcutils members it holds, which its version script keeps local, as on Android. And the build must carry
graphics/hwui's patches as they are: it lists them in patches.sha256 beside its lib folder once every library is
linked (graphics/hwui/CMakeLists.txt), since app_process loads hwui at every start (ADR-0043 test 2) and an older
build crashes there (patch 0007). Prints "skipped: ..." while a part is not built.

Usage: hwui_load_run.py <fixtures-bin> <hwui-lib> <art-lib> <bionic-lib> <bionic-system> <apk-convert> <apk-run>
                        <work> <expected>
"""
import fcntl
import hashlib
import os
import struct
import subprocess
import sys

ART_LIBRARIES = "/apex/com.android.art/lib64"
# The libraries of ART's build that hwui's set needs from /system/lib64.
PLATFORM_COPIES = ["libbase.so", "libziparchive.so", "libexpat.so", "libc++_shared.so"]
# The guest's page size (16 KB pages), which graphics/hwui/CMakeLists.txt links the set with.
PAGE = 0x4000
PT_LOAD = 1
SHT_DYNSYM = 11
# What libandroid_runtime calls in libhwui (AndroidRuntime.cpp), and the libraries of the set whose members the host
# flavor's static libandroidfw brings into it.
FUNCTIONS = ["init_android_graphics", "register_android_graphics_classes"]
HELD = ["libpng.so", "libcutils.so"]
PATCHES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hwui", "patches")


def load_alignments(path):
    """The alignments of a 64-bit little-endian ELF file's loaded segments."""
    with open(path, "rb") as handle:
        header = handle.read(64)
        table_offset, = struct.unpack_from("<Q", header, 32)
        entry_size, entries = struct.unpack_from("<HH", header, 54)
        handle.seek(table_offset)
        table = handle.read(entry_size * entries)
    # An entry starts with p_type; p_align is its last field, at 48.
    return {struct.unpack_from("<Q", table, index * entry_size + 48)[0] for index in range(entries)
            if struct.unpack_from("<I", table, index * entry_size)[0] == PT_LOAD}


def exported(path):
    """The global and weak names a 64-bit little-endian ELF file defines in its dynamic symbol table."""
    with open(path, "rb") as handle:
        data = handle.read()
    sections, = struct.unpack_from("<Q", data, 40)
    entry_size, entries = struct.unpack_from("<HH", data, 58)
    names = set()
    for index in range(entries):
        header = sections + index * entry_size
        if struct.unpack_from("<I", data, header + 4)[0] != SHT_DYNSYM:
            continue
        # sh_offset, sh_size and sh_link, which names the section of the symbols' strings.
        offset, size, link = struct.unpack_from("<QQI", data, header + 24)
        strings, = struct.unpack_from("<Q", data, sections + link * entry_size + 24)
        for symbol in range(offset, offset + size, 24):
            name, info, _, section = struct.unpack_from("<IBBH", data, symbol)
            if section != 0 and info >> 4 in (1, 2):
                start = strings + name
                names.add(data[start:data.index(b"\0", start)].decode())
    return names


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def libraries(folder):
    return sorted(name for name in os.listdir(folder) if name.endswith(".so"))


def patch_digests():
    """graphics/hwui's patches as their build lists them in patches.sha256: shasum -a 256's lines, by name."""
    lines = []
    for name in sorted(name for name in os.listdir(PATCHES) if name.endswith(".patch")):
        with open(os.path.join(PATCHES, name), "rb") as handle:
            lines.append(f"{hashlib.sha256(handle.read()).hexdigest()}  patches/{name}\n")
    return "".join(lines)


def built_digests(hwui):
    """The patches.sha256 beside the build's lib folder, or None."""
    stamp = os.path.join(os.path.dirname(os.path.normpath(hwui)), "patches.sha256")
    if not os.path.exists(stamp):
        return None
    with open(stamp) as handle:
        return handle.read()


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
    if built_digests(hwui) != patch_digests():
        print(f"FAIL: {os.path.dirname(os.path.normpath(hwui))} is not built with graphics/hwui/patches as they are "
              "(its patches.sha256): run tools/build-hwui.sh")
        sys.exit(1)
    misaligned = [f"{name} ({', '.join(hex(alignment) for alignment in sorted(alignments))})"
                  for name in libraries(hwui) for alignments in [load_alignments(os.path.join(hwui, name))]
                  if alignments != {PAGE}]
    if misaligned:
        print(f"FAIL: segments not aligned to the guest's {PAGE // 1024} KB pages: {', '.join(misaligned)}")
        sys.exit(1)
    symbols = exported(os.path.join(hwui, "libhwui.so"))
    theirs = set()
    for name in HELD:
        theirs |= exported(os.path.join(hwui, name))
    held = sorted(symbols & theirs)
    absent = [name for name in FUNCTIONS if name not in symbols]
    if held or absent:
        if held:
            print(f"FAIL: libhwui.so exports {len(held)} symbols of {' and '.join(HELD)} ({', '.join(held[:3])}, "
                  "...), which its version script keeps local on Android")
        if absent:
            print(f"FAIL: libhwui.so does not export {', '.join(absent)}")
        sys.exit(1)

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
