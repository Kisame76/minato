#!/usr/bin/env python3
"""A Java program on ART as guest code (ADR-0033 b, tests 1 and 2): apk-run runs dalvikvm as
/apex/com.android.art/bin/dalvikvm64, the way `dalvikvm64 -Xint -cp <jar> <class>` runs on Android 15, with the
files of ADR-0033 decision 9 where Android has them:
  - ART's libraries (<art>/lib) in /apex/com.android.art/lib64, the boot jars (<art>/javalib) in
    /apex/com.android.art/javalib;
  - bionic's libraries and the graphics front libs (tools/build-front-libs.sh) in /system/lib64, and bionic's
    /system: the front libs are public libraries, which libnativeloader preloads at every VM start (ADR-0040
    decision 3);
  - ICU's data in /apex/com.android.i18n/etc/icu, bionic's time zone file in /apex/com.android.tzdata/etc/tz;
  - /data with an empty dalvik-cache for arm64, and the program's jar in /data/local/tmp, read-only.
The options are those of decision 11: the interpreter only, no boot image, the three boot jars. The program must
print exactly <expected> on stdout and exit with 0. What ART logs goes to stderr, which is shown when the run fails.
--debug runs ART's debug flavor (libartd.so, under libart.so's name) with -Xcheck:jni.
--library puts the program's JNI library next to its jar, in /data/local/tmp, which -Djava.library.path names, as
on Android 15.
--offsets checks the last line of stderr, "<program> offsets: lowest 0x..., highest 0x...", which codec-test writes
(ADR-0033 test 2): the window is filled from the top, so the lowest offset lies at or above 2^31 and the highest in
the last MiB below the window's last page, which is never mapped. It also checks that ART's log names the window
once, with all 4 GiB. --upper-half sets APKRT_HEAP_WINDOW_UPPER_HALF, the test switch of patch 0002, so that ART
reserves only the window's upper 2 GiB, as without increased-memory-limit on iOS, and the log must name those.
--conscrypt adds Conscrypt's APEX as Android 15 has it (ADR-0046 c): conscrypt.jar (<art>/javalib) in
/apex/com.android.conscrypt/javalib, after the boot jars, libjavacrypto.so (<art>/lib) in its lib64, never in ART's,
and the CA certificates of build/sources/ca-certificates in its cacerts, where the runtime's link
/system/etc/security/cacerts leads (ADR-0046 decision 4). --okhttp and --bouncycastle add okhttp.jar and
bouncycastle.jar from ART's javalib after core-libart, in that order, as Android 15 has them (ADR-0046 d).
--nativeloader checks libnativeloader's lines that name a namespace, where it loads each JNI library and which
namespace it configures for a class loader: they must be Android 15's lines of logcat in <reference>, which
tools/android-reference.sh --art writes (ADR-0040 test 4).
--nterp runs nterp, ART's default interpreter, where --nterp is absent -Xint runs the switch interpreter (M6 a).
--stress checks the line "stress-test stats: ..." that stress-test writes to stderr (ADR-0033 test 4): at least one
of its signals must have been handled while the main thread collected, so that the run covered a signal handler
during ART's stop-the-world.
--gc <collector> checks that ART's start line, "Using CollectorType<collector> GC.", names that collector, as
app_process runs an app with CMS (ADR-0033 decision 5); the run gets a folder of its own.
Prints "skipped: ..." while dalvikvm, the jar, the library, Conscrypt's files, the added jars or the front libs are
missing, or the guest system lacks the public libraries' list that libnativeloader reads at start
(tools/build-bionic.sh).

Usage: art_run.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <jar> <class>
                  <expected> [--debug] [--library <lib.so>] [--offsets] [--upper-half] [--conscrypt] [--okhttp]
                  [--bouncycastle] [--nativeloader <reference>] [--stress] [--gc <collector>]
"""
import contextlib
import fcntl
import os
import re
import shutil
import subprocess
import sys

ART_ROOT = "/apex/com.android.art"
BOOT_JARS = ["core-oj", "core-libart", "core-icu4j"]
CONSCRYPT_ROOT = "/apex/com.android.conscrypt"
CONSCRYPT_LIBRARY = "libjavacrypto.so"
# Conscrypt's CA certificates (ADR-0046 decision 4), the guest's /apex/com.android.conscrypt/cacerts.
CA_CERTIFICATES = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build",
                                               "sources", "ca-certificates", "files"))
