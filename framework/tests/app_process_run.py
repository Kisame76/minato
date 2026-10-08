#!/usr/bin/env python3
"""ADR-0039's test 1 on the Mac: app_process starts app-start-test as Android 15 starts an app, and the app prints
what Android 15's process showed it (fixtures/expected/app-start-test.logcat).

apk-run runs /system/bin/app_process64 with the process name as --nice-name, as zygote's app_process starts, with
the files where Android has them, as
runtime/tests/art_run.py lays them out for dalvikvm:
  - ART's libraries in /apex/com.android.art/lib64, the boot jars in /apex/com.android.art/javalib, and our
    framework.jar in /system/framework;
  - Conscrypt's jar and libjavacrypto in their own APEX, /apex/com.android.conscrypt, as BOOTCLASSPATH names the jar
    (ADR-0046 c, decision 6): without it libcore's JCA providers fail to load;
  - bionic's libraries and libandroid_runtime.so (ADR-0039 decision 10) in /system/lib64, bionic's /system, ICU's
    data and the time zone file in their APEXes;
  - libhwui.so and its set (tools/build-hwui.sh) in /system/lib64, with ART's build's copies of libbase,
    libziparchive, libexpat and libc++_shared beside them, as graphics/tests/hwui_load_run.py lays them out:
    libandroid_runtime registers hwui's graphics natives at every start, and bionic's /system has the fonts that
    ActivityThread loads (ADR-0043 test 2);
  - the graphics front libs libEGL, libGLESv1_CM, libGLESv2, libGLESv3 and libvulkan (tools/build-front-libs.sh) in
    /system/lib64: public libraries, which libnativeloader preloads at every start (ADR-0040 decision 3), and the
    first 4 are what libandroid_runtime's GLES bindings need (ADR-0041 a)). Under apk-run they bind no driver; under
    graphics-run, apk-run with graphics/host's providers, they reach the Mac's ANGLE and MoltenVK (MINATO_ANGLE,
    MINATO_MOLTENVK);
  - the APK at Android 15's code path (/data/app/~~<a>/<package>-<b>/base.apk, ADR-0039 decision 6), and the data
    folders /data/user/0/<package> and /data/user_de/0/<package> with cache and code_cache, as installd creates the
    credential and the device protected one;
  - base.apk read-only, as Android's 0644 system:system file is to the app: ART refuses a dex file the app may
    write (dalvik_system_DexFile.cc);
  - framework-res as the folder /system/framework/framework-res (decision 8), which ActivityThread reads the app's
    manifest with since b3.5a, as every Android device has it. BOOTCLASSPATH, the uid and the heap properties come
    from the runtime (decision 2), and --app-environment starts it with init's environment alone, as an app starts,
    without the shell's PATH, ANDROID_DATA, ANDROID_ROOT and HOME.

The comparison takes the reference's lines without logcat's "I/System.out: " and leaves out what ADR-0039 lets
differ: BOOTCLASSPATH's and DEX2OATBOOTCLASSPATH's jars, the uid's number (10100 for every app, decision 3), and
zygote's ANDROID_SOCKET_* (the lead's call: they name zygote's descriptors). Since step c2b the app process runs its
main Looper and never ends by itself: the run ends 2 s after the MessageDigest line, the app's last, as
tools/android-reference.sh stops the app on the AVD after that line. app-start-test starts without --application, as
the app starts on the iPhone: ActivityThread makes the class its manifest names (step c1.1). ART's start line must
name CMS, the collector app_process gives an app (ADR-0033 decision 5). Prints "skipped: ..." while a part is not
built. framework/tests/java_app_run.py starts its apps with this file's start().

Usage: app_process_run.py <art> <bionic-lib> <bionic-system> <hwui-lib> <icu-data> <apk-convert> <apk-run> <work>
                          <app_process64> <framework.jar> <framework-res folder> <app-start-test.apk> <expected>
"""
import base64
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile
import zlib

