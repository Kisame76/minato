#!/usr/bin/env python3
"""The guest's libsqlite.so test on the Mac: sqlite-native-test
(framework/tests/native/sqlite_native_test.c) opens the library with dlopen in the guest on our loader and prints what
SQLite and Android's functions in it answer, which must be Android 15's (fixtures/expected/sqlite-native-test.txt).

apk-run runs it as a program of /data/local/tmp, a folder of the run where it makes its database, with bionic's
libraries and libsqlite.so in /system/lib64, libandroidicu and ICU's libraries with ART's in
/apex/com.android.art/lib64, where Minato keeps them (ADR-0040 decision 2), and ICU's data in
/apex/com.android.i18n/etc/icu. Prints "skipped: ..." while a part
is not built; a missing libsqlite.so is the test's own failure.

Usage: sqlite_native_run.py <apk-convert> <apk-run> <bionic-lib> <bionic-system> <art> <icu-data> <work>
                            <sqlite-native-test> <libsqlite.so> <expected>
"""
import os
import shutil
import subprocess
import sys

PREFIX = "sqlite-native-test: "
# libandroidicu, ICU's libraries under it and what they need of ART's set.
ICU_LIBRARIES = ("libandroidicu.so", "libicuuc.so", "libicui18n.so", "libbase.so", "libc++_shared.so")


def convert(apk_convert, elf, container):
    """The container of `elf`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    os.makedirs(os.path.dirname(container), exist_ok=True)
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def expected_lines(path):
    with open(path, encoding="utf-8") as handle:
        return [line for line in handle.read().splitlines() if line and not line.startswith("#")]


def main():
    if len(sys.argv) != 11:
        raise SystemExit(__doc__)
    (apk_convert, apk_run, bionic_lib, bionic_system, art, icu_data, work, program, sqlite,
     expected) = sys.argv[1:]
    for needed, how in ((program, "tools/build-framework-native.sh"), (icu_data, "tools/fetch-sources.py --set art"),
                        *((os.path.join(art, "lib", name), "tools/build-art.sh") for name in ICU_LIBRARIES),
                        (os.path.join(bionic_lib, "libc.so"), "tools/build-bionic.sh")):
        if not os.path.exists(needed):
            print(f"skipped: {needed} is not built ({how})")
            return

    system_lib64 = os.path.join(work, "system-lib64")
    for name in sorted(os.listdir(bionic_lib)):
        if name.endswith(".so"):
            convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system_lib64, name + ".dylib"))
    # Without the library, the program's dlopen names it missing, as the guest has no other.
    library = os.path.join(system_lib64, "libsqlite.so.dylib")
    if os.path.exists(sqlite):
        convert(apk_convert, sqlite, library)
    elif os.path.exists(library):
        os.remove(library)
    art_lib64 = os.path.join(work, "art-lib64")
    for name in ICU_LIBRARIES:
        convert(apk_convert, os.path.join(art, "lib", name), os.path.join(art_lib64, name + ".dylib"))
    container = os.path.join(work, "bin", "sqlite-native-test.dylib")
    convert(apk_convert, program, container)

    # ICU's data: the run's own read-only copy, made again only when the source is newer, so that nothing the guest
    # does reaches the shared file; each run links it into its /apex.
    data = os.path.join(work, "icu", os.path.basename(icu_data))
    if not os.path.exists(data) or os.path.getmtime(data) < os.path.getmtime(icu_data):
        os.makedirs(os.path.dirname(data), exist_ok=True)
        if os.path.exists(data):
            os.remove(data)
        shutil.copyfile(icu_data, data)
        os.chmod(data, 0o444)
    run = os.path.join(work, "run")
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(os.path.join(run, "apex", "com.android.i18n", "etc", "icu"))
    os.makedirs(os.path.join(run, "tmp"))
    os.link(data, os.path.join(run, "apex", "com.android.i18n", "etc", "icu", os.path.basename(icu_data)))

    command = [apk_run,
               "--mount", f"/system={bionic_system}",
               "--mount", f"/apex={os.path.join(run, 'apex')}",
               "--mount", f"/data/local/tmp={os.path.join(run, 'tmp')}",
               "--libraries", f"/system/lib64={system_lib64}",
               "--libraries", f"/apex/com.android.art/lib64={art_lib64}",
               "--cwd", "/data/local/tmp", "--argv0", "/data/local/tmp/sqlite-native-test",
               container]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=300)
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL: no end within {timeout.timeout} s")
        sys.exit(1)
    got = [line[len(PREFIX):] for line in result.stdout.splitlines() if line.startswith(PREFIX)]
    wanted = expected_lines(expected)
    if result.returncode == 0 and got == wanted:
        print(f"sqlite-native-test answered as Android 15's libsqlite.so, {len(got)} lines")
        return
    print(f"FAIL: exit status {result.returncode}")
    for line in wanted:
        if line not in got:
            print(f"  only Android's: {line}")
    for line in got:
        if line not in wanted:
            print(f"  only ours:      {line}")
    if got != wanted and sorted(got) == sorted(wanted):
        print("  the same lines in another order")
    print("stderr, last 40 lines:")
    print("\n".join(result.stderr.splitlines()[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
