#!/usr/bin/env python3
"""ADR-0039's Java app tests on the Mac after test 1 (b3.3's asset-manager-test, b3.4a's resources-test, b3.4c's
config-test): app_process starts the app as test 1 starts its app (framework/tests/app_process_run.py's start()), with
framework-res as the folder /system/framework/framework-res (decision 8) and, since b3.5b1, the platform's
/system/etc/permissions/platform.xml (tools/fetch-sources.py --set framework), whose split permissions ManifestPackage
reads; while it is not fetched, the app starts without them, as Android starts without a missing file. The app's
Application class reads
resources through framework.jar and libandroid_runtime.so and prints what the sources say
(fixtures/expected/<app>.txt), or logs what Android 15 logs (fixtures/expected/<app>.logcat).

A .txt file's lines are the app's System.out lines without "<app>: ", in order; a line that starts with "logcat "
must be a line of the process's log (apk-run's stderr), anywhere. A .logcat file, from tools/android-reference.sh,
holds the app's log lines with its own tag, which the process's log must hold in that order; those with the tag
<app>-device differ by device and are only shown. A configuration begins a session (apk-run --configuration), whose
host plays the app's lifecycle: apk-run's --lifecycle steps, "start" unless --lifecycle gives others, and the quit
after the app's finish (ADR-0043 step c1). Prints "skipped: ..." while a part is not built. Without a
configuration there is no session and no frame clock: the app's first traversal then leaves ViewRootImpl's sync barrier
at the head of the main queue, and the main Looper dispatches no post from another thread and no delayed post, so a
check that needs it passes a configuration (main-looper-check, autocomplete-check).

Since step c2b the app process runs its main Looper and never ends by itself. Every Java test ends with the line
"<app>: done", in System.out or in its log, and the run ends 2 s after it, as tools/android-reference.sh stops the app
on the AVD after that line (app_process_run.run_app). With --until <text>, the run ends 2 s after the app's line
"<app>: <text>" instead, and only the reference's lines up to its first such line are compared, with ours up to ours:
a start that cannot reach the reference's end, such as one without a session (ADR-0043 step c1).

With --drawn, the run must have drawn its window: apk-run's headless buffers log the first post of each window
(ADR-0043 step c3), which the process's log must hold.

With --in-order <text>, once or more, lines of the process's log must start with these texts in this order, among
its other lines: a host's lines among the app's, such as graphics-run's window host's (ADR-0043 c4 (c)).

With --env <name>=<value>, once or more, the app starts with this entry in its environment, as GuestStart adds a
run's extra entries (the gap scanner's MINATO_GAPSCAN=1).

With --once <line>, once or more, the process's log must hold this line exactly once: a line logged once however
often its cause comes, such as ART's log of a failed resolution (minato-resolve).

With --status <n>, the run must end by itself with status n, where every other run ends with 0: an app whose exception
no thread catches ends with SIGKILL from Android's handler, 137 (128 + 9) through apk-run's session (ADR-0039,
crash-test).

With --android-id, the app starts with ActivityThread's --android-id, the value its settings provider gives as
android_id, as the host gives it (ADR-0045, gap 13a).

With --obb <file>, once or more, the file lies in the app's OBB folder before the start, as a user or the library puts
one there (ADR-0049 test 8).

With --pick <file>, once or more, the file is what the user picks when the app asks for a document (ADR-0057 step b):
the session's host clones it into the app's Download folder, /storage/emulated/0/Download, answers the picker's
request with the clone and writes the manifest of the clones' names into the app's data folder, as a host does
(apk-run --pick-file); a request that allows one pick gets the first file, one that allows several all of them.

With --pick-photo <file>, once or more, the same for the photo picker (ADR-0057 step c): the host clones up to the
request's maximum of the files into the folder .minato/photos of the app's data folder and writes the manifest.

With --pick-save, the session's host answers an ACTION_CREATE_DOCUMENT request as a user who saves into the app's
Download folder: it creates the file there under the request's title, made unique (ADR-0057 step d). With --pick-tree
<folder>, it answers an ACTION_OPEN_DOCUMENT_TREE request with that folder, copied into Download before the start.

With --packs <file>, once or more, the file lies read-only in the folder local_testing of the app's external files
folder before the start, as the library lays out an app's kept pack slices there, and ActivityThread gets
--local-testing-dir local_testing (ADR-0050 test 2).

With --split <file>, once or more, the file is a split of the app's set, named as an install names it
(split_<split name>.apk), copied beside base.apk, whose libraries are installed too, and ActivityThread gets
--split <path> for each, as GuestStart passes them (ADR-0048 c)).

With --apk-inspect, the app starts with its signers.json (apk-inspect --signers), as the library writes it at the
import, and each certificate SHA-256 the APK's check proved becomes framework/tests/certificate_mask.py's mark of its
role in the app's lines, as tools/android-reference.sh marks Android's (b3.5b1). With --restart, the app starts a
second time over the first start's folders, as tools/android-reference.sh starts it again on the AVD, and the two
starts' lines count as one log (step c1's context-test).

Usage: java_app_run.py [--apk-inspect <apk-inspect>] [--restart] [--lifecycle <steps>] [--until <text>] [--drawn]
                       [--android-id <16 hex digits>] [--in-order <text>]... [--obb <file>]... [--packs <file>]...
                       [--pick <file>]... [--pick-photo <file>]...
                       [--pick-save] [--pick-tree <folder>]
                       [--status <n>] [--once <line>]... [--env <name>=<value>]... [--split <file>]...
                       <app> <package> <application class> <art> <bionic-lib> <bionic-system> <hwui-lib> <icu-data>
                       <apk-convert> <apk-run> <work> <app_process64> <framework.jar> <framework-res folder> <app.apk>
                       <expected> [<configuration>]
"""
import os
import re
import subprocess
import sys

