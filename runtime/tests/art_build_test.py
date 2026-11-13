#!/usr/bin/env python3
"""ART as guest code (ADR-0033 a): what tools/build-art.sh builds into <art> is complete.
  - Every library ART starts with is in <art>/lib, in the release flavor and in ART's debug flavor, and dalvikvm
    in <art>/bin; so is Conscrypt's libjavacrypto (ADR-0046 c).
  - Every DT_NEEDED is found in the set: ART's libraries, the guest system's in <bionic-lib> and the NDK's
    libc++_shared.so. Every undefined symbol that is not weak is defined, with its version, by a library of the
    closure of its DT_NEEDED, as Android's linker would look it up.
  - The guest system's libraries that ART links, libdl_android.so among them, are the ones in <bionic-lib>: ART's
    build makes none of them.
  - apk-convert converts every library and dalvikvm without a refused site.
  - The boot jars in <art>/javalib, with okhttp.jar, bouncycastle.jar and conscrypt.jar, define every class that
    ART's class roots (class_root.h) and its well-known classes (well_known_classes.cc) name, and none of the
    compile-time stand-ins of runtime/art/java.
  - bouncycastle.jar is R8's, as Soong runs it (ADR-0046 d): its classes name their source file by the mapping's
    hash, and Soong's ProGuard files have kept the members that Java serialization looks up by name. Every class
    whose name prefixes what its $Mappings registers (PREFIX = <Class>.class.getName()) is there, as R8 keeps it
    with the r8 wrapper's same-file policy.
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
# Conscrypt's JNI library, which conscrypt.jar loads (ADR-0046 c).
CONSCRYPT = ["libjavacrypto"]
# ICU's shim with the *_android names, which libsqlite and other platform libraries link instead of libicuuc and
# libicui18n, as on Android.
ICU_SHIM = ["libandroidicu"]
PROGRAMS = ["dalvikvm"]
# The guest system's libraries that ART links. Minato's bionic build makes them and ART's build none, so that the
# guest has one of each (ADR-0033 decision 2).
SYSTEM = ["libc", "libm", "libdl", "libdl_android", "ld-android", "liblog", "libz"]
JARS = ["core-oj", "core-libart", "okhttp", "bouncycastle", "core-icu4j", "conscrypt"]
# A class of bouncycastle.jar whose serialization members only Soong's ProGuard files keep, not BouncyCastle's own.
SERIALIZED = "Lcom/android/org/bouncycastle/jcajce/provider/asymmetric/rsa/BCRSAPublicKey;"
SERIALIZATION_MEMBERS = {"serialVersionUID", "readObject", "writeObject"}
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


def dex_string(data, index):
    """String `index` of a DEX file."""
    offset = struct.unpack_from("<I", data, struct.unpack_from("<I", data, 0x3C)[0] + 4 * index)[0]
    while data[offset] & 0x80:  # the ULEB128 length in UTF-16 units
        offset += 1
    end = data.index(0, offset + 1)
    return data[offset + 1:end].decode("utf-8", "replace")


def dex_classes(data):
    """The descriptors of the classes a DEX file defines."""
    type_ids_off = struct.unpack_from("<I", data, 0x44)[0]
    class_defs_size, class_defs_off = struct.unpack_from("<II", data, 0x60)
    classes = set()
    for index in range(class_defs_size):
        type_index = struct.unpack_from("<I", data, class_defs_off + 32 * index)[0]
        classes.add(dex_string(data, struct.unpack_from("<I", data, type_ids_off + 4 * type_index)[0]))
    return classes


def uleb128(data, offset):
    """The ULEB128 value at offset, and the offset after it."""
    value = shift = 0
    while data[offset] & 0x80:
        value |= (data[offset] & 0x7F) << shift
        offset, shift = offset + 1, shift + 7
    return value | data[offset] << shift, offset + 1


def dex_class(data, descriptor):
    """The source file and the names of the fields and methods of the class `descriptor` in a DEX file, or None if
    the file does not define it."""
    type_ids_off, field_ids_off, method_ids_off = (struct.unpack_from("<I", data, at)[0] for at in (0x44, 0x54, 0x5C))
    class_defs_size, class_defs_off = struct.unpack_from("<II", data, 0x60)
    for index in range(class_defs_size):
        class_def = struct.unpack_from("<8I", data, class_defs_off + 32 * index)
        class_index, source_index, data_off = class_def[0], class_def[4], class_def[6]
        if dex_string(data, struct.unpack_from("<I", data, type_ids_off + 4 * class_index)[0]) != descriptor:
            continue
        names, sizes, offset = set(), [], data_off
        if data_off:
            for _ in range(4):  # static fields, instance fields, direct methods, virtual methods
                size, offset = uleb128(data, offset)
                sizes.append(size)
        for kind, size in enumerate(sizes):
            member = 0  # each list's first index is whole, the others are differences
            for _ in range(size):
                difference, offset = uleb128(data, offset)
                _, offset = uleb128(data, offset)  # access flags
                member += difference
                if kind < 2:
                    names.add(dex_string(data, struct.unpack_from("<I", data, field_ids_off + 8 * member + 4)[0]))
                else:
                    _, offset = uleb128(data, offset)  # code offset
                    names.add(dex_string(data, struct.unpack_from("<I", data, method_ids_off + 8 * member + 4)[0]))
        return (dex_string(data, source_index) if source_index != 0xFFFFFFFF else None), names
    return None


def prefix_classes(sources):
    """The classes of BouncyCastle's sources whose $Mappings registers its algorithms under the class's own name
    (`PREFIX = <Class>.class.getName()`), as descriptors."""
    root = os.path.join(sources, "bouncycastle", "repackaged", "bcprov", "src", "main", "java")
    found = set()
    for folder, _, files in os.walk(root):
        for file in files:
            if not file.endswith(".java"):
                continue
            with open(os.path.join(folder, file), encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            if re.search(r"PREFIX\s*=\s*" + re.escape(file[:-5]) + r"\.class\.getName\(\)", text):
                found.add("L" + os.path.relpath(os.path.join(folder, file[:-5]), root) + ";")
    return found


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

    for name in RELEASE + DEBUG + CONSCRYPT + ICU_SHIM:
        check(f"{name}.so" in by_soname and by_soname[f"{name}.so"] in own, f"{name}.so is not in {art}/lib")
    shim = by_soname.get("libandroidicu.so")
    if shim is not None:
        # Every name of its map, at its version (LIBANDROIDICU_EXTERNAL_1).
        with open(os.path.join(sources, "icu", "libandroidicu", "libandroidicu.map.txt")) as handle:
            names = set(re.findall(r"^\s+(\w+_android);", handle.read(), re.MULTILINE))
        lacking = sorted(name for name in names if name not in shim.provides)
        check(names and not lacking, f"libandroidicu.so lacks {len(lacking)} of its map's {len(names)} names: "
                                     f"{', '.join(lacking[:8])}")
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
    path = os.path.join(art, "javalib", "bouncycastle.jar")
    if os.path.exists(path):
        with zipfile.ZipFile(path) as archive:
            found = [dex_class(archive.read(name), SERIALIZED) for name in archive.namelist()
                     if re.fullmatch(r"classes\d*\.dex", name)]
        entry = next((entry for entry in found if entry), None)
        check(entry is not None, f"bouncycastle.jar lacks {SERIALIZED}")
        if entry is not None:
            source, members = entry
            check(source is not None and source.startswith("go/retraceme "), f"bouncycastle.jar is not R8's as Soong "
                  f"runs it: {SERIALIZED} names its source file {source!r}, not by the mapping's hash")
            check(SERIALIZATION_MEMBERS <= members, f"bouncycastle.jar's {SERIALIZED} lacks "
                  f"{', '.join(sorted(SERIALIZATION_MEMBERS - members))}, which Soong's ProGuard files keep")
        with zipfile.ZipFile(path) as archive:
            own_classes = set().union(*(dex_classes(archive.read(name)) for name in archive.namelist()
                                        if re.fullmatch(r"classes\d*\.dex", name)))
        merged = sorted(descriptor for descriptor in prefix_classes(sources)
                        if descriptor[:-1] + "$Mappings;" in own_classes and descriptor not in own_classes)
        check(not merged, f"bouncycastle.jar lacks {len(merged)} classes whose name prefixes what their $Mappings "
              f"registers (R8 merged them away without the r8 wrapper's same-file policy): {', '.join(merged[:6])}")

    if failures:
        raise SystemExit(f"{failures} failures")
    print(f"{len(own)} libraries and {len(programs)} program: {symbols} undefined symbols resolved, all converted; "
          f"{len(defined)} classes in the boot jars, among them the {len(descriptors)} of ART's {roots} class roots "
          f"and {well_known} well-known classes")


if __name__ == "__main__":
    main()