# The heap window of runtime/art/patches/0002-heap-window.patch: 4 GiB, whose last 16 KiB page is never mapped.
WINDOW_TOP = (1 << 32) - 16 * 1024
# libnativeloader's lines that name a namespace, as logcat -v tag shows them: which namespace a JNI library loads
# in, and which namespace it configures for a class loader (ADR-0040 test 4).
NATIVELOADER_LINE = re.compile(r"[VDIWEF]/nativeloader: (Load .* ns |Configuring )")


# The graphics front libs: public libraries, which libnativeloader preloads at every VM start and without which the
# start aborts (ADR-0040 decision 3), as Android's /system/lib64 has them.
FRONT_LIBS = ("libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so")


def front_libs(bionic_lib):
    """The folder of the graphics front libs: tools/build-front-libs.sh's, which lies beside bionic's build."""
    return os.path.normpath(os.path.join(bionic_lib, "..", "..", "graphics", "guest", "bin"))


def missing_front_lib(bionic_lib):
    """The first graphics front lib that is not built, as "skipped: ..." text, or None."""
    for name in FRONT_LIBS:
        path = os.path.join(front_libs(bionic_lib), name)
        if not os.path.exists(path):
            return f"skipped: {path} is not built (tools/build-front-libs.sh)"
    return None


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


@contextlib.contextmanager
def convert_lock(work):
    """One conversion into `work` at a time, among all the runs that CTest starts side by side: they share the
    containers, and apk-convert writes each one through <container>.tmp."""
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def convert_guest(art, bionic_lib, apk_convert, work, debug, library=None):
    """Converts ART's libraries, bionic's, the front libs, dalvikvm and the JNI library `library`, if given, into
    containers in `work`; returns the folder of ART's. The containers stay from run to run, and the runs that CTest
    starts side by side share them, one converting at a time. dalvikvm opens libart.so: JniInvocation takes -XXlib
    only where ro.debuggable is 1, and the guest has no property area yet (ADR-0039 a). So the debug flavor's folder
    holds ART's debug flavor under that name. Conscrypt's library goes to a folder of its own, conscrypt-lib64."""
    art_lib64 = os.path.join(work, "art-lib64-debug" if debug else "art-lib64")
    with convert_lock(work):
        convert_directory(apk_convert, os.path.join(art, "lib"), art_lib64,
                          leave_out=(CONSCRYPT_LIBRARY,) + (("libart.so",) if debug else ()))
        if debug:
            convert(apk_convert, os.path.join(art, "lib", "libartd.so"), os.path.join(art_lib64, "libart.so.dylib"))
        conscrypt_library = os.path.join(art, "lib", CONSCRYPT_LIBRARY)
        if os.path.exists(conscrypt_library):
            os.makedirs(os.path.join(work, "conscrypt-lib64"), exist_ok=True)
            convert(apk_convert, conscrypt_library,
                    os.path.join(work, "conscrypt-lib64", CONSCRYPT_LIBRARY + ".dylib"))
        convert_directory(apk_convert, bionic_lib, os.path.join(work, "system-lib64"))
        for name in FRONT_LIBS:
            convert(apk_convert, os.path.join(front_libs(bionic_lib), name),
                    os.path.join(work, "system-lib64", name + ".dylib"))
        os.makedirs(os.path.join(work, "bin"), exist_ok=True)
        convert(apk_convert, os.path.join(art, "bin", "dalvikvm"), os.path.join(work, "bin", "dalvikvm64.dylib"))
        if library is not None:
            os.makedirs(os.path.join(work, "jni"), exist_ok=True)
            convert(apk_convert, library, os.path.join(work, "jni", os.path.basename(library) + ".dylib"))
    return art_lib64