import certificate_mask
from app_process_run import missing, start

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# apk-run's line at the first post of a window into its headless buffers (runtime/tools/apk_run.cpp).
FIRST_POST = "apk-run: the first post of window "
PERMISSIONS = os.path.join(ROOT, "build", "sources", "frameworks_base_etc", "data", "etc")


def main():
    args = sys.argv[1:]
    apk_inspect = None
    restart = False
    lifecycle = None
    until = None
    drawn = False
    android_id = None
    in_order = []
    obbs = []
    packs = []
    picks = []
    photos = []
    saves = False
    tree = None
    splits = []
    status = 0
    once = []
    environment = []
    while args[:1] in (["--apk-inspect"], ["--restart"], ["--lifecycle"], ["--until"], ["--drawn"], ["--android-id"],
                       ["--in-order"], ["--obb"], ["--packs"], ["--pick"], ["--pick-photo"], ["--pick-save"],
                       ["--pick-tree"], ["--status"], ["--once"], ["--env"], ["--split"]):
        if args[0] == "--restart":
            restart, args = True, args[1:]
        elif len(args) > 1 and args[0] == "--android-id":
            android_id, args = args[1], args[2:]
        elif len(args) > 1 and args[0] == "--obb":
            obbs, args = obbs + [args[1]], args[2:]
        elif args[0] == "--pick-save":
            saves, args = True, args[1:]
        elif len(args) > 1 and args[0] == "--pick-tree":
            tree, args = args[1], args[2:]
        elif len(args) > 1 and args[0] == "--pick-photo":
            photos, args = photos + [args[1]], args[2:]
        elif len(args) > 1 and args[0] == "--pick":
            picks, args = picks + [args[1]], args[2:]
        elif len(args) > 1 and args[0] == "--packs":
            packs, args = packs + [args[1]], args[2:]
        elif len(args) > 1 and args[0] == "--split":
            splits, args = splits + [args[1]], args[2:]
        elif len(args) > 1 and args[0] == "--env":
            environment, args = environment + [args[1]], args[2:]
        elif len(args) > 1 and args[0] == "--once":
            once, args = once + [args[1]], args[2:]
        elif len(args) > 1 and args[0] == "--status":
            status, args = int(args[1]), args[2:]
        elif args[0] == "--drawn":
            drawn, args = True, args[1:]
        elif len(args) > 1 and args[0] == "--in-order":
            in_order, args = in_order + [args[1]], args[2:]
        elif len(args) > 1 and args[0] == "--lifecycle":
            lifecycle, args = args[1], args[2:]
        elif len(args) > 1 and args[0] == "--until":
            until, args = args[1], args[2:]
        elif len(args) > 1:
            apk_inspect, args = args[1], args[2:]
        else:
            break
    if len(args) not in (16, 17):
        raise SystemExit(__doc__)
    (app, package, application, art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, apk_run, work,
     app_process, framework_jar, folder, apk, expected) = args[:16]
    configuration = args[16] if len(args) == 17 else None
    if os.path.isfile(hwui_lib):
        # 16 arguments were also the layout before <hwui-lib> with a configuration, which put ICU's data file here.
        raise SystemExit(f"java_app_run.py: <hwui-lib> is a file: {hwui_lib}\n{__doc__}")
    skipped = missing(art, bionic_system, hwui_lib, app_process, framework_jar, apk, app)
    if not skipped and not os.path.isdir(folder):
        skipped = f"skipped: {folder} is not built (tools/build-framework-res.sh)"
    if skipped:
        print(skipped)
        return
    mounts = [("/system/framework/framework-res", folder)]
    if os.path.isfile(os.path.join(PERMISSIONS, "platform.xml")):
        mounts.append(("/system/etc/permissions", PERMISSIONS))
    signers = None
    if apk_inspect:
        os.makedirs(work, exist_ok=True)
        signers = os.path.join(work, "signers.json")
        subprocess.run([apk_inspect, "--signers", signers, apk], check=True, capture_output=True)
        if not os.path.exists(signers):
            signers = None  # an unsigned APK has none
    results = []
    for again in ((False, True) if restart else (False,)):
        result = start(art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, apk_run, work, app_process,
                       framework_jar, apk, package, application, mounts=mounts, configuration=configuration,
                       signers=signers, again=again,
                       last=rf"^(?:[VDIWEF]/)?{re.escape(app)}: {re.escape(until or 'done')}$", lifecycle=lifecycle,
                       android_id=android_id, obbs=obbs, packs=packs, environment=environment, splits=splits,
                       picks=picks, photos=photos, saves=saves, tree=tree)
        if result is None:
            print("FAIL: no end within 300 s" + (" in the second start" if again else ""))
            sys.exit(1)
        results.append(result)
    # One log of the starts: the first start's failure counts.
    result = subprocess.CompletedProcess(results[0].args, next((r.returncode for r in results if r.returncode), 0),
                                         "".join(r.stdout for r in results), "".join(r.stderr for r in results))
    if drawn and not any(line.startswith(FIRST_POST) for line in result.stderr.splitlines()):
        print("FAIL: no post of the session's window into apk-run's headless buffers (ADR-0043 step c3)")
        sys.exit(1)
    found = in_order_found(result.stderr.splitlines(), in_order)
    if found < len(in_order):
        print("FAIL: the process's log lacks these lines in this order:")
        for index, text in enumerate(in_order):
            print(f"  {'found' if index < found else 'missing'}: {text}")
        print("stderr, last 40 lines:")
        print("\n".join(result.stderr.splitlines()[-40:]))
        sys.exit(1)
    counts = [(line, result.stderr.splitlines().count(line)) for line in once]
    if any(count != 1 for _, count in counts):
        print("FAIL: the process's log must hold these lines once:")
        for line, count in counts:
            print(f"  {count} times: {line}")
        sys.exit(1)
    if expected.endswith(".logcat"):
        marks = certificate_mask.marks(apk_inspect, apk) if apk_inspect else {}
        compare_log(app, result, expected, marks, until, status)
        return

    with open(expected, encoding="utf-8") as handle:
        lines = [line for line in handle.read().splitlines() if line and not line.startswith("#")]
    wanted = [line for line in lines if not line.startswith("logcat ")]
    wanted_log = [line[len("logcat "):] for line in lines if line.startswith("logcat ")]
    prefix = f"{app}: "
    got = [line[len(prefix):] for line in result.stdout.splitlines() if line.startswith(prefix)]
    log = result.stderr.splitlines()
    missing_log = [line for line in wanted_log if line not in log]
    if result.returncode == status and got == wanted and not missing_log:
        print(f"{app} read its resources as their sources say, {len(got)} lines and {len(wanted_log)} log lines")
        return
    print(f"FAIL: exit status {result.returncode}")
    for line in wanted:
        if line not in got:
            print(f"  only expected: {line}")
    for line in got:
        if line not in wanted:
            print(f"  only ours:     {line}")
    for line in missing_log:
        print(f"  not in the log: {line}")
    print("stderr, last 40 lines:")
    print("\n".join(log[-40:]))
    sys.exit(1)


