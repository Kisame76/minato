#!/usr/bin/env python3
"""framework/tests/app_process_run.py's convert() (ADR-0039 step c1): a container is made again unless it was made
from the very ELF file given, by its real path, size and time, so a folder pointed at an older build gets that
build's containers. Run with a stand-in for apk-convert in a scratch directory. And its run_app() (step c2b): an app
process runs its main Looper and never ends by itself, so a run ends 2 s after the app's last line, killed as
am force-stop ends it on Android, and counts as ended; a process that ends by itself keeps its exit status. Run with
stand-ins for the app process in Python. And its install_libraries() with start() (ADR-0042 b1): the APK's JNI
libraries go in as the import installs them, the ELF originals read-only in the app's lib/arm64 and their containers
beside the run, converted again only for a changed library and gone with a removed one; an APK without libraries
keeps start()'s command as it was. Run with stand-ins for apk-convert and apk-run. And start() with the graphics
front libs (ADR-0041 a)), libvulkan's too, since every start preloads them as public libraries (ADR-0040 decision
3): each goes into /system/lib64 as a container, from tools/build-front-libs.sh's folder."""
import os
import stat
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app_process_run  # noqa: E402

failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


with tempfile.TemporaryDirectory() as scratch:
    calls = os.path.join(scratch, "calls")
    # apk-convert's stand-in: "convert <elf> <container> ..." copies the ELF and notes the call.
    fake = os.path.join(scratch, "apk-convert")
    with open(fake, "w") as handle:
        handle.write(f'#!/bin/sh\ncp "$2" "$3" && echo "$2" >> "{calls}"\n')
    os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)

    def calls_made():
        return open(calls).read().splitlines() if os.path.exists(calls) else []

    def elf(folder, text, mtime):
        path = os.path.join(scratch, folder, "libx.so")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as handle:
            handle.write(text)
        os.utime(path, (mtime, mtime))
        return path

    container = os.path.join(scratch, "lib64", "libx.so.dylib")
    newer = elf("newer", "the newer build", 2_000_000_000)
    older = elf("older", "the older build", 1_000_000_000)

    app_process_run.convert(fake, newer, container)
    check(open(container).read() == "the newer build" and len(calls_made()) == 1, "the first start converts")
    app_process_run.convert(fake, newer, container)
    check(len(calls_made()) == 1, "the same file again keeps the container")

    # The stale copy: the folder points at an older build, whose file is older than the container.
    app_process_run.convert(fake, older, container)
    check(open(container).read() == "the older build", "an older build's file replaces the newer container")

    # The same path with other contents of another size, at the same time.
    with open(older, "w") as handle:
        handle.write("the older build, rebuilt")
    os.utime(older, (1_000_000_000, 1_000_000_000))
    app_process_run.convert(fake, older, container)
    check(open(container).read() == "the older build, rebuilt", "a file of another size is converted again")

    # A container without its stamp, as a start before the stamps left it.
    os.remove(os.path.join(scratch, "lib64", ".from", "libx.so.dylib"))
    count = len(calls_made())
    app_process_run.convert(fake, older, container)
    check(len(calls_made()) == count + 1, "a container without its stamp is converted again")


def program(*lines):
    """A stand-in for the app process: Python's lines, with print and stderr flushed as the app's log is."""
    return [sys.executable, "-c", "import sys, time\n" + "\n".join(lines)]


LAST = r"^(?:[VDIWEF]/)?app: done$"
LOG = "sys.stderr.write('I/app: {}\\n'); sys.stderr.flush()"

# The last line, then more output within the grace, and a process that would run on: killed, and the run ended.
began = time.monotonic()
result = app_process_run.run_app(program("print('app: one', flush=True)", "print('app: done', flush=True)",
                                     "print('app: after', flush=True)", LOG.format("logged"), "time.sleep(60)"),
                             LAST, grace=0.5)
took = time.monotonic() - began
check(result is not None and result.returncode == 0, f"the run ends with its last line: {result}")
check(result is not None and result.stdout.splitlines() == ["app: one", "app: done", "app: after"]
      and result.stderr.splitlines() == ["I/app: logged"], f"the output before the kill is kept: {result}")
check(took < 10, f"the process is killed after the grace, not at its end: {took:.1f} s")

# The last line in the log ends it too, as Log.i(TAG, "done") gives it.
result = app_process_run.run_app(program(LOG.format("done"), "time.sleep(60)"), LAST, grace=0.5)
check(result is not None and result.returncode == 0 and result.stderr == "I/app: done\n", f"a log line: {result}")