def make_run_folder(run, icu_data, bionic_system, conscrypt_jar=None):
    """A new folder `run` for the files a run sees: /data with an empty dalvik-cache for arm64 and /data/local/tmp,
    and /apex as Android has it, down to the APEXes' own folders, which ART and ICU look for. ART's javalib is a
    mount of its own, its lib64 and bin are the loader's; so is Conscrypt's lib64, its cacerts a mount of the CA
    certificates, and `conscrypt_jar`, if given, lies in Conscrypt's javalib."""
    shutil.rmtree(run, ignore_errors=True)
    folders = ["data/dalvik-cache/arm64", "data/local/tmp", "apex/com.android.art/javalib",
               "apex/com.android.art/lib64", "apex/com.android.art/bin", "apex/com.android.i18n/etc/icu",
               "apex/com.android.tzdata/etc/tz"]
    files = [(icu_data, f"apex/com.android.i18n/etc/icu/{os.path.basename(icu_data)}"),
             (os.path.join(bionic_system, "usr", "share", "zoneinfo", "tzdata"), "apex/com.android.tzdata/etc/tz/tzdata")]
    if conscrypt_jar is not None:
        folders += ["apex/com.android.conscrypt/javalib", "apex/com.android.conscrypt/lib64",
                    "apex/com.android.conscrypt/cacerts"]
        files.append((conscrypt_jar, "apex/com.android.conscrypt/javalib/conscrypt.jar"))
    for folder in folders:
        os.makedirs(os.path.join(run, folder))
    for source, target in files:
        os.link(source, os.path.join(run, target))  # a file of its own, as the guest sees it, not a link out


def apk_run_command(apk_run, art, bionic_system, work, art_lib64, run, arguments, cwd="/", environment=(), jni=(),
                    conscrypt=False, art_lib64_first=None, measures=None):
    """apk-run's command that runs dalvikvm64 with `arguments` in the files of the run folder `run`, the working
    directory `cwd` and the entries "<name>=<value>" of `environment` on top of init's; with `conscrypt`, Conscrypt's
    lib64 holds its library and its cacerts the CA certificates. `art_lib64_first`, if given, is a folder of
    containers that ART's lib64 looks in before `art_lib64`; `measures`, if given, the file that gets what the run
    measured (apk-run --measures)."""
    command = [apk_run,
               "--mount", f"/data={os.path.join(run, 'data')}",
               "--mount", f"/apex={os.path.join(run, 'apex')}",
               "--mount", f"{ART_ROOT}/javalib={os.path.join(art, 'javalib')}",
               "--mount", f"/system={bionic_system}",
               "--libraries", f"/system/lib64={os.path.join(work, 'system-lib64')}"]
    if art_lib64_first is not None:
        command += ["--libraries", f"{ART_ROOT}/lib64={art_lib64_first}"]
    command += ["--libraries", f"{ART_ROOT}/lib64={art_lib64}"]
    if conscrypt:
        command += ["--libraries", f"{CONSCRYPT_ROOT}/lib64={os.path.join(work, 'conscrypt-lib64')}",
                    "--mount", f"{CONSCRYPT_ROOT}/cacerts={CA_CERTIFICATES}"]
    command += list(jni) + ["--cwd", cwd] + (["--measures", measures] if measures is not None else [])
    for entry in environment:
        command += ["--env", entry]
    return command + ["--argv0", f"{ART_ROOT}/bin/dalvikvm64",
                      os.path.join(work, "bin", "dalvikvm64.dylib")] + list(arguments)


def dalvikvm_options(debug=False, conscrypt=False, added_jars=(), library=False, collector=None, nterp=False):
    """dalvikvm's options of decision 11: the interpreter only, no boot image, the boot jars, with the jars of
    `added_jars` after core-libart and Conscrypt's last with `conscrypt`; ART's JNI checks in the debug flavor,
    java.library.path for a program with a JNI library in /data/local/tmp, and -Xgc: with a `collector`. With `nterp`
    the interpreter is nterp, ART's default: no -Xint (M6 a, patch 0008)."""
    boot_jars = BOOT_JARS[:2] + list(added_jars) + BOOT_JARS[2:]
    boot_class_path = ":".join([f"{ART_ROOT}/javalib/{name}.jar" for name in boot_jars]
                               + ([f"{CONSCRYPT_ROOT}/javalib/conscrypt.jar"] if conscrypt else []))
    options = ["-Xusejit:false", "-Xnoimage-dex2oat", f"-Xbootclasspath:{boot_class_path}",
               f"-Xbootclasspath-locations:{boot_class_path}", "-XjdwpProvider:none"]
    if not nterp:
        options = ["-Xint"] + options
    if debug:
        options = ["-Xcheck:jni"] + options
    if library:
        options.append("-Djava.library.path=/data/local/tmp")
    if collector:
        options.append(f"-Xgc:{collector}")
    return options


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


def stress_line(stderr):
    """stress-test's line of what its parts saw, or None."""
    return next((line for line in stderr.splitlines() if line.startswith("stress-test stats: ")), None)