def in_order_found(log, texts):
    """How many of `texts`, from the first, start lines of `log` in their order."""
    found = 0
    for line in log:
        if found < len(texts) and line.startswith(texts[found]):
            found += 1
    return found


def until_line(lines, app, until):
    """`lines` up to and including the first line "<app>: <until>", or all of them without `until`."""
    if until is None:
        return lines
    for index, line in enumerate(lines):
        if re.match(rf"^[VDIWEF]/{re.escape(app)}: {re.escape(until)}$", line):
            return lines[:index + 1]
    return lines


def compare_log(app, result, expected, marks, until=None, status=0):
    """The process's log lines with the app's tag against Android's, in order, with the APK's proven certificates
    marked (certificate_mask's marks); the device's lines are shown. With `until`, both up to their first line
    "<app>: <until>". The run must have ended with `status`."""
    with open(expected, encoding="utf-8") as handle:
        wanted = until_line(handle.read().splitlines(), app, until)
    log = certificate_mask.mask(result.stderr, marks).splitlines()
    got = until_line([line for line in log if re.match(rf"^[VDIWEF]/{re.escape(app)}: ", line)], app, until)
    device = [line for line in log if re.match(rf"^[VDIWEF]/{re.escape(app)}-device: ", line)]
    if result.returncode == status and got == wanted:
        print(f"{app} logged what Android 15 logs, {len(got)} lines" + (f", and ended with status {status}" if status else ""))
        for line in device:
            print(f"  device: {line}")
        return
    print(f"FAIL: exit status {result.returncode}")
    for line in wanted:
        if line not in got:
            print(f"  only Android's: {line}")
    for line in got:
        if line not in wanted:
            print(f"  only ours:      {line}")
    print("stderr, last 40 lines:")
    print("\n".join(log[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