# A line that only holds the text is not the last line: the process runs on past the grace and ends by itself.
began = time.monotonic()
result = app_process_run.run_app(program("print('app: done or not', flush=True)", "time.sleep(1.5)", "sys.exit(3)"),
                                 LAST, grace=0.5)
check(result is not None and result.returncode == 3 and time.monotonic() - began < 10,
      f"another line, then an end by itself with its status: {result}")

# A crash within the grace keeps the crash's status.
result = app_process_run.run_app(program("print('app: done', flush=True)", "time.sleep(0.2)", "sys.exit(10)"), LAST,
                             grace=5)
check(result is not None and result.returncode == 10, f"a crash after the last line: {result}")

# Without its last line, no end within the timeout: None, and the process is killed.
began = time.monotonic()
result = app_process_run.run_app(program("print('app: one', flush=True)", "time.sleep(60)"), LAST, grace=0.5,
                             timeout=1)
check(result is None and time.monotonic() - began < 10, f"no last line within the timeout: {result}")


def apk(path, libraries):
    """An APK of the given lib/ entries, each with a fixed date as a build gives it, beside a manifest."""
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(zipfile.ZipInfo("AndroidManifest.xml", date_time=(2026, 10, 4, 4, 18, 0)), "manifest")
        for name, text in libraries.items():
            archive.writestr(zipfile.ZipInfo(name, date_time=(2026, 10, 4, 4, 18, 0)), text)
    return path


def files(folder):
    return sorted(name for name in os.listdir(folder) if not name.startswith(".")) if os.path.isdir(folder) else []


