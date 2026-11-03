#!/usr/bin/env python3
"""The Mac corpus harness: each APK of
a list starts once through app_process, as the Java checks start theirs (framework/tests/app_process_run.py), in a
session at the iPhone's display, inside tools/corpus.sb, and one JSON per app records the levels it reached, its first
failure and every finding by kind, which tools/corpus-gaps.py counts.

The levels, as the plan's section 3 defines them:
  imports    every library of lib/arm64-v8a converted (apk-convert), every refusal kept;
  starts     the launcher activity's window drawn, as ActivityTaskManager logs it ("Displayed <component> for user 0:
             +<time>"), or, for an app without that line, a window's first post on apk-run's headless host; for a
             NativeActivity also its start answered on activity-host ("apk-run: the start was answered", which
             Runtime's gap 4 (a) brings), as it may draw by GLES or not at all;
  frame      after the tap or the swipe, a window's frame that differs from its last one before the touch, or a new
             window that posted (apk-run's "after <step>" lines); a blinking cursor or an animation counts too, and
             under --graphics it is not measured (H2);
  audio, network  not measured until network d);
  lives      the process still running at the end of its time, after a background and foreground round, or ended
             with status 0.
Apps run on apk-run's headless host for now; --graphics runs them on graphics-run, the Mac's ANGLE with the App's
session layers, as tools/surface-check.sh does, where GLES works but no first post is logged yet. A Java app starts
through app_process; an app whose launcher activity is android.app.NativeActivity starts on activity-host without
Java, as the App starts it (GuestStart, ADR-0028), with its manifest's android.app.lib_name and func_name. Each app
runs in a child of this script under sandbox-exec with tools/corpus.sb: it reads the repo, the toolchain and the
system, writes only its run's folder and networks only on loopback, and the child refuses to run an APK when it can
read the user's home folder. An app's run folder goes once its child has ended, unless --keep-work keeps it. --build
takes ART, bionic, hwui, framework-res, ICU's data and the framework from another build folder than the repo's, such
as a trial's before its landing. --check-sandbox shows that the profile holds: no home folder, no address beyond the
host, loopback yes.
Python 3.9.

Usage: corpus-run.py [--seconds <n>] [--native <build dir>] [--build <build folder>] [--graphics] [--keep-work]
                     <list.json> <out folder>
       corpus-run.py --check-sandbox [--without-profile]
A list: {"apks": [{"apk": <path from the repo>, "package"?, "version"?, "url"?, "sha256"?, "license"?}, ...]}.
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("MINATO_REPO") or os.path.dirname(TOOLS)
PROFILE = os.path.join(TOOLS, "corpus.sb")
HOME = os.path.expanduser("~")
CONFIGURATION = "1290x2796@3,en-US"  # the iPhone 15 Pro Max's display, as the Java checks start their sessions
LIFECYCLE = "start,tap:645:1398,swipe:300:1398:1000:1398:200,background,foreground"
FIRST_POST = "apk-run: the first post of window"
# ActivityMetricsLogger's line for a launch, which our ActivityThread logs at the window's first draw.
DISPLAYED = re.compile(r"^I/ActivityTaskManager: Displayed (\S+) for user \d+: (\S+)$")
# apk-run's line once the session's start was answered: every callback of the start returned (Runtime's gap 4 (a)).
START_ANSWERED = "apk-run: the start was answered"
# apk-run's line for each window after a touch step (headless_host.h's Buffers).
TOUCHED = re.compile(r"^apk-run: window (\d+) after (\S+): (\d+) posts since, (\d+) in all, "
                     r"(the frame changed|the frame stayed|a new window)(, removed)?$")
TEST_NET = ("192.0.2.1", 443)  # RFC 5737's TEST-NET-1: nothing answers there, so only a refusal ends a connect fast
# The repo's files that app_process_run.start() hard-links into a run. A link from the repo needs the sandbox's whole
# file-write* on it, which no app gets, so the parent links them into the out folder's shared/ once, and the child
# links from there.
STAGED = ["build/sources/icu/icu4c/source/stubdata/icudt75l.dat", "build/bionic/system/usr/share/zoneinfo/tzdata",
          "build/art/javalib/conscrypt.jar"]

# Each finding's kind and the pattern whose first group names it, in the order a first failure is looked for.
FINDINGS = [
    ("refused", re.compile(r"apk-convert: incompatible: (.*)")),
    ("library", re.compile(r"(UnsatisfiedLinkError: .*|dlopen failed: .*|cannot locate symbol .*)")),
    # A class framework.jar lacks or has as a compile-only stub, as ART names it: "Failed resolution of: L<name>;" at a
    # call site, else the name.
    ("stub class", re.compile(r"NoClassDefFoundError: (?:Failed resolution of: L)?([\w$/.]+)")),
    ("syscall",
     re.compile(r"apkrt: (Linux syscall \d+|fcntl command \d+|ioctl request 0x[0-9a-f]+) is not supported yet")),
    ("unwritten", re.compile(r"Unwritten: (\S+)")),
    # Data the system lacks, which the app runs without (no failure): ICU's time zone files (a tzdata APEX).
    ("data", re.compile(r"(IcuRegistration: no time zone files) were found")),
    ("exception",
     re.compile(r"^(?:[VDIWEF]/[^:]*: )?((?:[a-zA-Z_$][\w$]*\.)+[A-Z][\w$]*(?:Exception|Error)(?:: .*)?)$")),
    ("killed", re.compile(r"Process\s*: Sending signal\. PID: \d+ (SIG: \d+)")),
    # An EGL call the app reports as failed, as GLES apps log it on apk-run's headless host, which has no GLES (H2).
    ("egl", re.compile(r"\b(egl[A-Z]\w*) failed")),
]


def sandbox_command(command, work):
    """`command` under tools/corpus.sb, plus the metadata of the repo's folders in the home folder: realpath, which
    apk-run's mounts call, looks at every folder of a path. The profile for the run is written into `work`."""
    folders, folder = [], os.path.dirname(REPO)
    while folder.startswith(HOME):
        folders.append(folder)
        folder = os.path.dirname(folder)
    profile = os.path.join(work, "corpus-run.sb")
    with open(PROFILE) as source, open(profile, "w") as handle:
        handle.write(source.read())
        if folders:
            literals = " ".join(f'(literal "{folder}")' for folder in folders)
            handle.write(f";; The repo's folders in the home folder, for realpath.\n"
                         f"(allow file-read-metadata {literals})\n")
    return ["sandbox-exec", "-f", profile, "-D", f"HOME={HOME}", "-D", f"REPO={REPO}", "-D", f"WORK={work}"] + command