def stress_failure(stderr):
    """Why stress-test's line does not show a signal handled during a collection, or None when it does."""
    line = stress_line(stderr)
    match = re.search(r", inside a collection (\d+) ", line or "")
    if not match:
        return "stderr has no line \"stress-test stats: ...\" with the signals inside a collection"
    if int(match.group(1)) == 0:
        return "no signal was handled while the main thread collected"
    return None


def collector_failure(stderr, collector):
    """Why ART's start line does not name the collector type `collector`, or None when it does."""
    named = re.findall(r"Using CollectorType(\w+) GC\.$", stderr, re.MULTILINE)
    if not named or any(name != collector for name in named):
        return f"ART's log names the collector types [{', '.join(named)}], not {collector}"
    return None


def window_failure(stderr, upper_half=False):
    """Why ART's log does not name the heap window once, with all 4 GiB or only their upper 2 GiB, or None."""
    sizes = re.findall(r"The heap window holds (\d+) MiB from 0x[0-9a-f]+, its base at 0x[0-9a-f]+$", stderr,
                       re.MULTILINE)
    wanted = "2048" if upper_half else "4096"
    if sizes != [wanted]:
        return (f"the log names the heap window {len(sizes)} times ({', '.join(sizes) or 'none'}), "
                f"not once with {wanted} MiB")
    return None


def nativeloader_failure(stderr, reference):
    """Why libnativeloader's lines that name a namespace are not Android 15's lines of logcat in `reference`, or
    None when they are."""
    with open(reference) as handle:
        wanted = handle.read().splitlines()
    seen = [line for line in stderr.splitlines() if NATIVELOADER_LINE.match(line)]
    if seen == wanted:
        return None
    return f"libnativeloader names other namespaces than {os.path.basename(reference)}: [{' | '.join(seen)}]"


def compiled_oat(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, run, jar, debug):
    """The oat of `jar` that our dex2oat makes with --compiler-filter=speed (M6 c2), next to the jar in the run folder
    as ART looks for it (oat/arm64/<name>.odex and .vdex), and its container, which --libraries gives ART's dlopen as
    that odex; returns apk-run's arguments for it."""
    import dex2oat_make
    name = os.path.splitext(os.path.basename(jar))[0]
    result, produced = dex2oat_make.make_odex(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, jar,
                                              f"/data/local/tmp/{os.path.basename(jar)}", debug, "speed")
    if result.returncode != 0:
        raise SystemExit(f"dex2oat exited with {result.returncode}\n{result.stderr[-3000:]}")
    oat = os.path.join(run, "data", "local", "tmp", "oat", "arm64")
    os.makedirs(oat)
    shutil.copy(os.path.join(produced, "app.odex"), os.path.join(oat, f"{name}.odex"))
    shutil.copy(os.path.join(produced, "app.vdex"), os.path.join(oat, f"{name}.vdex"))
    dex2oat_make.discard(produced)
    containers = os.path.join(work, "oat-" + os.path.basename(run))
    os.makedirs(containers, exist_ok=True)
    container = os.path.join(containers, f"{name}.odex.dylib")
    if os.path.exists(container):
        os.unlink(container)
    convert(apk_convert, os.path.join(oat, f"{name}.odex"), container)
    return ["--libraries", f"/data/local/tmp/oat/arm64={containers}"]


