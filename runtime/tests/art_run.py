#!/usr/bin/env python3
"""A Java program on ART as guest code (ADR-0033 b, tests 1 and 2): apk-run runs dalvikvm as
/apex/com.android.art/bin/dalvikvm64, the way `dalvikvm64 -Xint -cp <jar> <class>` runs on Android 15, with the
files of ADR-0033 decision 9 where Android has them:
  - ART's libraries (<art>/lib) in /apex/com.android.art/lib64, the boot jars (<art>/javalib) in
    /apex/com.android.art/javalib;
  - bionic's libraries in /system/lib64, and bionic's /system;
  - ICU's data in /apex/com.android.i18n/etc/icu, bionic's time zone file in /apex/com.android.tzdata/etc/tz;
  - /data with an empty dalvik-cache for arm64, and the program's jar in /data/local/tmp, read-only.
The options are those of decision 11: the interpreter only, no boot image, the three boot jars. The program must
print exactly <expected> on stdout and exit with 0. What ART logs goes to stderr, which is shown when the run fails.
--debug runs ART's debug flavor (libartd.so, under libart.so's name) with -Xcheck:jni.
--library puts the program's JNI library next to its jar, in /data/local/tmp, which -Djava.library.path names, as
on Android 15.
--offsets checks the last line of stderr, "<program> offsets: lowest 0x..., highest 0x...", which codec-test writes
(ADR-0033 test 2): the window is filled from the top, so the lowest offset lies at or above 2^31 and the highest in
the last MiB below the window's last page, which is never mapped.
Prints "skipped: ..." while dalvikvm, the jar or the library is missing, or the guest system lacks the public
libraries' list that libnativeloader reads at start (tools/build-bionic.sh).

Usage: art_run.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <jar> <class>
                  <expected> [--debug] [--library <lib.so>] [--offsets]
"""
import fcntl
import os
import re
import shutil
import subprocess
import sys

ART_ROOT = "/apex/com.android.art"
BOOT_JARS = ["core-oj", "core-libart", "core-icu4j"]
# The heap window of runtime/art/patches/0002-heap-window.patch: 4 GiB, whose last 16 KiB page is never mapped.
WINDOW_TOP = (1 << 32) - 16 * 1024


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def convert_directory(apk_convert, source, target, leave_out=()):
    os.makedirs(target, exist_ok=True)
    for name in sorted(os.listdir(source)):
        if name.endswith(".so") and name not in leave_out:
            convert(apk_convert, os.path.join(source, name), os.path.join(target, name + ".dylib"))


def offsets_failure(stderr):
    """Why the offsets on stderr's last line do not show a window filled from the top, or None when they do."""
    lines = stderr.splitlines()
    match = re.fullmatch(r"\S+ offsets: lowest 0x([0-9a-f]{8}), highest 0x([0-9a-f]{8})", lines[-1] if lines else "")
    if not match:
        return "stderr does not end with the offsets"
    lowest, highest = (int(value, 16) for value in match.groups())
    if lowest < 1 << 31:
        return f"the lowest offset 0x{lowest:08x} lies below 2^31"
    if not WINDOW_TOP - (1 << 20) <= highest < WINDOW_TOP:
        return f"the highest offset 0x{highest:08x} lies outside the last MiB below 0x{WINDOW_TOP:08x}"
    return None