def check_sandbox(without_profile):
    """Whether the profile holds: home folder unreadable, TEST-NET refused at once, loopback connected."""
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    server.settimeout(10)
    port = server.getsockname()[1]

    def accept():
        try:
            server.accept()[0].close()
        except OSError:
            pass

    acceptor = threading.Thread(target=accept)
    acceptor.start()
    probe = (
        "import os, socket\n"
        f"try:\n    os.listdir({HOME!r}); home = 'readable'\nexcept PermissionError:\n    home = 'refused'\n"
        f"try:\n    socket.create_connection({TEST_NET!r}, 3).close(); net = 'connected'\n"
        "except PermissionError:\n    net = 'refused'\nexcept OSError:\n    net = 'not refused'\n"
        f"try:\n    socket.create_connection(('127.0.0.1', {port}), 3).close(); loop = 'connected'\n"
        "except OSError as error:\n    loop = 'failed: ' + str(error)\n"
        "print(home, net, loop, sep='|')\n")
    work = os.path.join(os.environ.get("TMPDIR", "/tmp"), "corpus-sandbox-check")
    os.makedirs(work, exist_ok=True)
    command = [sys.executable, "-c", probe]
    result = subprocess.run(command if without_profile else sandbox_command(command, work), capture_output=True,
                            text=True)
    acceptor.join()
    server.close()
    home, net, loop = (result.stdout.strip().split("|") + ["?", "?", "?"])[:3]
    print(f"home folder: {home}; an address beyond the host: {net}; loopback: {loop}")
    if result.returncode != 0 or (home, net, loop) != ("refused", "refused", "connected"):
        print("FAIL: the corpus profile does not hold" + (f" ({result.stderr.strip()[:200]})" if result.stderr else ""))
        sys.exit(1)
    print("corpus profile: fine")


