#!/usr/bin/env python3
"""ADR-0039's test 1 on the Mac: app_process starts app-start-test as Android 15 starts an app, and the app prints
what Android 15's process showed it (fixtures/expected/app-start-test.logcat).

apk-run runs /system/bin/app_process64 with the process name as --nice-name, as zygote's app_process starts, with
the files where Android has them, as
runtime/tests/art_run.py lays them out for dalvikvm:
  - ART's libraries in /apex/com.android.art/lib64, the boot jars in /apex/com.android.art/javalib, and our
    framework.jar in /system/framework;
  - bionic's libraries and libandroid_runtime.so (ADR-0039 decision 10) in /system/lib64, bionic's /system, ICU's
    data and the time zone file in their APEXes;
  - the APK at Android 15's code path (/data/app/~~<a>/<package>-<b>/base.apk, ADR-0039 decision 6), and the data
    folder /data/user/0/<package> with cache and code_cache, as installd creates them;
  - base.apk read-only, as Android's 0644 system:system file is to the app: ART refuses a dex file the app may
    write (dalvik_system_DexFile.cc);
  - framework-res as the folder /system/framework/framework-res (decision 8), which ActivityThread reads the app's
    manifest with since b3.5a, as every Android device has it. BOOTCLASSPATH, the uid and the heap properties come from the runtime
    (decision 2), and --app-environment starts it with init's environment alone, as an app starts, without the
    shell's PATH, ANDROID_DATA, ANDROID_ROOT and HOME.

The comparison takes the reference's lines without logcat's "I/System.out: " and leaves out what ADR-0039 lets
differ: BOOTCLASSPATH's and DEX2OATBOOTCLASSPATH's jars, the uid's number (10100 for every app, decision 3), and
zygote's ANDROID_SOCKET_* (the lead's call: they name zygote's descriptors). Prints "skipped: ..." while a part is
not built. framework/tests/java_app_run.py starts its apps with this file's start().

Usage: app_process_run.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work>
                          <app_process64> <framework.jar> <framework-res folder> <app-start-test.apk> <expected>
"""
import base64
import hashlib
import os
import re
import shutil
import subprocess
import sys

ART_ROOT = "/apex/com.android.art"
PACKAGE = "io.github.kisame76.minato.test.appstart"
APPLICATION = "io.github.kisame76.minato.test.appstart.StartApplication"
APP_UID = 10100


def convert(apk_convert, elf, container):
    """The container of `elf`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    os.makedirs(os.path.dirname(container), exist_ok=True)
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def convert_directory(apk_convert, source, target):
    for name in sorted(os.listdir(source)):
        if name.endswith(".so"):
            convert(apk_convert, os.path.join(source, name), os.path.join(target, name + ".dylib"))


def code_path(apk, package):
    """Android 15's code path for this APK as GuestStart makes it: the halves of base.apk's SHA-256."""
    digest = hashlib.sha256(open(apk, "rb").read()).digest()
    def half(data):
        return base64.urlsafe_b64encode(data).decode()
    return f"/data/app/~~{half(digest[:16])}/{package}-{half(digest[16:])}"


def expected_lines(path):
    """The reference's lines as our process must print them."""
    lines = []
    for line in open(path, encoding="utf-8").read().splitlines():
        line = line.removeprefix("I/System.out: ")
        if re.match(r"app-start-test: env (ANDROID_SOCKET_\w+)=", line):
            continue
        line = re.sub(r"^(app-start-test: env (DEX2OAT)?BOOTCLASSPATH)=.*", r"\1=<jars>", line)
        line = re.sub(r"^(app-start-test: (getuid|Process\.myUid)) \d+$", rf"\1 {APP_UID}", line)
        lines.append(line)
    return lines


def our_lines(stdout):
    lines = []
    for line in stdout.splitlines():
        if not line.startswith("app-start-test: "):
            continue
        lines.append(re.sub(r"^(app-start-test: env (DEX2OAT)?BOOTCLASSPATH)=.*", r"\1=<jars>", line))
    return lines


def missing(art, bionic_system, app_process, framework_jar, apk, app):
    """The first part that is not built, as "skipped: ..." text, or None."""
    for needed, how in ((os.path.join(art, "lib", "libart.so"), "tools/build-art.sh"),
                        (app_process, "tools/build-framework-native.sh"),
                        (framework_jar, "tools/build-framework.sh"),
                        (apk, f"tools/build-java-test-app.sh {app}"),
                        (os.path.join(bionic_system, "etc", "public.libraries.txt"), "tools/build-bionic.sh")):
        if not os.path.exists(needed):
            return f"skipped: {needed} is not built ({how})"
    return None


