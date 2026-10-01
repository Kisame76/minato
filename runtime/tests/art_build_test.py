#!/usr/bin/env python3
"""ART as guest code (ADR-0033 a): what tools/build-art.sh builds into <art> is complete.
  - Every library ART starts with is in <art>/lib, in the release flavor and in ART's debug flavor, and dalvikvm
    in <art>/bin.
  - Every DT_NEEDED is found in the set: ART's libraries, the guest system's in <bionic-lib> and the NDK's
    libc++_shared.so. Every undefined symbol that is not weak is defined, with its version, by a library of the
    closure of its DT_NEEDED, as Android's linker would look it up.
  - The guest system's libraries that ART links, libdl_android.so among them, are the ones in <bionic-lib>: ART's
    build makes none of them.
  - apk-convert converts every library and dalvikvm without a refused site.
  - The boot jars in <art>/javalib define every class that ART's class roots (class_root.h) and its well-known
    classes (well_known_classes.cc) name, and none of the compile-time stand-ins of runtime/art/java.
Prints "skipped: ..." while <art>/lib is missing.

Usage: art_build_test.py <art> <bionic-lib> <apk-convert> <sources>
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

# The libraries ART loads at start or that dalvikvm links (ADR-0033 decisions 2 and 10); their DT_NEEDED closure
# brings in the rest.
RELEASE = ["libart", "libopenjdkjvm", "libjavacore", "libopenjdk", "libicu_jni", "libartpalette-system", "libsigchain",
           "libnativehelper", "libnativeloader"]
DEBUG = ["libartd", "libopenjdkjvmd", "libopenjdkd"]
PROGRAMS = ["dalvikvm"]
# The guest system's libraries that ART links. Minato's bionic build makes them and ART's build none, so that the
# guest has one of each (ADR-0033 decision 2).
SYSTEM = ["libc", "libm", "libdl", "libdl_android", "ld-android", "liblog", "libz"]
JARS = ["core-oj", "core-libart", "core-icu4j"]
failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def ndk_toolchain():
    ndk = os.environ.get("ANDROID_NDK") or os.path.expanduser("~/Library/Android/sdk/ndk/30.0.16248370")
    return os.path.join(ndk, "toolchains", "llvm", "prebuilt", "darwin-x86_64")


class Library:
    """A shared library's or program's dynamic linking: its soname, DT_NEEDED and dynamic symbols."""

    def __init__(self, path, toolchain):
        self.path = path
        dynamic = subprocess.run([os.path.join(toolchain, "bin", "llvm-readelf"), "-d", "-W", path], check=True,
                                 capture_output=True, text=True).stdout
        self.needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dynamic)
        soname = re.findall(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", dynamic)
        self.soname = soname[0] if soname else os.path.basename(path)
        symbols = subprocess.run([os.path.join(toolchain, "bin", "llvm-nm"), "-D", "-P", path], check=True,
                                 capture_output=True, text=True).stdout
        self.undefined, self.defined = set(), set()
        for line in symbols.splitlines():
            name, kind = line.split()[:2]
            if kind == "U":
                self.undefined.add(name)
            elif kind not in ("w", "v"):  # weak references may stay unresolved
                self.defined.add(name)
        # A versioned reference is met by its version, hidden or default; an unversioned one by the default.
        self.provides = set()
        for name in self.defined:
            base, _, version = name.partition("@")
            self.provides.add(base if not version else f"{base}@{version.lstrip('@')}")
            if not version or version.startswith("@"):
                self.provides.add(base)


def closure(library, by_soname):
    """The library and its DT_NEEDED, breadth first, as the linker's local group; and the sonames not found."""
    order, missing, queue = [], [], [library]
    seen = {library.soname}
    while queue:
        current = queue.pop(0)
        order.append(current)
        for needed in current.needed:
            if needed in seen:
                continue
            seen.add(needed)
            if needed in by_soname:
                queue.append(by_soname[needed])
            else:
                missing.append(needed)
    return order, missing


def dex_classes(data):
    """The descriptors of the classes a DEX file defines."""
    string_ids_off, type_ids_off = struct.unpack_from("<I", data, 0x3C)[0], struct.unpack_from("<I", data, 0x44)[0]
    class_defs_size, class_defs_off = struct.unpack_from("<II", data, 0x60)

    def string(index):
        offset = struct.unpack_from("<I", data, string_ids_off + 4 * index)[0]
        while data[offset] & 0x80:  # the ULEB128 length in UTF-16 units
            offset += 1
        end = data.index(0, offset + 1)
        return data[offset + 1:end].decode("utf-8", "replace")

    classes = set()
    for index in range(class_defs_size):
        type_index = struct.unpack_from("<I", data, class_defs_off + 32 * index)[0]
        classes.add(string(struct.unpack_from("<I", data, type_ids_off + 4 * type_index)[0]))
    return classes