def badging(apk):
    """Package, launchable activity and native code of `apk`, from the host's aapt2."""
    output = subprocess.run([os.path.join(REPO, "build", "aapt2", "aapt2"), "dump", "badging", apk],
                            capture_output=True, text=True).stdout
    package = re.search(r"^package: name='([^']+)'.*?versionName='([^']*)'", output, re.M)
    activity = re.search(r"^launchable-activity: name='([^']+)'", output, re.M)
    abis = re.search(r"^native-code: (.*)$", output, re.M)
    return {"package": package.group(1) if package else None, "version": package.group(2) if package else None,
            "activity": activity.group(1) if activity else None,
            "abis": re.findall(r"'([^']+)'", abis.group(1)) if abis else []}


def manifest_tree(apk):
    """The manifest as aapt2 prints its tree: nested {"tag", "attributes", "children"}, attribute values as text."""
    output = subprocess.run([os.path.join(REPO, "build", "aapt2", "aapt2"), "dump", "xmltree", "--file",
                             "AndroidManifest.xml", apk], capture_output=True, text=True).stdout
    root = {"tag": None, "attributes": {}, "children": []}
    stack = [(-1, root)]
    for line in output.splitlines():
        indent, text = len(line) - len(line.lstrip()), line.strip()
        element = re.match(r"E: (\S+)", text)
        attribute = re.match(r"A: (?:[^=]*?:)?(\w+)\(0x[0-9a-f]+\)=(.*)$", text) or re.match(r"A: (\w+)=(.*)$", text)
        if element:
            while stack[-1][0] >= indent:
                stack.pop()
            node = {"tag": element.group(1), "attributes": {}, "children": []}
            stack[-1][1]["children"].append(node)
            stack.append((indent, node))
        elif attribute:
            string = re.match(r'"((?:[^"\\]|\\.)*)"', attribute.group(2))
            stack[-1][1]["attributes"][attribute.group(1)] = string.group(1) if string else attribute.group(2)
    return root


def native_activity(apk):
    """The library and function the launcher NativeActivity loads, as NativeActivity.onCreate reads them from its
    meta-data, with its defaults ("main", "ANativeActivity_onCreate"): from the manifest's first activity named
    android.app.NativeActivity."""
    def walk(node):
        yield node
        for child in node["children"]:
            yield from walk(child)
    values = {}
    for node in walk(manifest_tree(apk)):
        if node["tag"] == "activity" and node["attributes"].get("name") == "android.app.NativeActivity":
            values = {child["attributes"].get("name"): child["attributes"].get("value")
                      for child in node["children"] if child["tag"] == "meta-data"}
            break
    return (values.get("android.app.lib_name") or "main",
            values.get("android.app.func_name") or "ANativeActivity_onCreate")


