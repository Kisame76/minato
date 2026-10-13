#!/usr/bin/env python3
"""The boot jars: ART's (ADR-0033 decision 10), core-oj, core-libart and core-icu4j, and those of ADR-0046, okhttp,
bouncycastle and conscrypt, from the sources and Java resources that tools/art-plan.py takes from their Android.bp
modules into <description.json>.

As Soong compiles them against core-all, javac compiles all their sources at once, as the module java.base, with
the compile-time sources of runtime/art/java and of core-all (java.base's module-info and the lambda stubs): JDK
17, -source 17 -target 17, string concatenation inline. The classes are then split by the source file they come
from (their package and SourceFile attribute), so that each jar gets its own sources' classes and none of the
compile-time ones. D8 dexes each jar as a platform build for API 31, with all classes as its library, and keeps
records and sealed classes as the platform's own. A jar whose module optimizes (BouncyCastle's) goes through R8
instead, as Soong runs it through prebuilts/r8's wrapper (its system properties): shrunk and optimized as its
`optimize` says, with its ProGuard files and Soong's own (build/make/core/proguard.flags), against the other classes
as its library. A jar holds its dex files uncompressed and 4-byte aligned, as Android's boot jars do, and its Java
resources; every entry has the same time, so that a build makes the same bytes.

Usage: boot_jars.py <description.json> <jdk-home> <r8.jar> <output-directory> <work-directory>
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

JAVAC_FLAGS = ["-J-Xmx4g", "-source", "17", "-target", "17", "-XDstringConcat=inline", "-encoding", "UTF-8",
               "-nowarn", "-g", "--system=none", "--patch-module", "java.base=."]
D8_FLAGS = ["--release", "--min-api", "31", "--android-platform-build"]
# What Soong adds for an optimizing module (java/dex.go): source files named by the mapping's hash, as on a device.
R8_FLAGS = D8_FLAGS + ["--map-id-template", "%MAP_HASH", "--source-file-template", "go/retraceme %MAP_HASH"]
# The platform defines records and sealed classes itself: without these, D8 turns java.lang.Record into its own
# record tag, and ART, which looks java.lang.Record up at start (well_known_classes.cc), finds no such class. With
# them, records and permitted subclasses go into the dex as annotations, which ART reads.
D8_PROPERTIES = ["-Dcom.android.tools.r8.emitRecordAnnotationsInDex",
                 "-Dcom.android.tools.r8.emitPermittedSubclassesAnnotationsInDex"]
# What Soong's r8, prebuilts/r8's R8Wrapper, sets before it runs R8 in full mode: among them the same-file policy,
# which merges classes only within one source file. Without it, R8 merged BouncyCastle's DESede into PKIXCRLUtil, so
# DESede.class.getName(), the prefix of every DESede algorithm BC registers, named PKIXCRLUtil, and 30 of BC's
# services named classes that do not exist (BKS's key sealing among them). The wrapper also turns on R8's experimental
# keep annotations, which no boot jar uses.
R8_PROPERTIES = D8_PROPERTIES + ["-Dcom.android.tools.r8.disableEnqueuerDeferredTracing=1",
                                 "-Dcom.android.tools.r8.enableSameFilePolicy=1",
                                 "-Dcom.android.tools.r8.experimental.enablewhyareyounotinlining=1",
                                 "-Dcom.android.tools.r8.experimental.enableconvertchecknotnull=1",
                                 "-Dcom.android.tools.r8.enableEmptyMemberRulesToDefaultInitRuleConversion=1"]
TIMESTAMP = (2008, 1, 1, 0, 0, 0)  # soong_zip's
ALIGNMENT_EXTRA = 0xD935  # zipalign's extra field for the alignment of a stored entry
PACKAGE = re.compile(r"^package\s+([\w.]+)\s*;", re.M)


def source_key(path):
    """<package path>/<file name> of a Java source, as its classes' package and SourceFile attribute name it."""
    with open(path, encoding="utf-8", errors="replace") as handle:
        found = PACKAGE.search(handle.read())
    return (found.group(1).replace(".", "/") + "/" if found else "") + os.path.basename(path)


def class_source(data):
    """The SourceFile attribute of a class file."""
    count = struct.unpack_from(">H", data, 8)[0]
    strings, offset, index = {}, 10, 1
    while index < count:
        tag = data[offset]
        if tag == 1:  # Utf8
            length = struct.unpack_from(">H", data, offset + 1)[0]
            strings[index] = data[offset + 3:offset + 3 + length].decode("utf-8", "replace")
            offset += 3 + length
        else:
            offset += 1 + {3: 4, 4: 4, 5: 8, 6: 8, 7: 2, 8: 2, 9: 4, 10: 4, 11: 4, 12: 4, 15: 3, 16: 2, 17: 4, 18: 4,
                           19: 2, 20: 2}[tag]
        index += 2 if tag in (5, 6) else 1  # longs and doubles take two slots
    offset += 6
    interfaces = struct.unpack_from(">H", data, offset)[0]
    offset += 2 + 2 * interfaces
    for _ in range(2):  # fields, then methods
        members = struct.unpack_from(">H", data, offset)[0]
        offset += 2
        for _ in range(members):
            attributes = struct.unpack_from(">H", data, offset + 6)[0]
            offset += 8
            for _ in range(attributes):
                offset += 6 + struct.unpack_from(">I", data, offset + 2)[0]
    attributes = struct.unpack_from(">H", data, offset)[0]
    offset += 2
    for _ in range(attributes):
        name, length = struct.unpack_from(">HI", data, offset)
        if strings.get(name) == "SourceFile":
            return strings[struct.unpack_from(">H", data, offset + 6)[0]]
        offset += 6 + length
    return None


