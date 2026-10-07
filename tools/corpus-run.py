#!/usr/bin/env python3
"""The Mac corpus harness (docs/plans/2026-10-07-corpus-harness.md, section 3 of the compatibility plan): each APK of
a list starts once through app_process, as the Java checks start theirs (framework/tests/app_process_run.py), in a
session at the iPhone's display, inside tools/corpus.sb, and one JSON per app records the levels it reached, its first
failure and every finding by kind, which tools/corpus-gaps.py counts.

The levels, as the plan's section 3 defines them:
  imports    every library of lib/arm64-v8a converted (apk-convert), every refusal kept;
  starts     the first post of a window on apk-run's headless host, until the activity controller logs "Displayed";
  frame      the first frame, then input that changes it: not measured until apk-run counts posts and compares the
             frame after a touch;
  audio, network  not measured until network d);
  lives      the process still running at the end of its time, after a background and foreground round, or ended
             with status 0.
Apps run on apk-run's headless host for now; --graphics runs them on graphics-run, the Mac's ANGLE with the App's
session layers, as tools/surface-check.sh does, where GLES works but no first post is logged yet. Each app runs in a
child of this script under sandbox-exec with tools/corpus.sb: it reads the repo, the toolchain and the system, writes
only its run's folder and networks only on loopback, and the child refuses to run an APK when it can read the user's
home folder. --check-sandbox shows that the profile holds: no home folder, no address beyond the host, loopback yes.
Python 3.9.

Usage: corpus-run.py [--seconds <n>] [--native <build dir>] [--graphics] <list.json> <out folder>
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
    ("stub class", re.compile(r"NoClassDefFoundError: (\S+)")),
    ("syscall",
     re.compile(r"apkrt: (Linux syscall \d+|fcntl command \d+|ioctl request 0x[0-9a-f]+) is not supported yet")),
    ("unwritten", re.compile(r"Unwritten: (\S+)")),
    ("exception",
     re.compile(r"^(?:[VDIWEF]/[^:]*: )?((?:[a-zA-Z_$][\w$]*\.)+[A-Z][\w$]*(?:Exception|Error)(?:: .*)?)$")),
    ("killed", re.compile(r"Process\s*: Sending signal\. PID: \d+ (SIG: \d+)")),
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
                if first is None and kind != "unwritten":
                    first = {"kind": kind, "text": name}
                break
    return found, first


def run_one(entry, out, seconds, native, graphics):
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
        """os.link, with a source in the repo taken from the parent's shared/ copy (STAGED), else copied."""
        source = os.fspath(source)
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
        build = os.path.join(REPO, "build")
        mounts = [("/system/framework/framework-res", os.path.join(build, "framework-res", "framework-res"))]
        try:
            result = app_process_run.start(
                os.path.join(build, "art"), os.path.join(build, "bionic", "lib"),
                os.path.join(build, "bionic", "system"), os.path.join(build, "hwui", "lib"),
                os.path.join(build, "sources", "icu", "icu4c", "source", "stubdata", "icudt75l.dat"),
                os.path.join(REPO, native, "importer", "converter", "apk-convert"),
                os.path.join(REPO, native, *(("graphics", "tests", "graphics-run") if graphics
                                             else ("runtime", "apk-run"))),
                os.path.join(work, "run"),
                os.path.join(build, "framework-native", "app_process64"),
                os.path.join(build, "framework", "framework.jar"), apk, record["package"], None, mounts=mounts,
                configuration=CONFIGURATION, lifecycle=LIFECYCLE)
        except SystemExit as error:
            log += f"\n{error}"
    if result is not None:
        log += "\n" + result.stdout + "\n" + result.stderr
        record["status"] = result.returncode
        record["alive_at_end"] = kept.get("alive_at_end", False)
    starts = FIRST_POST in log
    record["levels"].update({
        "starts": starts,
        "frame": None,
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
        run_one(entry, out, seconds, native, arguments[5:] == ["--graphics"])
        return
    # build/native/macos: Runtime's rollout folder, with apk-run, graphics-run and apk-convert of main's HEAD.
    seconds, native, graphics = 120.0, os.path.join("build", "native", "macos"), False
    while arguments[:1] in (["--seconds"], ["--native"], ["--graphics"]):
        if arguments[0] == "--graphics":
            graphics, arguments = True, arguments[1:]
            continue
        if arguments[0] == "--seconds":
            seconds = float(arguments[1])
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
                                   str(seconds), native] + (["--graphics"] if graphics else []), out)
        child = subprocess.run(command, env=environment, capture_output=True, text=True)
        name = os.path.splitext(os.path.basename(entry["apk"]))[0]
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