def start_native_activity(app_process_run, apk, package, apk_convert, apk_run, build, work, run_app):
    """The APK's NativeActivity on activity-host in a session, as GuestStart starts it in the App (ADR-0028, decision
    14): base.apk at its code path, the app's data, external and OBB folders, /system from bionic's, the system's
    libraries (bionic's and the graphics front libs) and the app's, and activity-host with the manifest's library and
    function; `run_app`'s result."""
    library, function = native_activity(apk)
    convert = app_process_run.convert
    system_lib64, app_lib64 = os.path.join(work, "system-lib64"), os.path.join(work, "app-lib64")
    app_process_run.convert_directory(apk_convert, os.path.join(build, "bionic", "lib"), system_lib64)
    app_process_run.convert_directory(apk_convert, os.path.join(build, "graphics", "guest", "bin"), system_lib64)
    host = os.path.join(work, "bin", "activity-host.dylib")
    convert(apk_convert, os.path.join(build, "bionic", "bin", "activity-host"), host)
    app = app_process_run.code_path(apk, package)
    run = os.path.join(work, "run")
    for folder in ("data", "external-data", "external-obb", "lib-arm64"):
        os.makedirs(os.path.join(run, folder), exist_ok=True)
    app_process_run.install_libraries(apk_convert, apk, os.path.join(run, "lib-arm64"), work)
    command = [apk_run, "--mount", f"{app}/base.apk={apk}"]
    for guest, host_folder in ((f"/data/user/0/{package}", "data"), (f"/data/data/{package}", "data"),
                               (f"/storage/emulated/0/Android/data/{package}", "external-data"),
                               (f"/sdcard/Android/data/{package}", "external-data"),
                               (f"/storage/emulated/0/Android/obb/{package}", "external-obb"),
                               (f"/sdcard/Android/obb/{package}", "external-obb")):
        command += ["--mount", f"{guest}={os.path.join(run, host_folder)}"]
    command += ["--mount", f"/system={os.path.join(build, 'bionic', 'system')}",
                "--configuration", CONFIGURATION, "--lifecycle", LIFECYCLE,
                "--libraries", f"/system/lib64={system_lib64}", "--libraries", f"{app}/lib/arm64={app_lib64}",
                "--cwd", "/", "--app-environment", "--env", f"LD_LIBRARY_PATH={app}/lib/arm64",
                "--argv0", "/system/bin/activity-host", host,
                "--package", package, "--apk", f"{app}/base.apk", "--library", library, "--function", function]
    return run_app(command)


def findings_of(text):
    found = {kind: [] for kind, _ in FINDINGS}
    first = None
    for line in text.splitlines():
        line = line.rstrip()
        for kind, pattern in FINDINGS:
            match = pattern.search(line)
            if match:
                name = match.group(1)
                if name not in found[kind]:
                    found[kind].append(name)
                if first is None and kind not in ("unwritten", "data"):
                    first = {"kind": kind, "text": name}
                break
    return found, first


def duration_ms(text):
    """TimeUtils.formatDuration's text as milliseconds ("+1s234ms" is 1234), or None."""
    if text == "0":
        return 0
    match = re.fullmatch(r"([+-])(?:(\d+)d)?(?:(\d+)h)?(?:(\d+)m(?!s))?(?:(\d+)s)?(\d+)ms", text)
    if not match:
        return None
    days, hours, minutes, seconds, millis = (int(group or 0) for group in match.groups()[1:])
    total = (((days * 24 + hours) * 60 + minutes) * 60 + seconds) * 1000 + millis
    return -total if match.group(1) == "-" else total