# hwui's checks (graphics/tests): whether an app process can start with hwui, and the layout of its set.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "graphics", "tests"))
import hwui_ready  # noqa: E402
from hwui_load_run import PLATFORM_COPIES  # noqa: E402

ART_ROOT = "/apex/com.android.art"
CONSCRYPT_ROOT = "/apex/com.android.conscrypt"
CONSCRYPT_LIBRARY = "libjavacrypto.so"
# The APK folder of the JNI libraries that Android's installer extracts for an arm64 device.
APP_ABI = "lib/arm64-v8a/"
# The graphics front libs: public libraries, which every start preloads (ADR-0040 decision 3); libandroid_runtime.so
# links the first 4 (ADR-0041 a)).
FRONT_LIBS = ("libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so")
PACKAGE = "io.github.kisame76.minato.test.appstart"
APP_UID = 10100
# app-start-test's last line, after which the reference stops it (tools/android-reference.sh).
LAST = r"^app-start-test: MessageDigest "
# The collector an app runs, app_process's -Xgc: (ADR-0033 decision 5), as ART's heap names it in the line it logs
# at every start, on Android 15 too: "Using CollectorTypeCMS GC.".
COLLECTOR = "CMS"
# Monet's overlays for /data/resource-cache (ADR-0043 f)).
MONET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "monet-overlays.py")


def collector_type(stderr):
    """The collector type that ART's start line names, or None."""
    match = re.search(r"Using CollectorType(\w+) GC\.", stderr)
    return match.group(1) if match else None


def convert(apk_convert, elf, container):
    """The container of `elf`, made again unless it was made from this very file: its real path, size and time, which
    a stamp in the folder's .from/ records. A check for a newer ELF alone kept a container of a newer build when a
    folder such as build/hwui pointed at an older one."""
    info = os.stat(elf)
    source = f"{os.path.realpath(elf)} {info.st_size} {info.st_mtime_ns}\n"
    stamp = os.path.join(os.path.dirname(container), ".from", os.path.basename(container))
    if os.path.exists(container) and os.path.exists(stamp):
        with open(stamp, encoding="utf-8") as handle:
            if handle.read() == source:
                return
    os.makedirs(os.path.dirname(stamp), exist_ok=True)
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")
    with open(stamp, "w", encoding="utf-8") as handle:
        handle.write(source)


def convert_directory(apk_convert, source, target, leave_out=()):
    for name in sorted(os.listdir(source)):
        if name.endswith(".so") and name not in leave_out:
            convert(apk_convert, os.path.join(source, name), os.path.join(target, name + ".dylib"))


def extracted(name):
    """Whether Android's installer extracts a file of lib/<abi>/ by this name, as APKLibrary.isExtracted has its rule
    (NativeLibraryHelper's isFilenameSafe): lib*.so of A-Z, a-z, 0-9 and + , - . = _."""
    return (name.startswith("lib") and name.endswith(".so")
            and all(char.isascii() and (char.isalnum() or char in "+,-.=_") for char in name))


