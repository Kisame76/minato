#!/usr/bin/env python3
"""The guest's export table (ADR-0055 decision 3, tools/export-table.py): for the built bionic libraries, each block's
names are exactly what llvm-nm reads from the library, with the same versions, its needs line is the library's
DT_NEEDED in order, and the stub marks are stubgen's stubs of the NDK's names; the same for the libraries of the other
directories given that exist (ART's, the front libs, hwui's, the framework's). Two small libraries built here with the
NDK's clang cover a hidden version, a default one and the base version, which no guest library has yet, DF_1_GLOBAL,
a file name that is not the soname; and what the generator refuses rather than write a wrong line: two different
libraries under one soname, a hidden base version, a defined name of version index 0, another machine, an executable
and a name with a space. Without the
NDK (ANDROID_NDK, or r30 in the Android SDK) or a built bionic the test is skipped.

Usage: export_table_test.py <export-table.py> <bionic lib directory> [<library directory> ...]
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "android"))
import stubgen  # noqa: E402

HEADER = "# minato export table 1"

VERSIONED_C = """
int current(void) { return 2; }
int old_both(void) { return 1; }
int new_both(void) { return 2; }
__asm__(".symver old_both, both@V1");
__asm__(".symver new_both, both@@V2");
int data_object = 7;
"""
VERSIONED_MAP = "V1 { global: current; both; local: *; };\nV2 { global: data_object; both; } V1;\n"
BASE_C = "int plain(void) { return 3; }\nint unlisted(void) { return 4; }\n"
BASE_MAP = "V1 { global: plain; };\n"  # unlisted keeps the base version


def run(arguments):
    result = subprocess.run(arguments, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"{os.path.basename(arguments[0])} failed: {result.stderr.strip()}")
    return result.stdout


def patched(source, target, changes):
    """A copy of `source` at `target` with bytes replaced: (offset, bytes) pairs."""
    with open(source, "rb") as handle:
        data = bytearray(handle.read())
    for offset, value in changes:
        data[offset:offset + len(value)] = value
    with open(target, "wb") as handle:
        handle.write(data)
    return target


def versym_offset(readelf, path, name):
    """Where the .gnu.version entry of the dynamic symbol `name` lies in the file."""
    section = [line.split() for line in run([readelf, "-S", "--wide", path]).splitlines() if " .gnu.version " in line]
    offset = int(section[0][section[0].index(".gnu.version") + 3], 16)
    for line in run([readelf, "--dyn-syms", "--wide", path]).splitlines():
        fields = line.split()
        if fields and fields[-1].split("@")[0] == name:
            return offset + 2 * int(fields[0].rstrip(":"))
    raise SystemExit(f"{name} is not in {path}'s dynamic symbols")


def parse(path):
    """The table's blocks: soname -> (needs, set of symbol lines). Fails on anything the format does not allow."""
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    problems = []
    if lines[0] != HEADER:
        problems.append(f"first line {lines[0]!r}")
    if lines[-1] != "":
        problems.append("no newline at the end")
    blocks, order, current, flags = {}, [], None, {}
    for number, line in enumerate(lines[1:-1], start=2):
        if line.startswith("@ "):
            fields = line[2:].split(" ")
            if len(fields) > 2 or (len(fields) == 2 and fields[1] != "global"):
                problems.append(f"line {number}: {line!r}")
            current = fields[0]
            flags[current] = fields[1:]
            if current in blocks:
                problems.append(f"line {number}: {current} twice")
            blocks[current] = [None, []]
            order.append(current)
        elif current is None:
            problems.append(f"line {number}: {line!r} before the first block")
        elif line == "needs" or line.startswith("needs "):
            if blocks[current][0] is not None:
                problems.append(f"line {number}: a second needs line")
            blocks[current][0] = line.split()[1:]
        elif line and " " not in line and not line.startswith("#"):
            if blocks[current][0] is None:
                problems.append(f"line {number}: a name before the needs line")
            blocks[current][1].append(line)
        else:
            problems.append(f"line {number}: {line!r}")
    if order != sorted(order):
        problems.append("blocks not sorted by soname")
    for soname, (_, names) in blocks.items():
        keys = [(line.rstrip("!").partition("@")[0], line.rstrip("!").partition("@")[2].lstrip("@")) for line in names]
        if keys != sorted(keys):
            problems.append(f"{soname}: names not sorted")
    return {soname: (needs, set(names)) for soname, (needs, names) in blocks.items()}, flags, problems