def run_one(entry, out, seconds, native, build, graphics):
    """In the sandbox: the levels, findings and log of one APK, as JSON in `out`."""
    try:
        os.listdir(HOME)
        raise SystemExit("refused: the user's home folder is readable, so this is not inside tools/corpus.sb")
    except PermissionError:
        pass
    sys.path.insert(0, os.path.join(REPO, "framework", "tests"))
    import app_process_run

    real_link = os.link

    def link(source, target, *args, **kwargs):
        """os.link, with a source in the repo, also behind a link, from the parent's shared/ copy (STAGED), else
        copied."""
        source = os.path.realpath(os.fspath(source))
        if source.startswith(REPO + os.sep):
            staged = os.path.join(out, "shared", os.path.relpath(source, REPO))
            if not os.path.exists(staged):
                shutil.copyfile(source, target)
                return None
            source = staged
        return real_link(source, target, *args, **kwargs)

    os.link = link

    apk = os.path.join(REPO, entry["apk"])
    name = os.path.splitext(os.path.basename(apk))[0]
    work = os.path.join(out, "work", name)
    record = {"apk": entry["apk"], "levels": {}, "findings": {}, "first_failure": None}
    record.update(badging(apk))
    started = time.monotonic()

    # Imports: every library Android's installer takes, through the converter.
    refusals = []
    with zipfile.ZipFile(apk) as archive:
        libraries = [item for item in archive.namelist()
                     if item.startswith(app_process_run.APP_ABI) and item.endswith(".so")]
        os.makedirs(os.path.join(work, "import"), exist_ok=True)
        for library in libraries:
            elf = os.path.join(work, "import", os.path.basename(library))
            with open(elf, "wb") as handle:
                handle.write(archive.read(library))
            try:
                app_process_run.convert(os.path.join(REPO, native, "importer", "converter", "apk-convert"), elf,
                                        elf + ".dylib")
            except SystemExit as refusal:
                refusals.append(f"apk-convert: incompatible: {os.path.basename(library)}: {refusal}")
    record["levels"]["imports"] = not refusals and record["package"] is not None
    log = "\n".join(refusals)

    result, kept = None, {}
    if record["levels"]["imports"]:
        def run_app(command, last=None, grace=2.0, timeout=300):
            """app_process_run.run_app's run for the corpus: `seconds` of it, its output kept at the end."""
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                       errors="replace")
            output = {"stdout": [], "stderr": []}
            readers = [threading.Thread(target=lambda n=n: output[n].extend(getattr(process, n))) for n in output]
            for reader in readers:
                reader.start()
            try:
                status = process.wait(seconds)
                kept["alive_at_end"] = False
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                status = 0
                kept["alive_at_end"] = True
            for reader in readers:
                reader.join()
            return subprocess.CompletedProcess(command, status, "".join(output["stdout"]), "".join(output["stderr"]))

        app_process_run.run_app = run_app
        build = os.path.abspath(build)
        mounts = [("/system/framework/framework-res", os.path.join(build, "framework-res", "framework-res"))]
        apk_convert = os.path.join(REPO, native, "importer", "converter", "apk-convert")
        apk_run = os.path.join(REPO, native, *(("graphics", "tests", "graphics-run") if graphics
                                               else ("runtime", "apk-run")))
        try:
            if record["activity"] == "android.app.NativeActivity":
                record["host"] = "activity-host"
                result = start_native_activity(app_process_run, apk, record["package"], apk_convert, apk_run, build,
                                               os.path.join(work, "run"), run_app)
            else:
                record["host"] = "app_process"
                result = app_process_run.start(
                    os.path.join(build, "art"), os.path.join(build, "bionic", "lib"),
                    os.path.join(build, "bionic", "system"), os.path.join(build, "hwui", "lib"),
                    os.path.join(build, "sources", "icu", "icu4c", "source", "stubdata", "icudt75l.dat"),
                    apk_convert, apk_run, os.path.join(work, "run"),
                    os.path.join(build, "framework-native", "app_process64"),
                    os.path.join(build, "framework", "framework.jar"), apk, record["package"], None, mounts=mounts,
                    configuration=CONFIGURATION, lifecycle=LIFECYCLE)
        except SystemExit as error:
            log += f"\n{error}"
    if result is not None:
        log += "\n" + result.stdout + "\n" + result.stderr
        record["status"] = result.returncode
        record["alive_at_end"] = kept.get("alive_at_end", False)
    lines = log.splitlines()
    displayed = next((match for match in map(DISPLAYED.match, lines) if match), None)
    if displayed:
        record["displayed"] = {"component": displayed.group(1), "ms": duration_ms(displayed.group(2))}
    touched = [match for match in map(TOUCHED.match, lines) if match]
    record["touches"] = [{"step": match.group(2), "window": int(match.group(1)), "since": int(match.group(3)),
                          "posts": int(match.group(4)), "frame": match.group(5), "removed": bool(match.group(6))}
                         for match in touched]
    starts = (displayed is not None or FIRST_POST in log or
              record.get("host") == "activity-host" and START_ANSWERED in lines)
    frame = any(match.group(5) == "the frame changed" or (match.group(5) == "a new window" and match.group(3) != "0")
                for match in touched)
    record["levels"].update({
        "starts": starts,
        "frame": None if graphics else frame,
        "audio": None,
        "network": None,
        "lives": bool(result is not None and starts and
                      (record["alive_at_end"] or result.returncode == 0) and result.returncode >= 0),
    })
    record["findings"], record["first_failure"] = findings_of(log)
    if result is not None and result.returncode < 0:
        record["findings"].setdefault("signal", []).append(f"signal {-result.returncode}")
        record["first_failure"] = record["first_failure"] or {"kind": "signal", "text": f"signal {-result.returncode}"}
    record["seconds"] = round(time.monotonic() - started, 1)
    with open(os.path.join(out, f"{name}.log"), "w") as handle:
        handle.write(log)
    with open(os.path.join(out, f"{name}.json"), "w") as handle:
        json.dump(record, handle, indent=2)
        handle.write("\n")