def install_libraries(apk_convert, apk, folder, work):
    """The APK's JNI libraries as the import installs them (APKLibrary.installNativeCode, ADR-0042 b1): every
    lib/arm64-v8a/<name> that Android's installer extracts, its ELF original read-only in `folder`, the app's
    lib/arm64, where ART's class loader looks for the file, and its container in work/app-lib64, which apk-run's
    --libraries makes that folder's libraries. The originals stay in work/app-lib, dated as the APK's entries, so that
    convert()'s stamps keep the containers across runs; `folder` gets hard links to them. A changed library is
    converted again, and one the APK no longer holds goes with its container. Returns the names, sorted."""
    originals = os.path.join(work, "app-lib")
    containers = os.path.join(work, "app-lib64")
    with zipfile.ZipFile(apk) as archive:
        entries = {info.filename[len(APP_ABI):]: info for info in archive.infolist()
                   if info.filename.startswith(APP_ABI) and extracted(info.filename[len(APP_ABI):])}
        for name, info in sorted(entries.items()):
            original = os.path.join(originals, name)
            container = os.path.join(containers, name + ".dylib")
            dated = time.mktime(info.date_time + (0, 0, -1))
            if not same_file(original, info, dated):
                # A changed library may keep its size and date: its container goes, so that convert() makes it again.
                for stale in (original, container, os.path.join(containers, ".from", name + ".dylib")):
                    if os.path.lexists(stale):
                        os.remove(stale)
                os.makedirs(originals, exist_ok=True)
                with archive.open(info) as source, open(original, "wb") as target:
                    shutil.copyfileobj(source, target)
                os.chmod(original, 0o444)
                os.utime(original, (dated, dated))
            convert(apk_convert, original, container)
            link = os.path.join(folder, name)
            if not (os.path.exists(link) and os.path.samefile(link, original)):
                if os.path.lexists(link):
                    os.remove(link)
                os.link(original, link)
    for directory, suffix in ((originals, ""), (containers, ".dylib")):
        for file in os.listdir(directory) if os.path.isdir(directory) else []:
            if not file.startswith(".") and file[:len(file) - len(suffix)] not in entries:
                os.remove(os.path.join(directory, file))
                if os.path.lexists(os.path.join(directory, ".from", file)):
                    os.remove(os.path.join(directory, ".from", file))
    return sorted(entries)


def same_file(path, info, dated):
    """Whether the file at `path` is the archive entry `info`, extracted and dated `dated`: its size, date and CRC."""
    if not os.path.isfile(path):
        return False
    status = os.stat(path)
    if status.st_size != info.file_size or int(status.st_mtime) != int(dated):
        return False
    crc = 0
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            crc = zlib.crc32(block, crc)
    return crc == info.CRC


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


def front_libs(app_process):
    """The folder of the graphics front libs: tools/build-front-libs.sh's, which lies beside app_process's build."""
    return os.path.join(os.path.dirname(app_process), "..", "graphics", "guest", "bin")


def missing(art, bionic_system, hwui_lib, app_process, framework_jar, apk, app):
    """The first part that is not built, as "skipped: ..." text, or None, and then hwui's set and bionic's font lists
    as graphics/tests/hwui_ready.py has them: every start registers hwui's natives and loads the font map."""
    for needed, how in (*((os.path.join(front_libs(app_process), name), "tools/build-front-libs.sh")
                          for name in FRONT_LIBS),
                        (os.path.join(art, "lib", "libart.so"), "tools/build-art.sh"),
                        (os.path.join(art, "lib", CONSCRYPT_LIBRARY), "tools/build-art.sh"),
                        (os.path.join(art, "javalib", "conscrypt.jar"), "tools/build-art.sh"),
                        (app_process, "tools/build-framework-native.sh"),
                        (framework_jar, "tools/build-framework.sh"),
                        (apk, f"tools/build-java-test-app.sh {app}"),
                        (os.path.join(bionic_system, "etc", "public.libraries.txt"), "tools/build-bionic.sh")):
        if not os.path.exists(needed):
            return f"skipped: {needed} is not built ({how})"
    return hwui_ready.missing(hwui_lib, bionic_system)


