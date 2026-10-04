#!/usr/bin/env python3
"""A program outside the APEXes and the folders of ART and Conscrypt (ADR-0039 a, ADR-0040 decision 2, ADR-0046 a):
apk-run runs the fixture ns-test as /system/bin/ns-test, where Android 15 runs a program in linkerconfig's [system]
section, ten times:
  - with the fixture library libns-lib.so in /apex/com.android.art/lib64 under the name libjdwp.so, one of the names
    that section links to com_android_art, and libns-dep.so and libns-plugin.so beside it:
    - the program's dlopen of libns-dep.so, a library of that folder off the link list, fails as linker64 says;
    - its dlopen of libjdwp.so finds the library through the link, and the library finds what it needs
      (libns-dep.so) and what it opens (libns-plugin.so) in its own folder;
  - with the three in /apex/com.android.conscrypt/lib64, libns-lib.so under the name libjavacrypto.so, the library
    that conscrypt.jar loads:
    - the program's dlopen of libjavacrypto.so fails, as no link reaches that folder;
    - android_dlopen_ext opens it in the exported namespace com_android_conscrypt, where libnativeloader opens it
      for conscrypt.jar and ends the process if the namespace is missing, and the library finds what it needs and
      what it opens in its own folder;
  - with libns-plugin.so in an app's JNI folder, the lib/arm64 of Android 15's code path
    /data/app/~~<base64>==/<package>-<base64>==, which apk-run's --libraries and --mount must take whole, its '='
    included (ADR-0042 b1): the program's dlopen of the library by that path opens it;
  - with the three in that folder, libns-lib.so under its own name: in a namespace created on the folder as
    libnativeloader creates a class loader's, android_dlopen_ext opens the library by its full path, as ART opens an
    app's JNI library, and the library finds what it needs and what it opens in the namespace's folder, as linker64
    finds them in the library's namespace (ADR-0033 e0);
  - with ART's folder of the first run and an app's folder whose libns-dep.so is the app's own, with ART's soname and
    another value: the program opens libjdwp.so, which loads ART's libns-dep.so, then the app's libns-lib.so in a
    class loader's namespace, which gets the app's libns-dep.so, while ART keeps its own (ADR-0040 test 1, ADR-0033
    e2a);
  - with the app's folder and a library in the app's data folder, from a class loader's namespace, which is isolated
    with /data:/mnt/expand as its permitted path: a library of /system/lib64 off the public list, by its path, is
    refused with linker64's message; libz.so, a public one, by its path loads through the link; the library under the
    app's data folder loads (ADR-0033 e2b1);
  - the same below target SDK 24, where the namespace has the exempt list: libutils.so, which is on it, loads by its
    path in /system/lib64 with linker64's warning on stderr, and by its name from the default namespace (ADR-0033
    e2b1), which takes the instance of the first namespace with a line in the log (ADR-0040 decision 6, e2b2);
  - with libns-find.so, whose ns_lib_check asks dlsym(RTLD_DEFAULT) for ns_plugin_value, in /system/lib64 and in the
    app's folder: after the program opened a library with ns_plugin_value RTLD_GLOBAL in the default namespace, the
    copy in the default namespace sees it and the one in a class loader's namespace does not (ADR-0033 e2b2);
  - two class loaders' namespaces that each ask to be the anonymous namespace: the first is made and opens its
    library, the second is refused with linker64's message (ADR-0033 e2b2);
  - dlsym(RTLD_DEFAULT) passes over a library opened without RTLD_GLOBAL, but not one opened below target SDK 23
    (ADR-0033 e2b2).
Each run is a process of its own: the loader's one scope would give the second run's library the first run's
libraries by name (ADR-0033 decision 7). The runs must print exactly <expected> together and exit with 0. The
expected output is ours: Android lets nothing be installed into /system/bin or an APEX. Prints "skipped: ..." while
a part is not built.

Usage: ns_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

# An app's code path as Android 15 names it, with the halves of a SHA-256 in base64, each ending in "==".
APP_CODE = "/data/app/~~AAECAwQFBgcICQoLDA0ODw==/io.github.kisame76.minato.test.ns-EBESExQVFhcYGRobHB0eHw=="

# A library folder: the folder in <work> that holds it, its guest path, the containers in it (the name the loader
# finds, and the fixture it is made from), and a guest path that apk-run also mounts on the folder, or None.
ART = ("art-lib64", "/apex/com.android.art/lib64",
       {"libjdwp.so": "libns-lib.so", "libns-dep.so": "libns-dep.so", "libns-plugin.so": "libns-plugin.so"}, None)
CONSCRYPT = ("conscrypt-lib64", "/apex/com.android.conscrypt/lib64",
             {"libjavacrypto.so": "libns-lib.so", "libns-dep.so": "libns-dep.so", "libns-plugin.so": "libns-plugin.so"},
             None)
APP_PLUGIN = ("app-lib", f"{APP_CODE}/lib/arm64", {"libns-plugin.so": "libns-plugin.so"}, APP_CODE)
APP = ("class-loader-lib", f"{APP_CODE}/lib/arm64",
       {"libns-lib.so": "libns-lib.so", "libns-dep.so": "libns-dep.so", "libns-plugin.so": "libns-plugin.so"}, APP_CODE)
# The app ships its own libns-dep.so, with ART's soname and another value (ADR-0040 test 1).
APP_OWN = ("app-own-lib", f"{APP_CODE}/lib/arm64",
           {"libns-lib.so": "libns-lib.so", "libns-dep.so": "libns-dep-app.so", "libns-plugin.so": "libns-plugin.so"},
           APP_CODE)
CLASS_LOADER = f"@{APP_CODE}/lib/arm64:{APP_CODE}/lib/arm64/libns-lib.so"
# The app's data folder, as Android 15 names it, with a library the app put into its files.
APP_DATA = "/data/user/0/io.github.kisame76.minato.test.ns"
DATA = ("data-files", f"{APP_DATA}/files", {"libns-data.so": "libns-plugin.so"}, f"{APP_DATA}/files")
# Libraries of /system/lib64 besides bionic's: one off the public list, libutils.so, on the exempt list, and the probe
# of dlsym(RTLD_DEFAULT).
SYSTEM_EXTRA = {"libns-private.so": "libns-plugin.so", "libutils.so": "libns-plugin.so",
                "libns-find.so": "libns-find.so"}
FIND = ("find-lib", f"{APP_CODE}/lib/arm64", {"libns-find.so": "libns-find.so"}, APP_CODE)
# The log's line when a namespace takes an instance of another where linker64 would load the file again.
TAKEN = ('I/linker  : namespace "(default)" takes library "/system/lib64/libutils.so" from namespace "clns-ns-test", '
         "where linker64 would load the file again (ADR-0040 decision 6)")
# linker64's warning for a library of the exempt list that a namespace could not load otherwise.
EXEMPT_WARNING = (
    'WARNING: linker: Warning: library "/system/lib64/libutils.so" ("/system/lib64/libutils.so") needed or dlopened '
    'by "/system/bin/ns-test" is not accessible by namespace "clns-ns-test" and will not work when the app moves to '
    "API level 24 or later (https://android.googlesource.com/platform/bionic/+/main/android-changes-for-ndk-developers"
    ".md#private-api-enforced-for-api-level-24) (allowing for now because this app's target API level is still 23)")

# Each run: its library folders, ns-test's arguments, and lines its stderr must hold.
RUNS = [
    ([ART], ["libns-dep.so", "libjdwp.so"]),
    ([CONSCRYPT], ["libjavacrypto.so", "com_android_conscrypt:libjavacrypto.so"]),
    ([APP_PLUGIN], [f"{APP_CODE}/lib/arm64/libns-plugin.so"]),
    ([APP], [CLASS_LOADER]),
    ([ART, APP_OWN], ["libjdwp.so", CLASS_LOADER]),
    ([APP, DATA], [f"@{APP_CODE}/lib/arm64:/system/lib64/libns-private.so", f"@{APP_CODE}/lib/arm64:/system/lib64/libz.so",
                   f"@{APP_CODE}/lib/arm64:{APP_DATA}/files/libns-data.so"]),
    ([APP], ["sdk=23", f"@{APP_CODE}/lib/arm64:/system/lib64/libutils.so", f"@{APP_CODE}/lib/arm64:libutils.so"],
     [EXEMPT_WARNING, TAKEN]),
    ([FIND], ["+/system/lib64/libns-private.so", "libns-find.so",
              f"@{APP_CODE}/lib/arm64:{APP_CODE}/lib/arm64/libns-find.so"]),
    ([APP], [f"@{CLASS_LOADER}", f"@{CLASS_LOADER}"]),
    ([], ["/system/lib64/libns-private.so", "libns-find.so", "sdk=22", "/system/lib64/libutils.so", "libns-find.so"]),
]
FOLDERS = list({folder[0]: folder for folders, *_ in RUNS for folder in folders}.values())


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def main():
    if len(sys.argv) != 8:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work, expected = sys.argv[1:]
    fixtures_used = {fixture for _, _, containers, _ in FOLDERS for fixture in containers.values()}
    for name in ["ns-test"] + sorted(fixtures_used | set(SYSTEM_EXTRA.values())):
        if not os.path.exists(os.path.join(fixtures, name)):
            print(f"skipped: {os.path.join(fixtures, name)} is not built (tools/build-fixtures.sh)")
            return

    # The containers stay from run to run; runs side by side convert one at a time.
    system = os.path.join(work, "system-lib64")
    program = os.path.join(work, "bin", "ns-test.dylib")
    for folder in [system, os.path.dirname(program)] + [os.path.join(work, folder[0]) for folder in FOLDERS]:
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        for name, fixture in SYSTEM_EXTRA.items():
            convert(apk_convert, os.path.join(fixtures, fixture), os.path.join(system, name + ".dylib"))
        for folder, _, containers, _ in FOLDERS:
            for name, fixture in containers.items():
                convert(apk_convert, os.path.join(fixtures, fixture), os.path.join(work, folder, name + ".dylib"))
        convert(apk_convert, os.path.join(fixtures, "ns-test"), program)

    printed = ""
    missing = []
    for folders, arguments, *extra in RUNS:
        command = [apk_run, "--mount", f"/system={bionic_system}", "--libraries", f"/system/lib64={system}"]
        for folder, libraries, _, mount in folders:
            command += ["--libraries", f"{libraries}={os.path.join(work, folder)}"]
            if mount is not None:
                command += ["--mount", f"{mount}={os.path.join(work, folder)}"]
        command += ["--cwd", "/", "--argv0", "/system/bin/ns-test", program] + arguments
        try:
            result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)
        except subprocess.TimeoutExpired as timeout:
            print(f"FAIL: no end within {timeout.timeout} s")
            sys.exit(1)
        printed += result.stdout
        missing = [line for line in (extra[0] if extra else []) if line not in result.stderr.splitlines()]
        if result.returncode != 0 or missing:
            break
    with open(expected) as handle:
        wanted = handle.read()
    if result.returncode == 0 and printed == wanted and not missing:
        print("ns-test found ART's folder through the link list and Conscrypt's through its exported namespace, and "
              "the library there what it needs and opens beside it; and an app's library by its code path, within "
              "its namespace's paths")
        return
    print(f"FAIL: exit status {result.returncode}")
    for line in missing:
        print(f"stderr lacks: [{line}]")
    print(f"stdout: [{printed}]")
    print(f"expected: [{wanted}]")
    print("stderr, last 40 lines:")
    print("\n".join(result.stderr.splitlines()[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