with tempfile.TemporaryDirectory() as scratch:
    calls = os.path.join(scratch, "calls")
    fake = os.path.join(scratch, "apk-convert")
    with open(fake, "w") as handle:
        handle.write(f'#!/bin/sh\ncp "$2" "$3" && echo "$2" >> "{calls}"\n')
    os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)

    def conversions():
        return len(open(calls).read().splitlines()) if os.path.exists(calls) else 0

    work = os.path.join(scratch, "work")
    run_lib = os.path.join(work, "run", "data", "app", "x", "lib", "arm64")
    containers = os.path.join(work, "app-lib64")
    godot = apk(os.path.join(scratch, "godot.apk"), {
        "lib/arm64-v8a/libgodot_android.so": "godot's ELF",
        "lib/arm64-v8a/libc++_shared.so": "libc++'s ELF",
        # What Android's installer leaves out: no lib*.so name, a character it does not allow, another ABI, a folder.
        "lib/arm64-v8a/gdbserver": "a program",
        "lib/arm64-v8a/lib bad.so": "a space",
        "lib/armeabi-v7a/libold.so": "32-bit",
        "lib/arm64-v8a/sub/libnested.so": "nested"})

    os.makedirs(run_lib)
    names = app_process_run.install_libraries(fake, godot, run_lib, work)
    check(names == ["libc++_shared.so", "libgodot_android.so"], f"the libraries Android's installer extracts: {names}")
    check(files(run_lib) == names and open(os.path.join(run_lib, "libgodot_android.so")).read() == "godot's ELF",
          f"their ELF originals in the app's lib/arm64: {files(run_lib)}")
    check(all(not os.stat(os.path.join(run_lib, name)).st_mode & 0o222 for name in files(run_lib)),
          "the originals are read-only, as in the version's folder")
    check(files(containers) == [name + ".dylib" for name in names]
          and open(os.path.join(containers, "libc++_shared.so.dylib")).read() == "libc++'s ELF"
          and conversions() == 2, f"a container each beside the run: {files(containers)}, {conversions()} conversions")

    # A start over the same run (again) and a new run converts nothing.
    app_process_run.install_libraries(fake, godot, run_lib, work)
    check(conversions() == 2 and files(run_lib) == names, "a start over the same run converts nothing")
    for name in os.listdir(run_lib):
        os.remove(os.path.join(run_lib, name))
    app_process_run.install_libraries(fake, godot, run_lib, work)
    check(conversions() == 2 and files(run_lib) == names, "a new run converts nothing either")

    # A changed library is converted again, alone; a removed one goes, original and container.
    changed = apk(os.path.join(scratch, "changed.apk"), {"lib/arm64-v8a/libgodot_android.so": "godot's newer ELF"})
    newer_lib = os.path.join(work, "run2", "lib", "arm64")
    os.makedirs(newer_lib)
    names = app_process_run.install_libraries(fake, changed, newer_lib, work)
    check(names == ["libgodot_android.so"] and conversions() == 3
          and open(os.path.join(containers, "libgodot_android.so.dylib")).read() == "godot's newer ELF",
          f"a changed library is converted again: {names}, {conversions()} conversions")
    check(files(containers) == ["libgodot_android.so.dylib"] and files(newer_lib) == ["libgodot_android.so"],
          f"a removed library's container goes: {files(containers)}")

    # An APK without libraries installs nothing and leaves no containers.
    plain = apk(os.path.join(scratch, "plain.apk"), {})
    plain_lib = os.path.join(work, "run3", "lib", "arm64")
    os.makedirs(plain_lib)
    check(app_process_run.install_libraries(fake, plain, plain_lib, work) == [] and files(plain_lib) == []
          and files(containers) == [] and conversions() == 3, "an APK without libraries installs nothing")

    # start(): the command gets the app's library folder only with libraries (stand-ins for every input).
    tree = os.path.join(scratch, "tree")
    inputs = {name: os.path.join(tree, name) for name in ("art", "bionic-lib", "bionic-system", "hwui-lib")}
    for folder in inputs.values():
        os.makedirs(folder)
    os.makedirs(os.path.join(inputs["art"], "lib"))
    os.makedirs(os.path.join(inputs["art"], "javalib"))
    os.makedirs(os.path.join(inputs["bionic-system"], "usr", "share", "zoneinfo"))
    for path in [os.path.join(inputs["art"], "lib", name) for name in
                 [app_process_run.CONSCRYPT_LIBRARY, *app_process_run.PLATFORM_COPIES]] + [
            os.path.join(inputs["art"], "javalib", "conscrypt.jar"),
            os.path.join(inputs["bionic-system"], "usr", "share", "zoneinfo", "tzdata"),
            os.path.join(tree, "icudt75l.dat"), os.path.join(tree, "app_process64"),
            os.path.join(tree, "framework.jar")]:
        with open(path, "w") as handle:
            handle.write(os.path.basename(path))
    # The graphics front libs, which tools/build-front-libs.sh builds beside app_process's build.
    fronts = ("libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so")
    os.makedirs(os.path.join(scratch, "graphics", "guest", "bin"))
    for name in fronts:
        with open(os.path.join(scratch, "graphics", "guest", "bin", name), "w") as handle:
            handle.write(f"{name}'s ELF")
    echo = os.path.join(scratch, "apk-run")
    with open(echo, "w") as handle:
        handle.write('#!/bin/sh\nfor argument in "$@"; do echo "$argument"; done\n')
    os.chmod(echo, os.stat(echo).st_mode | stat.S_IXUSR)

    def started(package_apk, start_work, again=False):
        result = app_process_run.start(inputs["art"], inputs["bionic-lib"], inputs["bionic-system"], inputs["hwui-lib"],
                                       os.path.join(tree, "icudt75l.dat"), fake, echo, start_work,
                                       os.path.join(tree, "app_process64"), os.path.join(tree, "framework.jar"),
                                       package_apk, "io.example.app", "android.app.Application", again=again)
        return result.stdout.splitlines()

    plain_command = started(plain, os.path.join(scratch, "plain-work"))
    entries = [plain_command[i + 1] for i, argument in enumerate(plain_command) if argument == "--libraries"]
    check(len(entries) == 3 and not any("/lib/arm64=" in entry for entry in entries),
          f"without libraries the command's library folders stay those of today: {entries}")
    plain_lib64 = os.path.join(scratch, "plain-work", "system-lib64")
    check([open(os.path.join(plain_lib64, name + ".dylib")).read() if name + ".dylib" in files(plain_lib64) else None
           for name in fronts] == [f"{name}'s ELF" for name in fronts],
          f"the graphics front libs' containers go to /system/lib64: {files(plain_lib64)}")
    godot_work = os.path.join(scratch, "godot-work")
    command = started(godot, godot_work)
    app = app_process_run.code_path(godot, "io.example.app")
    check(f"{app}/lib/arm64={os.path.join(godot_work, 'app-lib64')}" in command,
          "with libraries apk-run gets the app's library folder")
    check(files(os.path.join(godot_work, "run", app.lstrip("/"), "lib", "arm64")) == ["libc++_shared.so",
                                                                                      "libgodot_android.so"],
          "and the run holds their originals")
    count = conversions()
    command = started(godot, godot_work, again=True)
    check(conversions() == count and f"{app}/lib/arm64={os.path.join(godot_work, 'app-lib64')}" in command,
          "a start over the same run converts nothing and keeps the folder")

if failures:
    sys.exit(f"app_process_run_test: {failures} failed")
print("app_process_run_test: convert(), run_app(), install_libraries() and start() fine")
