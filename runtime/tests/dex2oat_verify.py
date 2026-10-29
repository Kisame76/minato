#!/usr/bin/env python3
"""dex2oat makes a verify oat of a Java program (M6 c1, test 2): apk-run runs dex2oat as
/apex/com.android.art/bin/dex2oat64 imageless, with the options apk-runner-35's dex2oat64 took for the oat of
fixtures/oat (--compiler-filter=verify, --base=0x70000000, our boot jars as -Xbootclasspath), on <jar>. It must exit 0
and write the odex and the vdex. The odex must be what ART loads (OatHeader "oat\\n244" at oatdata, 0x4000, a .dex
range at oatdex, 0x8000), carry the key-value store of a verify oat of an imageless build (compiler-filter verify,
requires-image false, our boot class path, and its checksums, which DexFileLoader::GetMultiDexChecksum computes from
the boot jars' zip entries, so ART accepts the odex for a run with these boot jars), and oat_test must convert it into
a container that loads (importer/converter/tests/oat_test.cpp). --debug runs dex2oatd with ART's debug flavor.
Prints "skipped: ..." while ART, the jar or the guest system's files are missing.

Usage: dex2oat_verify.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <jar> <oat-test>
                         [--debug]
"""
import os
import re
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import art_run  # noqa: E402

OATDATA = 0x4000
OATDEX = 0x8000


def key_values(odex):
    """The key-value store of the oat header as a dict: the NUL-separated key and value strings that follow the
    header, found by the keys that dex2oat writes (the store has no marker of its own that is stable to search)."""
    values = {}
    for key in (b"bootclasspath", b"bootclasspath-checksums", b"classpath", b"compiler-filter", b"debuggable",
                b"native-debuggable", b"requires-image"):
        match = re.search(re.escape(b"\0" + key + b"\0") + rb"([^\0]*)\0", odex[OATDATA:OATDATA + 0x4000])
        if match:
            values[key.decode()] = match.group(1).decode()
    return values


def dex_checksum(jar):
    """The checksum the oat's list holds for `jar`: DexFileLoader::GetMultiDexChecksum, the CRC-32 of each zip entry
    classes.dex, classes2.dex, ... xor-ed onto kEmptyMultiDexChecksum (1)."""
    checksum = 1
    with zipfile.ZipFile(jar) as archive:
        for info in archive.infolist():
            if re.fullmatch(r"classes\d*\.dex", info.filename):
                checksum ^= info.CRC
    return f"{checksum:08x}"


def check_odex(odex, art):
    """Why `odex` (bytes) is not a verify oat of an imageless build with our boot jars, or None."""
    if odex[OATDATA:OATDATA + 8] != b"oat\n244\0":
        return f"no OatHeader \"oat\\n244\" at {OATDATA:#x}"
    if len(odex) < OATDEX:
        return "the odex ends before its .dex range"
    values = key_values(odex)
    jars = [f"{art_run.ART_ROOT}/javalib/{name}.jar" for name in art_run.BOOT_JARS]
    wanted = {"compiler-filter": "verify", "requires-image": "false", "debuggable": "false",
              "native-debuggable": "false", "bootclasspath": ":".join(jars),
              "bootclasspath-checksums": ":".join("d/" + dex_checksum(os.path.join(art, "javalib", f"{name}.jar"))
                                                  for name in art_run.BOOT_JARS)}
    for key, value in wanted.items():
        if values.get(key) != value:
            return f"the oat's {key} is {values.get(key)!r}, not {value!r}"
    return None


def main():
    arguments = [argument for argument in sys.argv[1:] if argument != "--debug"]
    debug = "--debug" in sys.argv[1:]
    if len(arguments) != 9:
        raise SystemExit(__doc__)
    art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, jar, oat_test = arguments
    program = "dex2oatd" if debug else "dex2oat"
    for path in (os.path.join(art, "bin", "dalvikvm"), jar, oat_test):
        if not os.path.exists(path):
            print(f"skipped: {path} is not built")
            return 0
    if not os.path.exists(os.path.join(art, "bin", program)):
        print(f"{art}/bin/{program} is not built: tools/build-art.sh must build it")
        return 1
    missing = art_run.missing_front_lib(bionic_lib)
    if missing:
        print(missing)
        return 0
    work = os.path.abspath(work)
    art_lib64 = art_run.convert_guest(art, bionic_lib, apk_convert, work, debug)
    container = os.path.join(work, "bin", f"{program}-64.dylib")
    with art_run.convert_lock(work):
        art_run.convert(apk_convert, os.path.join(art, "bin", program), container)
    run = os.path.join(work, "dex2oat-verify-run-debug" if debug else "dex2oat-verify-run")
    art_run.make_run_folder(run, icu_data, bionic_system)
    shutil.copy(jar, os.path.join(run, "data", "local", "tmp", "app.jar"))
    boot_class_path = ":".join(f"{art_run.ART_ROOT}/javalib/{name}.jar" for name in art_run.BOOT_JARS)
    arguments = ["--dex-file=/data/local/tmp/app.jar", "--dex-location=/data/local/tmp/app.jar",
                 "--oat-file=/data/local/tmp/app.odex", "--oat-location=/data/local/tmp/app.odex",
                 "--instruction-set=arm64", "--compiler-filter=verify", "--base=0x70000000",
                 "--runtime-arg", f"-Xbootclasspath:{boot_class_path}",
                 "--runtime-arg", f"-Xbootclasspath-locations:{boot_class_path}", "--runtime-arg", "-Xnoimage-dex2oat"]
    command = art_run.apk_run_command(apk_run, art, bionic_system, work, art_lib64, run, arguments)
    at = command.index("--argv0")
    command[at + 1:at + 3] = [f"{art_run.ART_ROOT}/bin/dex2oat64", container]
    result = subprocess.run(command, capture_output=True, text=True, timeout=600, stdin=subprocess.DEVNULL)
    produced = os.path.join(run, "data", "local", "tmp")
    odex_path = os.path.join(produced, "app.odex")
    if result.returncode != 0 or not os.path.exists(odex_path) or os.path.getsize(odex_path) == 0 \
            or not os.path.exists(os.path.join(produced, "app.vdex")) \
            or os.path.getsize(os.path.join(produced, "app.vdex")) == 0:
        print(f"{program} exited with {result.returncode} and wrote no odex and vdex\n{result.stderr[-3000:]}")
        return 1
    with open(odex_path, "rb") as handle:
        failure = check_odex(handle.read(), art)
    if failure:
        print(f"{program}'s odex: {failure}")
        return 1
    converted = subprocess.run([oat_test, os.path.join(work, "oat-test-debug" if debug else "oat-test"), odex_path],
                               capture_output=True, text=True, timeout=300)
    if converted.returncode != 0:
        print(f"oat_test does not accept {program}'s odex:\n{converted.stdout}{converted.stderr}")
        return 1
    print(f"{program} writes a verify oat that ART accepts for these boot jars and that converts and loads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