def nm_names(nm, path):
    """Defined dynamic symbols that linker64 binds (global or weak, also an ifunc), in nm's notation."""
    names = set()
    for line in run([nm, "-D", "--defined-only", path]).splitlines():
        kind, symbol = line.split()[-2:]
        if kind.isupper() or kind == "i":
            names.add(symbol)
    return names


def needed(readelf, path):
    return [line.split("[", 1)[1].rstrip("]") for line in run([readelf, "-d", path]).splitlines() if "(NEEDED)" in line]


def is_global(readelf, path):
    return any("(FLAGS_1)" in line and "GLOBAL" in line.split() for line in run([readelf, "-d", path]).splitlines())


def soname(readelf, path):
    for line in run([readelf, "-d", path]).splitlines():
        if "(SONAME)" in line:
            return line.split("[", 1)[1].rstrip("]")
    return os.path.basename(path)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    generator, built = sys.argv[1], sys.argv[2]
    ndk = os.environ.get("ANDROID_NDK") or os.path.expanduser("~/Library/Android/sdk/ndk/30.0.16248370")
    toolchain = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "darwin-x86_64")
    sysroot = os.path.join(toolchain, "sysroot")
    nm, readelf = os.path.join(toolchain, "bin", "llvm-nm"), os.path.join(toolchain, "bin", "llvm-readelf")
    clang = os.path.join(toolchain, "bin", "clang")
    if not os.path.exists(nm) or not os.path.exists(stubgen.stub_path(sysroot, "libandroid")):
        print(f"skipped: no NDK at {ndk}")
        return
    if not os.path.exists(os.path.join(built, "libc.so")):
        print(f"skipped: no bionic build in {built} (tools/build-bionic.sh)")
        return
    problems = []
    with tempfile.TemporaryDirectory() as scratch:
        # The two small libraries: a hidden and a default version of one name, and a library with only the base one.
        # The versioned one's file is not named after its soname; the twin is another library of the same soname.
        samples, built_samples = [], {}
        for file_name, name, source, version_map in (
                ("libsample-versioned-file.so", "libsample-versioned.so", VERSIONED_C, VERSIONED_MAP),
                ("libsample-base.so", "libsample-base.so", BASE_C, BASE_MAP),
                ("libsample-twin.so", "libsample-versioned.so", BASE_C, BASE_MAP)):
            c_file, map_file = os.path.join(scratch, file_name + ".c"), os.path.join(scratch, file_name + ".map")
            with open(c_file, "w", encoding="utf-8") as handle:
                handle.write(source)
            with open(map_file, "w", encoding="utf-8") as handle:
                handle.write(version_map)
            output = os.path.join(scratch, file_name)
            run([clang, "--target=aarch64-linux-android29", "-shared", "-fPIC", "-nostdlib", "-o", output, c_file,
                 f"-Wl,--version-script={map_file}", f"-Wl,-soname,{name}"]
                + (["-Wl,-z,global"] if file_name == "libsample-base.so" else []))
            built_samples[file_name] = output
        samples = [built_samples["libsample-versioned-file.so"], built_samples["libsample-base.so"]]
        twins = subprocess.run([sys.executable, generator, os.path.join(scratch, "twins.txt"),
                                built_samples["libsample-versioned-file.so"], built_samples["libsample-twin.so"]],
                               capture_output=True, text=True)
        if twins.returncode == 0 or "different libraries named libsample-versioned.so" not in twins.stderr:
            problems.append(f"two libraries of one soname: {twins.returncode}, {twins.stderr.strip()!r}")

        # What lld never writes, but a line could not say: each refused, with the reason.
        base = built_samples["libsample-base.so"]
        unlisted = versym_offset(readelf, base, "unlisted")
        x86 = os.path.join(scratch, "libsample-x86.so")
        run([clang, "--target=x86_64-linux-android29", "-shared", "-fPIC", "-nostdlib", "-o", x86,
             os.path.join(scratch, "libsample-base.so.c")])
        with open(base, "rb") as handle:
            spaced = os.path.join(scratch, "spaced.so")
            with open(spaced, "wb") as copy:
                copy.write(handle.read().replace(b"unlisted", b"unl sted"))
        refused = {
            "a hidden base version": (patched(base, os.path.join(scratch, "hidden-base.so"),
                                              [(unlisted, b"\x01\x80")]), "hidden base version"),
            "version index 0": (patched(base, os.path.join(scratch, "local-version.so"), [(unlisted, b"\x00\x00")]),
                                "version index 0"),
            "another machine": (x86, "no AArch64 shared library"),
            "an executable": (patched(base, os.path.join(scratch, "exec.so"), [(16, b"\x02\x00")]),
                              "no AArch64 shared library"),
            "a name with a space": (spaced, "a name the table cannot hold"),
        }
        for what, (path, reason) in refused.items():
            result = subprocess.run([sys.executable, generator, os.path.join(scratch, "refused.txt"), path],
                                    capture_output=True, text=True)
            if result.returncode == 0 or reason not in result.stderr:
                problems.append(f"{what}: {result.returncode}, {result.stderr.strip()!r}")
        directories = [built] + [directory for directory in sys.argv[3:] if os.path.isdir(directory)]
        libraries = sorted(os.path.join(directory, name) for directory in directories
                           for name in os.listdir(directory) if name.endswith(".so"))
        # libc.so twice: the same library under one soname is one block.
        table = os.path.join(scratch, "export-table.txt")
        run([sys.executable, generator, table] + libraries + samples + [os.path.join(built, "libc.so")])
        blocks, flags, format_problems = parse(table)
        problems += format_problems

        expected_samples = {
            "libsample-versioned.so": (["both@V1", "both@@V2", "current@@V1", "data_object@@V2"], []),
            "libsample-base.so": (["plain@@V1", "unlisted"], []),
        }
        for name, (names, needs) in expected_samples.items():
            if blocks.get(name) != (needs, set(names)):
                problems.append(f"{name}: {blocks.get(name)}, expected {(needs, set(names))}")
        if flags.get("libsample-base.so") != ["global"] or flags.get("libsample-versioned.so") != []:
            problems.append(f"the global flag: {flags.get('libsample-base.so')}, {flags.get('libsample-versioned.so')}")

        for path in libraries:
            name = soname(readelf, path)
            if name not in blocks:
                problems.append(f"{name}: no block")
                continue
            needs, lines = blocks[name]
            if (flags[name] == ["global"]) != is_global(readelf, path):
                problems.append(f"{name}: global flag {flags[name]}, DF_1_GLOBAL {is_global(readelf, path)}")
            if needs != needed(readelf, path):
                problems.append(f"{name}: needs {needs}, DT_NEEDED {needed(readelf, path)}")
            exported = {line.rstrip("!") for line in lines}
            from_nm = nm_names(nm, path)
            problems += [f"{name}: lacks {symbol}" for symbol in sorted(from_nm - exported)]
            problems += [f"{name}: has {symbol}, which nm does not show" for symbol in sorted(exported - from_nm)]
            # The stubs: stubgen's, from the NDK's names and its tables, as far as the library exports them
            # (libmediandk has no key strings yet, so they are missing, not stubs).
            library = name[:-len(".so")]
            stubs = set()
            if library in stubgen.LIBRARIES:
                version = stubgen.LIBRARIES[library]["version"]
                ndk_names = stubgen.stub_names(nm, sysroot, library)
                stubs = {f"{stub}@@{version}" if version else stub
                         for stub in stubgen.stubbed_names(library, ndk_names)} & exported
            marked = {line[:-1] for line in lines if line.endswith("!")}
            problems += [f"{name}: {symbol} is a stub, not marked" for symbol in sorted(stubs - marked)]
            problems += [f"{name}: {symbol} is marked, but no stub" for symbol in sorted(marked - stubs)]
        if len(blocks) != len({soname(readelf, path) for path in libraries}) + len(samples):
            problems.append(f"{len(blocks)} blocks for {len(libraries)} libraries and {len(samples)} samples")
    if problems:
        print("\n".join(problems))
        raise SystemExit(1)
    stub_count = sum(line.endswith("!") for _, lines in blocks.values() for line in lines)
    missing = [directory for directory in sys.argv[3:] if not os.path.isdir(directory)]
    print(f"export table as nm reads the libraries: {len(blocks)} blocks, {stub_count} stubs"
          + (f"; not built, so not checked: {', '.join(missing)}" if missing else ""))


if __name__ == "__main__":
    main()