def run_app(command, last=None, grace=2.0, timeout=300):
    """The result of `command`, apk-run's, or None when it has not ended within `timeout` s, after which it is killed.
    An app process runs its main Looper and does not end by itself (step c2b): `last` is a pattern of the app's last
    line, and once a line of its output or its log matches it, the process has `grace` s to end. Then it is killed,
    as tools/android-reference.sh has am force-stop end the app after that line, and the run counts as ended, with
    status 0. A process that ends by itself keeps its status, also a crash within the grace."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    pattern = re.compile(last) if last is not None else None
    seen = threading.Event()
    output = {"stdout": [], "stderr": []}

    def read(name, stream):
        for line in stream:
            output[name].append(line)
            if pattern is not None and pattern.search(line.rstrip("\n")):
                seen.set()

    readers = [threading.Thread(target=read, args=(name, getattr(process, name))) for name in output]
    for reader in readers:
        reader.start()
    deadline = time.monotonic() + timeout
    while process.poll() is None and not seen.is_set() and time.monotonic() < deadline:
        seen.wait(0.1)
    status = process.poll()
    if status is None and seen.is_set():
        try:
            status = process.wait(grace)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            status = 0
    elif status is None:
        process.kill()
        process.wait()
    for reader in readers:
        reader.join()
    if status is None:
        return None
    return subprocess.CompletedProcess(command, status, "".join(output["stdout"]), "".join(output["stderr"]))


def start(art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, apk_run, work, app_process, framework_jar,
          apk, package, application, mounts=(), configuration=None, signers=None, again=False, last=None,
          lifecycle=None, android_id=None, obbs=(), packs=()):
    """Starts the APK's Application class through app_process as an app process; apk-run's result, or None after a
    timeout, as run_app() ends it 2 s after a line matches `last`, the app's last line. `application` goes to
    ActivityThread as --application, which takes the place of the manifest's class; None leaves the manifest's (step
    c1.1). libandroid_runtime.so beside app_process goes to /system/lib64 with hwui's set and ART's copies for it,
    `mounts` are more --mount entries (guest path, host path), and `configuration` begins a session with apk-run's
    --configuration (<width>x<height>@<scale>[,<language>[-<country>]]) before app_process starts, whose host then
    plays `lifecycle`, apk-run's --lifecycle steps (ADR-0043 step c1), "start" when it is None. `signers`, the APK's
    signers.json (apk-inspect --signers), lies beside base.apk and is read-only like it, as the library keeps it in
    the version's folder, and ActivityThread gets it with --signers (b3.5b1). The app's data folder is a mount of its
    own at /data/data/<package>, which the runtime's link /data/user/0 names too, and its external data and OBB
    folders are mounted below /storage/emulated/0, where the runtime's links lead /sdcard, as GuestStart mounts them
    (decision 6, and ADR-0050 c1's d4 for the paths): a rename or link between them and the rest of
    /data answers EXDEV, as on Android 15 (ADR-0039 decision 3). The run's /data holds /data/data, which the guest
    stats with Android 15's owner, root's 0751 (ADR-0052); /data/local stays absent, as the owner model counts
    /data/local/tmp as the app's. With framework-res's folder among `mounts`, /data/resource-cache holds Monet's
    overlays made from that build of framework-res, as SystemUI and idmap2 keep them there on Android 15 (ADR-0043
    f)): every app's Resources take them, as its overlay paths. libsqlite.so beside app_process goes to /system/lib64
    too, for libandroid_runtime's database natives (the compatibility plan's gap 7).
    `android_id` goes to ActivityThread as --android-id, the value its settings provider gives as android_id, as the
    host gives it (ADR-0045, gap 13a); None leaves it to make one for the start.
    `again` starts it once more over the run folder of the start before, as Android starts an app's process again over
    its data (step c1); the folders are made only for a new run. `obbs` are files copied into the app's OBB folder of
    a new run, as a user or the library puts them there (ADR-0049 test 8). `packs` are files copied read-only into the
    folder local_testing of the app's external files folder of a new run, as the library lays out an app's kept pack
    slices there, and ActivityThread gets --local-testing-dir local_testing, as GuestStart passes it (ADR-0050 test
    2)."""
    os.makedirs(work, exist_ok=True)
    art_lib64 = os.path.join(work, "art-lib64")
    system_lib64 = os.path.join(work, "system-lib64")
    convert_directory(apk_convert, os.path.join(art, "lib"), art_lib64, leave_out=(CONSCRYPT_LIBRARY,))
    # Conscrypt's library is its APEX's, where its jar's namespace looks; a copy an older start left in ART's goes.
    conscrypt_lib64 = os.path.join(work, "conscrypt-lib64")
    if os.path.exists(os.path.join(art_lib64, CONSCRYPT_LIBRARY + ".dylib")):
        os.remove(os.path.join(art_lib64, CONSCRYPT_LIBRARY + ".dylib"))
    convert(apk_convert, os.path.join(art, "lib", CONSCRYPT_LIBRARY),
            os.path.join(conscrypt_lib64, CONSCRYPT_LIBRARY + ".dylib"))
    convert_directory(apk_convert, bionic_lib, system_lib64)
    convert_directory(apk_convert, hwui_lib, system_lib64)
    for name in PLATFORM_COPIES:
        convert(apk_convert, os.path.join(art, "lib", name), os.path.join(system_lib64, name + ".dylib"))
    for name in FRONT_LIBS:
        convert(apk_convert, os.path.join(front_libs(app_process), name), os.path.join(system_lib64, name + ".dylib"))
    # app_process needs it once it links it: then the loader names it missing.
    runtime = os.path.join(os.path.dirname(app_process), "libandroid_runtime.so")
    if os.path.exists(runtime):
        convert(apk_convert, runtime, os.path.join(system_lib64, "libandroid_runtime.so.dylib"))
    # Its database natives link libsqlite.so (the compatibility plan's gap 7), which Android 15 has in /system/lib64.
    sqlite = os.path.join(os.path.dirname(app_process), "libsqlite.so")
    if os.path.exists(sqlite):
        convert(apk_convert, sqlite, os.path.join(system_lib64, "libsqlite.so.dylib"))
    convert(apk_convert, app_process, os.path.join(work, "bin", "app_process64.dylib"))

    run = os.path.join(work, "run")
    app = code_path(apk, package)
    data = f"/data/user/0/{package}"
    device_data = f"/data/user_de/0/{package}"
    # The app's external data and OBB folders at the paths Android 15's realpath gives them, where /sdcard leads.
    external = {f"/storage/emulated/0/Android/data/{package}": "external-data",
                f"/storage/emulated/0/Android/obb/{package}": "external-obb"}
    if not again or not os.path.isdir(run):
        shutil.rmtree(run, ignore_errors=True)
        for folder in ("data/dalvik-cache/arm64", "data/data", app.lstrip("/"), f"{data}/cache".lstrip("/"),
                       f"{data}/code_cache".lstrip("/"), f"{device_data}/cache".lstrip("/"),
                       f"{device_data}/code_cache".lstrip("/"), "apex/com.android.art/javalib",
                       "apex/com.android.art/lib64", "apex/com.android.art/bin", "apex/com.android.i18n/etc/icu",
                       "apex/com.android.tzdata/etc/tz", "apex/com.android.conscrypt/javalib",
                       "apex/com.android.conscrypt/lib64", "framework", *sorted(set(external.values()))):
            os.makedirs(os.path.join(run, folder))
        for obb in obbs:
            shutil.copy(obb, os.path.join(run, "external-obb", os.path.basename(obb)))
        if packs:
            local_testing = os.path.join(run, "external-data", "files", "local_testing")
            os.makedirs(local_testing)
            for pack in packs:
                shutil.copy(pack, os.path.join(local_testing, os.path.basename(pack)))
                os.chmod(os.path.join(local_testing, os.path.basename(pack)), 0o444)
        shutil.copy(apk, os.path.join(run, app.lstrip("/"), "base.apk"))
        os.chmod(os.path.join(run, app.lstrip("/"), "base.apk"), 0o444)
        os.makedirs(os.path.join(run, app.lstrip("/"), "lib", "arm64"))
        if signers is not None:
            shutil.copy(signers, os.path.join(run, app.lstrip("/"), "signers.json"))
            os.chmod(os.path.join(run, app.lstrip("/"), "signers.json"), 0o444)
        shutil.copy(framework_jar, os.path.join(run, "framework", "framework.jar"))
        for source, target in ((icu_data, f"apex/com.android.i18n/etc/icu/{os.path.basename(icu_data)}"),
                               (os.path.join(bionic_system, "usr", "share", "zoneinfo", "tzdata"),
                                "apex/com.android.tzdata/etc/tz/tzdata"),
                               (os.path.join(art, "javalib", "conscrypt.jar"),
                                "apex/com.android.conscrypt/javalib/conscrypt.jar")):
            os.link(source, os.path.join(run, target))
    # Over the same run too, where they are already in place: the APK's JNI libraries (ADR-0042 b1).
    libraries = install_libraries(apk_convert, apk, os.path.join(run, app.lstrip("/"), "lib", "arm64"), work)

    command = [apk_run,
               "--mount", f"/data={os.path.join(run, 'data')}",
               "--mount", f"/apex={os.path.join(run, 'apex')}",
               "--mount", f"{ART_ROOT}/javalib={os.path.join(art, 'javalib')}",
               "--mount", f"/system={bionic_system}",
               "--mount", f"/system/framework={os.path.join(run, 'framework')}",
               # The data folder a mount of its own, as GuestStart mounts /data/data/<package> and Android 15 its
               # app's data folder, so that a rename or link from the rest of /data answers EXDEV (ADR-0039 decision 3);
               # /data/user/0/<package>, the --data-dir, leads there through the runtime's link, as on Android 15.
               "--mount", f"/data/data/{package}={os.path.join(run, data.lstrip('/'))}",
               # The device protected one too, as GuestStart mounts it.
               "--mount", f"{device_data}={os.path.join(run, device_data.lstrip('/'))}"]
    for guest, host in external.items():
        command += ["--mount", f"{guest}={os.path.join(run, host)}"]
    for guest, host in mounts:
        command += ["--mount", f"{guest}={host}"]
    framework_res = dict(mounts).get("/system/framework/framework-res")
    if framework_res is not None:
        cache = os.path.join(work, "resource-cache")
        result = subprocess.run([sys.executable, MONET, os.path.dirname(os.path.abspath(framework_res)), cache],
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"monet-overlays.py failed: {result.stderr.strip()}")
        command += ["--mount", f"/data/resource-cache={cache}"]
    if configuration is not None:
        command += ["--configuration", configuration, "--lifecycle", lifecycle or "start"]
    command += ["--libraries", f"/system/lib64={system_lib64}",
                "--libraries", f"{ART_ROOT}/lib64={art_lib64}",
                "--libraries", f"{CONSCRYPT_ROOT}/lib64={conscrypt_lib64}"]
    if libraries:
        command += ["--libraries", f"{app}/lib/arm64={os.path.join(work, 'app-lib64')}"]
    command += ["--cwd", "/", "--app-environment", "--argv0", "/system/bin/app_process64",
                os.path.join(work, "bin", "app_process64.dylib"),
                f"--nice-name={package}", "--package", package, "--apk", f"{app}/base.apk",
                "--library-path", f"{app}/lib/arm64", "--data-dir", data]
    if application is not None:
        command += ["--application", application]
    if signers is not None:
        command += ["--signers", f"{app}/signers.json"]
    if android_id is not None:
        command += ["--android-id", android_id]
    if packs:
        command += ["--local-testing-dir", "local_testing"]
    return run_app(command, last)


def main():
    if len(sys.argv) != 14:
        raise SystemExit(__doc__)
    (art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, apk_run, work, app_process, framework_jar,
     folder, apk, expected) = sys.argv[1:]
    skipped = missing(art, bionic_system, hwui_lib, app_process, framework_jar, apk, "app-start-test")
    if not skipped and not os.path.isdir(folder):
        skipped = f"skipped: {folder} is not built (tools/build-framework-res.sh)"
    if skipped:
        print(skipped)
        return
    result = start(art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, apk_run, work, app_process,
                   framework_jar, apk, PACKAGE, None, mounts=[("/system/framework/framework-res", folder)],
                   last=LAST)
    if result is None:
        print("FAIL: no end within 300 s")
        sys.exit(1)
    wanted, got = expected_lines(expected), our_lines(result.stdout)
    collector = collector_type(result.stderr)
    if result.returncode == 0 and got == wanted and collector == COLLECTOR:
        print(f"app-start-test printed what Android 15's app process shows, {len(got)} lines; ART runs {collector}")
        return
    print(f"FAIL: exit status {result.returncode}")
    if collector != COLLECTOR:
        print(f"  ART runs {collector or 'a collector its log does not name'}, not {COLLECTOR} (ADR-0033 decision 5)")
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