def main():
    arguments = sys.argv[1:]
    debug = "--debug" in arguments
    offsets = "--offsets" in arguments
    upper_half = "--upper-half" in arguments
    conscrypt = "--conscrypt" in arguments
    stress = "--stress" in arguments
    nterp = "--nterp" in arguments
    compiled = "--compiled" in arguments
    added_jars = [name for name in ("okhttp", "bouncycastle") if f"--{name}" in arguments]
    arguments = [argument for argument in arguments
                 if argument not in ("--debug", "--offsets", "--upper-half", "--conscrypt", "--okhttp",
                                     "--bouncycastle", "--stress", "--nterp", "--compiled")]
    library = None
    if "--library" in arguments:
        at = arguments.index("--library")
        library = arguments[at + 1] if at + 1 < len(arguments) else None
        del arguments[at:at + 2]
    nativeloader = None
    if "--nativeloader" in arguments:
        at = arguments.index("--nativeloader")
        nativeloader = arguments[at + 1] if at + 1 < len(arguments) else None
        del arguments[at:at + 2]
    collector = None
    if "--gc" in arguments:
        at = arguments.index("--gc")
        collector = arguments[at + 1] if at + 1 < len(arguments) else None
        del arguments[at:at + 2]
    if len(arguments) != 10 or ("--library" in sys.argv and library is None) or (
            "--nativeloader" in sys.argv and nativeloader is None) or ("--gc" in sys.argv and collector is None):
        raise SystemExit(__doc__)
    art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, jar, main_class, expected = arguments
    dalvikvm = os.path.join(art, "bin", "dalvikvm")
    for needed in [dalvikvm, jar] + [os.path.join(art, "javalib", f"{name}.jar") for name in added_jars]:
        if not os.path.exists(needed):
            print(f"skipped: {needed} is not built (tools/build-art.sh)")
            return
    if library is not None and not os.path.exists(library):
        print(f"skipped: {library} is not built (tools/build-fixtures.sh)")
        return
    conscrypt_jar = os.path.join(art, "javalib", "conscrypt.jar") if conscrypt else None
    if conscrypt:
        for needed in (conscrypt_jar, os.path.join(art, "lib", CONSCRYPT_LIBRARY)):
            if not os.path.exists(needed):
                print(f"skipped: {needed} is not built (tools/build-art.sh)")
                return
        if not os.path.isdir(CA_CERTIFICATES):
            print(f"skipped: {CA_CERTIFICATES} is not fetched (tools/fetch-sources.py --set art)")
            return
    public_libraries = os.path.join(bionic_system, "etc", "public.libraries.txt")
    if not os.path.exists(public_libraries):
        print(f"skipped: {public_libraries} is not built (tools/build-bionic.sh)")
        return
    skipped = missing_front_lib(bionic_lib)
    if skipped:
        print(skipped)
        return

    art_lib64 = convert_guest(art, bionic_lib, apk_convert, work, debug, library)
    run = os.path.join(work, os.path.splitext(os.path.basename(jar))[0] + ("-debug" if debug else "")
                       + ("-upper-half" if upper_half else "") + (f"-{collector.lower()}" if collector else ""))
    make_run_folder(run, icu_data, bionic_system, conscrypt_jar)
    # Read-only, as an app's dex must be: at SDK 34 and later ART refuses a writable one to every uid but root,
    # system and shell (dalvik_system_DexFile.cc), and the guest runs as the app uid; the reference ran as shell.
    copied = shutil.copy(jar, os.path.join(run, "data", "local", "tmp"))
    os.chmod(copied, 0o444)
    if library is not None:
        shutil.copy(library, os.path.join(run, "data", "local", "tmp"))

    options = dalvikvm_options(debug, conscrypt, added_jars, library is not None, collector, nterp or compiled)
    jni = ["--libraries", f"/data/local/tmp={os.path.join(work, 'jni')}"] if library is not None else []
    if compiled:
        jni += compiled_oat(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, run, jar, debug)
    command = apk_run_command(apk_run, art, bionic_system, work, art_lib64, run,
                              options + ["-cp", f"/data/local/tmp/{os.path.basename(jar)}", main_class], jni=jni,
                              conscrypt=conscrypt,
                              environment=["APKRT_HEAP_WINDOW_UPPER_HALF=1"] if upper_half else ())
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=300)
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL: no end within {timeout.timeout} s")
        sys.exit(1)
    with open(expected) as handle:
        wanted = handle.read()
    failure = offsets_failure(result.stderr) if offsets else None
    if failure is None and (offsets or upper_half):
        failure = window_failure(result.stderr, upper_half)
    if failure is None and nativeloader is not None:
        failure = nativeloader_failure(result.stderr, nativeloader)
    if failure is None and stress:
        failure = stress_failure(result.stderr)
    if failure is None and collector:
        failure = collector_failure(result.stderr, collector)
    if result.returncode == 0 and result.stdout == wanted and failure is None:
        print(f"{os.path.basename(jar)} printed what Android 15 prints" + (" (debug flavor)" if debug else "") +
              (" in the window's upper half" if upper_half else "") +
              ("; libnativeloader names Android 15's namespaces" if nativeloader else "") +
              (f"; {result.stderr.splitlines()[-1]}" if offsets else "") +
              (f"; {stress_line(result.stderr)}" if stress else "") +
              (f"; ART runs {collector}" if collector else ""))
        return
    print(f"FAIL: exit status {result.returncode}" + (f"; {failure}" if failure else ""))
    print(f"stdout: [{result.stdout}]")
    print(f"expected: [{wanted}]")
    print("stderr, last 60 lines:")
    print("\n".join(result.stderr.splitlines()[-60:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