def classes_art_checks(sources):
    """The class descriptors of ART's class roots and well-known classes, from its sources. A well-known class is
    named by its descriptor or, where ART finds it through JNI (CacheClass), by its JNI name."""
    runtime = os.path.join(sources, "art", "runtime")
    with open(os.path.join(runtime, "class_root.h")) as handle:
        roots = re.findall(r'M\(k\w+,\s*"([^"]+)"', handle.read())
    with open(os.path.join(runtime, "well_known_classes.cc")) as handle:
        text = handle.read()
    well_known = re.findall(r'"(L[\w/$]+;)"', text)
    well_known += [f"L{name};" for name in re.findall(r'CacheClass\(env,\s*"([\w/$]+)"\)', text)]
    descriptors = set()
    for descriptor in roots + well_known:
        element = descriptor.lstrip("[")
        if element.startswith("L"):  # primitive arrays need no class of the jars
            descriptors.add(element)
    return descriptors, len(roots), len(set(well_known))


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    art, bionic, convert, sources = sys.argv[1:]
    if not os.path.isdir(os.path.join(art, "lib")):
        print(f"skipped: no ART build in {art} (tools/build-art.sh)")
        return
    toolchain = ndk_toolchain()
    libcxx = os.path.join(toolchain, "sysroot", "usr", "lib", "aarch64-linux-android", "libc++_shared.so")

    # The set: ART's libraries, then the guest system's and the NDK's libc++_shared.so for what ART's lack.
    by_soname = {}
    for directory in (os.path.join(art, "lib"), bionic):
        for name in sorted(os.listdir(directory)) if os.path.isdir(directory) else []:
            if name.endswith(".so"):
                library = Library(os.path.join(directory, name), toolchain)
                by_soname.setdefault(library.soname, library)
    if os.path.exists(libcxx):
        by_soname.setdefault("libc++_shared.so", Library(libcxx, toolchain))
    own = [library for library in by_soname.values() if library.path.startswith(os.path.join(art, "lib"))]

    for name in RELEASE + DEBUG:
        check(f"{name}.so" in by_soname and by_soname[f"{name}.so"] in own, f"{name}.so is not in {art}/lib")
    for name in SYSTEM:
        check(os.path.exists(os.path.join(bionic, f"{name}.so")), f"{name}.so is not in {bionic}")
        check(not os.path.exists(os.path.join(art, "lib", f"{name}.so")),
              f"{name}.so is in {art}/lib, but ART links the guest system's in {bionic}")
    programs = []
    for name in PROGRAMS:
        path = os.path.join(art, "bin", name)
        check(os.path.exists(path), f"{name} is not in {art}/bin")
        if os.path.exists(path):
            programs.append(Library(path, toolchain))

    symbols = 0
    for library in own + programs:
        order, missing = closure(library, by_soname)
        for needed in missing:
            check(False, f"{os.path.basename(library.path)} needs {needed}, which the set lacks")
        provided = set().union(*(other.provides for other in order))
        unresolved = sorted(symbol for symbol in library.undefined if symbol not in provided)
        symbols += len(library.undefined)
        check(not unresolved, f"{os.path.basename(library.path)}: {len(unresolved)} undefined symbols that its "
                              f"DT_NEEDED do not define: {', '.join(unresolved[:12])}")

    with tempfile.TemporaryDirectory() as scratch:
        for library in own + programs:
            result = subprocess.run([convert, "convert", library.path,
                                     os.path.join(scratch, os.path.basename(library.path) + ".dylib")],
                                    capture_output=True, text=True)
            check(result.returncode == 0, f"apk-convert refuses {os.path.basename(library.path)}: "
                                          f"{(result.stderr or result.stdout).strip()[:300]}")

    defined = set()
    for jar in JARS:
        path = os.path.join(art, "javalib", f"{jar}.jar")
        check(os.path.exists(path), f"{jar}.jar is not in {art}/javalib")
        if not os.path.exists(path):
            continue
        with zipfile.ZipFile(path) as archive:
            dex_files = [name for name in archive.namelist() if re.fullmatch(r"classes\d*\.dex", name)]
            check(dex_files, f"{jar}.jar holds no classes.dex")
            for name in dex_files:
                defined |= dex_classes(archive.read(name))
    descriptors, roots, well_known = classes_art_checks(sources)
    absent = sorted(descriptors - defined)
    check(not absent, f"the boot jars lack {len(absent)} classes ART checks at start: {', '.join(absent[:12])}")
    stand_ins = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "art", "java")
    leaked = sorted(descriptor for descriptor in defined
                    if descriptor.startswith("L") and os.path.exists(os.path.join(stand_ins, descriptor[1:-1].split("$")[0] + ".java")))
    check(not leaked, f"the boot jars define compile-time stand-ins: {', '.join(leaked[:12])}")

    if failures:
        raise SystemExit(f"{failures} failures")
    print(f"{len(own)} libraries and {len(programs)} program: {symbols} undefined symbols resolved, all converted; "
          f"{len(defined)} classes in the boot jars, among them the {len(descriptors)} of ART's {roots} class roots "
          f"and {well_known} well-known classes")


if __name__ == "__main__":
    main()