def start(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process, framework_jar, apk,
          package, application, mounts=(), configuration=None):
    """Starts the APK's Application class through app_process as an app process; apk-run's result, or None after
    a timeout. libandroid_runtime.so beside app_process goes to /system/lib64, `mounts` are more --mount entries
    (guest path, host path), and `configuration` begins a session with apk-run's --configuration
    (<width>x<height>@<scale>[,<language>[-<country>]]) before app_process starts."""
    os.makedirs(work, exist_ok=True)
    art_lib64 = os.path.join(work, "art-lib64")
    system_lib64 = os.path.join(work, "system-lib64")
    convert_directory(apk_convert, os.path.join(art, "lib"), art_lib64)
    convert_directory(apk_convert, bionic_lib, system_lib64)
    # app_process needs it once it links it: then the loader names it missing.
    runtime = os.path.join(os.path.dirname(app_process), "libandroid_runtime.so")
    if os.path.exists(runtime):
        convert(apk_convert, runtime, os.path.join(system_lib64, "libandroid_runtime.so.dylib"))
    convert(apk_convert, app_process, os.path.join(work, "bin", "app_process64.dylib"))

    run = os.path.join(work, "run")
    shutil.rmtree(run, ignore_errors=True)
    app = code_path(apk, package)
    data = f"/data/user/0/{package}"
    for folder in ("data/dalvik-cache/arm64", app.lstrip("/"), f"{data}/cache".lstrip("/"),
                   f"{data}/code_cache".lstrip("/"), "apex/com.android.art/javalib", "apex/com.android.art/lib64",
                   "apex/com.android.art/bin", "apex/com.android.i18n/etc/icu", "apex/com.android.tzdata/etc/tz",
                   "framework"):
        os.makedirs(os.path.join(run, folder))
    shutil.copy(apk, os.path.join(run, app.lstrip("/"), "base.apk"))
    os.chmod(os.path.join(run, app.lstrip("/"), "base.apk"), 0o444)
    os.makedirs(os.path.join(run, app.lstrip("/"), "lib", "arm64"))
    shutil.copy(framework_jar, os.path.join(run, "framework", "framework.jar"))
    for source, target in ((icu_data, f"apex/com.android.i18n/etc/icu/{os.path.basename(icu_data)}"),
                           (os.path.join(bionic_system, "usr", "share", "zoneinfo", "tzdata"),
                            "apex/com.android.tzdata/etc/tz/tzdata")):
        os.link(source, os.path.join(run, target))

    command = [apk_run,
               "--mount", f"/data={os.path.join(run, 'data')}",
               "--mount", f"/apex={os.path.join(run, 'apex')}",
               "--mount", f"{ART_ROOT}/javalib={os.path.join(art, 'javalib')}",
               "--mount", f"/system={bionic_system}",
               "--mount", f"/system/framework={os.path.join(run, 'framework')}"]
    for guest, host in mounts:
        command += ["--mount", f"{guest}={host}"]
    if configuration is not None:
        command += ["--configuration", configuration]
    command += ["--libraries", f"/system/lib64={system_lib64}",
                "--libraries", f"{ART_ROOT}/lib64={art_lib64}",
                "--cwd", "/", "--app-environment", "--argv0", "/system/bin/app_process64",
                os.path.join(work, "bin", "app_process64.dylib"),
                f"--nice-name={package}", "--package", package, "--apk", f"{app}/base.apk",
                "--library-path", f"{app}/lib/arm64", "--data-dir", data, "--application", application]
    try:
        return subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=300)
    except subprocess.TimeoutExpired:
        return None


def main():
    if len(sys.argv) != 13:
        raise SystemExit(__doc__)
    (art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process, framework_jar, folder, apk,
     expected) = sys.argv[1:]
    skipped = missing(art, bionic_system, app_process, framework_jar, apk, "app-start-test")
    if not skipped and not os.path.isdir(folder):
        skipped = f"skipped: {folder} is not built (tools/build-framework-res.sh)"
    if skipped:
        print(skipped)
        return
    result = start(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process, framework_jar,
                   apk, PACKAGE, APPLICATION, mounts=[("/system/framework/framework-res", folder)])
    if result is None:
        print("FAIL: no end within 300 s")
        sys.exit(1)
    wanted, got = expected_lines(expected), our_lines(result.stdout)
    if result.returncode == 0 and got == wanted:
        print(f"app-start-test printed what Android 15's app process shows, {len(got)} lines")
        return
    print(f"FAIL: exit status {result.returncode}")
    for line in wanted:
        if line not in got:
            print(f"  only Android's: {line}")
    for line in got:
        if line not in wanted:
            print(f"  only ours:      {line}")
    print("stderr, last 40 lines:")
    print("\n".join(result.stderr.splitlines()[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