def main():
    arguments = sys.argv[1:]
    if arguments[:1] == ["--check-sandbox"]:
        check_sandbox("--without-profile" in arguments)
        return
    if arguments[:1] == ["--one"]:
        entry, out, seconds, native = json.loads(arguments[1]), arguments[2], float(arguments[3]), arguments[4]
        run_one(entry, out, seconds, native, arguments[5], arguments[6:] == ["--graphics"])
        return
    # build/native/macos: Runtime's rollout folder, with apk-run, graphics-run and apk-convert of main's HEAD.
    seconds, native, graphics, keep_work = 120.0, os.path.join("build", "native", "macos"), False, False
    build = os.path.join(REPO, "build")
    while arguments[:1] in (["--seconds"], ["--native"], ["--build"], ["--graphics"], ["--keep-work"]):
        if arguments[0] == "--graphics":
            graphics, arguments = True, arguments[1:]
            continue
        if arguments[0] == "--keep-work":
            keep_work, arguments = True, arguments[1:]
            continue
        if arguments[0] == "--seconds":
            seconds = float(arguments[1])
        elif arguments[0] == "--build":
            build = os.path.abspath(arguments[1])
        else:
            native = arguments[1]
        arguments = arguments[2:]
    if len(arguments) != 2:
        raise SystemExit(__doc__.split("Usage: ")[1].split("\n\n")[0])
    listing, out = arguments
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    with open(listing) as handle:
        entries = json.load(handle)["apks"]
    # graphics-run's providers on the Mac's ANGLE, with the App's session layers and no window shown, as
    # tools/surface-check.sh runs its app.
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", MINATO_REPO=REPO, MINATO_SESSION_LAYERS="1",
                       MINATO_WINDOW_WATCH="hidden",
                       MINATO_ANGLE=os.path.join(REPO, "build", "angle", "angle", "out", "macos", "libGLESv2.dylib"))
    for path in STAGED:
        staged = os.path.join(out, "shared", path)
        if os.path.exists(os.path.join(REPO, path)) and not os.path.exists(staged):
            os.makedirs(os.path.dirname(staged), exist_ok=True)
            os.link(os.path.join(REPO, path), staged)
    for entry in entries:
        command = sandbox_command([sys.executable, os.path.abspath(__file__), "--one", json.dumps(entry), out,
                                   str(seconds), native, build] + (["--graphics"] if graphics else []), out)
        child = subprocess.run(command, env=environment, capture_output=True, text=True)
        name = os.path.splitext(os.path.basename(entry["apk"]))[0]
        if not keep_work:
            # The run's converted libraries and folders, about 70 MB an app, also after a child that died.
            shutil.rmtree(os.path.join(out, "work", name), ignore_errors=True)
        try:
            with open(os.path.join(out, f"{name}.json")) as handle:
                levels = json.load(handle)["levels"]
            shown = ", ".join(f"{level} {'yes' if value else 'no' if value is False else '-'}"
                              for level, value in levels.items())
            print(f"{name}: {shown}", flush=True)
        except (OSError, ValueError, KeyError):
            print(f"{name}: no result ({child.returncode}): {child.stderr.strip()[-300:]}", flush=True)


if __name__ == "__main__":
    main()