def main():
    arguments = sys.argv[1:]
    debug = "--debug" in arguments
    offsets = "--offsets" in arguments
    arguments = [argument for argument in arguments if argument not in ("--debug", "--offsets")]
    library = None
    if "--library" in arguments:
        at = arguments.index("--library")
        library = arguments[at + 1] if at + 1 < len(arguments) else None
        del arguments[at:at + 2]
    if len(arguments) != 10 or ("--library" in sys.argv and library is None):
        raise SystemExit(__doc__)
    art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, jar, main_class, expected = arguments
    dalvikvm = os.path.join(art, "bin", "dalvikvm")
    for needed in (dalvikvm, jar):
        if not os.path.exists(needed):
            print(f"skipped: {needed} is not built (tools/build-art.sh)")
            return
    if library is not None and not os.path.exists(library):
        print(f"skipped: {library} is not built (tools/build-fixtures.sh)")
        return
    public_libraries = os.path.join(bionic_system, "etc", "public.libraries.txt")
    if not os.path.exists(public_libraries):
        print(f"skipped: {public_libraries} is not built (tools/build-bionic.sh)")
        return

    # The containers stay from run to run, and the runs that CTest starts side by side share them, one converting
    # at a time; the files the program sees are new for each run. dalvikvm opens libart.so: JniInvocation takes
    # -XXlib only where ro.debuggable is 1, and the guest has no property area yet (ADR-0039 a). So the debug run
    # finds ART's debug flavor under that name.
    art_lib64 = os.path.join(work, "art-lib64-debug" if debug else "art-lib64")
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        convert_directory(apk_convert, os.path.join(art, "lib"), art_lib64,
                          leave_out=("libart.so",) if debug else ())
        if debug:
            convert(apk_convert, os.path.join(art, "lib", "libartd.so"), os.path.join(art_lib64, "libart.so.dylib"))
        convert_directory(apk_convert, bionic_lib, os.path.join(work, "system-lib64"))
        os.makedirs(os.path.join(work, "bin"), exist_ok=True)
        convert(apk_convert, dalvikvm, os.path.join(work, "bin", "dalvikvm64.dylib"))
        if library is not None:
            os.makedirs(os.path.join(work, "jni"), exist_ok=True)
            convert(apk_convert, library, os.path.join(work, "jni", os.path.basename(library) + ".dylib"))
    run = os.path.join(work, os.path.splitext(os.path.basename(jar))[0] + ("-debug" if debug else ""))
    shutil.rmtree(run, ignore_errors=True)
    # /apex as Android has it, down to the APEXes' own folders, which ART and ICU look for: ART's javalib is a
    # mount of its own, its lib64 and bin are the loader's.
    for folder in ("data/dalvik-cache/arm64", "data/local/tmp", "apex/com.android.art/javalib",
                   "apex/com.android.art/lib64", "apex/com.android.art/bin", "apex/com.android.i18n/etc/icu",
                   "apex/com.android.tzdata/etc/tz"):
        os.makedirs(os.path.join(run, folder))
    # Read-only, as an app's dex must be: at SDK 34 and later ART refuses a writable one to every uid but root,
    # system and shell (dalvik_system_DexFile.cc), and the guest runs as the app uid; the reference ran as shell.
    copied = shutil.copy(jar, os.path.join(run, "data", "local", "tmp"))
    os.chmod(copied, 0o444)
    if library is not None:
        shutil.copy(library, os.path.join(run, "data", "local", "tmp"))
    for source, target in ((icu_data, f"apex/com.android.i18n/etc/icu/{os.path.basename(icu_data)}"),
                           (os.path.join(bionic_system, "usr", "share", "zoneinfo", "tzdata"),
                            "apex/com.android.tzdata/etc/tz/tzdata")):
        os.link(source, os.path.join(run, target))  # a file of its own, as the guest sees it, not a link out

    boot_class_path = ":".join(f"{ART_ROOT}/javalib/{name}.jar" for name in BOOT_JARS)
    options = ["-Xint", "-Xusejit:false", "-Xnoimage-dex2oat", f"-Xbootclasspath:{boot_class_path}",
               f"-Xbootclasspath-locations:{boot_class_path}", "-XjdwpProvider:none"]
    if debug:
        options = ["-Xcheck:jni"] + options
    jni = []
    if library is not None:
        options.append("-Djava.library.path=/data/local/tmp")
        jni = ["--libraries", f"/data/local/tmp={os.path.join(work, 'jni')}"]
    command = [apk_run,
               "--mount", f"/data={os.path.join(run, 'data')}",
               "--mount", f"/apex={os.path.join(run, 'apex')}",
               "--mount", f"{ART_ROOT}/javalib={os.path.join(art, 'javalib')}",
               "--mount", f"/system={bionic_system}",
               "--libraries", f"/system/lib64={os.path.join(work, 'system-lib64')}",
               "--libraries", f"{ART_ROOT}/lib64={art_lib64}"] + jni + [
               "--cwd", "/", "--argv0", f"{ART_ROOT}/bin/dalvikvm64",
               os.path.join(work, "bin", "dalvikvm64.dylib")] + options + [
               "-cp", f"/data/local/tmp/{os.path.basename(jar)}", main_class]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=300)
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL: no end within {timeout.timeout} s")
        sys.exit(1)
    with open(expected) as handle:
        wanted = handle.read()
    failure = offsets_failure(result.stderr) if offsets else None
    if result.returncode == 0 and result.stdout == wanted and failure is None:
        print(f"{os.path.basename(jar)} printed what Android 15 prints" + (" (debug flavor)" if debug else "") +
              (f"; {result.stderr.splitlines()[-1]}" if offsets else ""))
        return
    print(f"FAIL: exit status {result.returncode}" + (f"; {failure}" if failure else ""))
    print(f"stdout: [{result.stdout}]")
    print(f"expected: [{wanted}]")
    print("stderr, last 60 lines:")
    print("\n".join(result.stderr.splitlines()[-60:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