def write_jar(path, entries):
    """A jar of (name, bytes) entries in name order: dex files stored and 4-byte aligned, the rest deflated."""
    temporary = path + ".tmp"
    with zipfile.ZipFile(temporary, "w") as jar:
        for name, data in sorted(entries):
            info = zipfile.ZipInfo(name, TIMESTAMP)
            info.external_attr = 0o644 << 16
            if name.endswith(".dex"):
                info.compress_type = zipfile.ZIP_STORED
                start = jar.fp.tell() + 30 + len(name.encode()) + 6
                padding = -start % 4
                info.extra = struct.pack("<HHH", ALIGNMENT_EXTRA, 2 + padding, 4) + bytes(padding)
            else:
                info.compress_type = zipfile.ZIP_DEFLATED
            jar.writestr(info, data)
    os.replace(temporary, path)


def shrink(java, r8, work, name, settings, own, program, dexed):
    """R8 over the jar's classes `program` into `dexed`, as Soong runs it for a module with `optimize`: its ProGuard
    files, then -dontshrink, -dontoptimize, -dontobfuscate and -ignorewarnings as `settings` says, and the mapping,
    without which R8 has no hash to name the source files by. The library is every other class: R8 takes no program
    class twice."""
    owned = {relative for relative, _ in own}
    library = os.path.join(work, f"{name}.library.jar")
    with zipfile.ZipFile(os.path.join(work, "all-classes.jar")) as source, zipfile.ZipFile(library, "w") as target:
        for entry in source.namelist():
            if entry not in owned:
                target.writestr(entry, source.read(entry))
    switches = os.path.join(work, f"{name}.r8.flags")
    with open(switches, "w") as handle:
        for setting, switch in (("shrink", "-dontshrink"), ("optimize", "-dontoptimize"),
                                ("obfuscate", "-dontobfuscate")):
            if not settings[setting]:
                handle.write(switch + "\n")
        if settings["ignore_warnings"]:
            handle.write("-ignorewarnings\n")
    configuration = [argument for path in settings["flags"] + [switches] for argument in ("--pg-conf", path)]
    subprocess.run([java, "-Xmx4g"] + R8_PROPERTIES + ["-cp", r8, "com.android.tools.r8.R8"] + R8_FLAGS
                   + ["--lib", library] + configuration
                   + ["--pg-map-output", os.path.join(work, f"{name}.map"), "--output", dexed, program], check=True)


def main():
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)
    description, jdk, r8, output, work = sys.argv[1:]
    with open(description) as handle:
        jars = json.load(handle)
    shutil.rmtree(work, ignore_errors=True)
    classes = os.path.join(work, "classes")
    os.makedirs(classes)

    sources = [path for jar in jars["jars"].values() for path in jar["sources"]] + jars["compile_only"]
    with open(os.path.join(work, "sources.args"), "w") as handle:
        handle.write("\n".join(sources) + "\n")
    subprocess.run([os.path.join(jdk, "bin", "javac")] + JAVAC_FLAGS + ["-d", classes, "@" + os.path.join(work, "sources.args")],
                   check=True)

    owner = {}
    for name, jar in jars["jars"].items():
        for path in jar["sources"]:
            owner[source_key(path)] = name
    compile_only = {source_key(path) for path in jars["compile_only"]}
    split = {name: [] for name in jars["jars"]}
    for directory, _, files in os.walk(classes):
        for file in files:
            path = os.path.join(directory, file)
            relative = os.path.relpath(path, classes)
            if relative == "module-info.class":
                continue
            with open(path, "rb") as handle:
                data = handle.read()
            key = os.path.dirname(relative) + "/" + (class_source(data) or "")
            if key in owner:
                split[owner[key]].append((relative, data))
            elif key not in compile_only:
                raise SystemExit(f"boot_jars: {relative} comes from {key}, which no jar and no compile-time source has")

    with zipfile.ZipFile(os.path.join(work, "all-classes.jar"), "w") as library:
        for directory, _, files in os.walk(classes):
            for file in files:
                path = os.path.join(directory, file)
                library.write(path, os.path.relpath(path, classes))
    os.makedirs(output, exist_ok=True)
    java = os.path.join(jdk, "bin", "java")
    for name, jar in jars["jars"].items():
        program = os.path.join(work, f"{name}.classes.jar")
        with zipfile.ZipFile(program, "w") as handle:
            for relative, data in split[name]:
                handle.writestr(relative, data)
        dexed = os.path.join(work, f"{name}.dex.jar")
        if "r8" in jar:
            shrink(java, r8, work, name, jar["r8"], split[name], program, dexed)
        else:
            subprocess.run([java, "-Xmx4g"] + D8_PROPERTIES + ["-cp", r8, "com.android.tools.r8.D8"] + D8_FLAGS
                           + ["--lib", os.path.join(work, "all-classes.jar"), "--output", dexed, program], check=True)
        entries = []
        with zipfile.ZipFile(dexed) as handle:
            entries += [(entry, handle.read(entry)) for entry in handle.namelist() if entry.endswith(".dex")]
        for path, entry in jar["resources"]:
            with open(path, "rb") as handle:
                entries.append((entry, handle.read()))
        write_jar(os.path.join(output, f"{name}.jar"), entries)
        print(f"boot_jars: {name}.jar: {len(split[name])} classes, {len(jar['resources'])} resources")


if __name__ == "__main__":
    main()
