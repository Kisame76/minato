#!/usr/bin/env python3
"""Fetches and builds ANGLE's libGLESv2 for Minato (ADR-0029), below build/angle/.

Usage: tools/build-angle.sh --self-test          check the script's guards (no network; needs the checkout's depot_tools)
       tools/build-angle.sh --fetch              fetch the pinned checkout (the only step that uses the network)
       tools/build-angle.sh --gen [slice...]     gn gen for the slices (ios, ios-simulator, macos; all by default)
       tools/build-angle.sh --build [slice...]   patches, gn gen, ninja, the checks and build/ANGLE.xcframework
       tools/build-angle.sh --licenses slice     the licenses of what a built slice holds (decision 11)

The pins are the "angle" entry of third_party/sources.json. ANGLE and depot_tools come as single commits
through plain git. gclient.py of that depot_tools, run with this Python rather than through vpython3, so
that depot_tools installs nothing, syncs the git dependencies of ANGLE's DEPS that the entry keeps; the
other git dependencies are left out, and so is every CIPD and GCS entry. The compiler, GN and the two
Khronos XML files are fetched here and checked against their digests. The script writes only below build/angle/, and before every git command that
writes it checks that the repository is the nested one, never Minato's own tree.
"""
import argparse
import ast
import base64
import hashlib
import json
import os
import pprint
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "angle")
CHECKOUT = os.path.join(BUILD, "angle")
STAMP = os.path.join(BUILD, "stamp.json")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
ARGUMENTS = os.path.join(ROOT, "third_party", "angle", "args")
PATCHES = os.path.join(ROOT, "third_party", "angle", "patches")
STRIPPED = os.path.join(BUILD, "stripped")
# The one output outside build/angle/ (ADR-0029, decision 5), which the app build embeds.
XCFRAMEWORK = os.path.join(ROOT, "build", "ANGLE.xcframework")
# Each slice's library in its output directory.
LIBRARIES = {"ios": "libGLESv2.framework", "ios-simulator": "libGLESv2.framework", "macos": "libGLESv2.dylib"}
APP_DEPLOYMENT_TARGET = (26, 0)
SLICES = ["ios", "ios-simulator", "macos"]

# No self-update and no usage reports (ADR-0029, decision 2). depot_tools stays off PATH, so that
# nothing can start its vpython3 or cipd, which would install themselves first.
ENVIRONMENT = dict(os.environ, DEPOT_TOOLS_UPDATE="0", DEPOT_TOOLS_METRICS="0", DEPOT_TOOLS_COLLECT_METRICS="0")


def pins():
    with open(MANIFEST) as handle:
        return json.load(handle)["angle"]


def inside(path, base=None):
    real, base = os.path.realpath(path), os.path.realpath(BUILD if base is None else base)
    return real == base or real.startswith(base + os.sep)


def require_inside(path):
    if not (inside(path) or inside(path, XCFRAMEWORK)):
        raise SystemExit(f"build-angle: refusing to write outside {BUILD} and {XCFRAMEWORK}: {path}")


def require_nested(repository):
    """ADR-0029, decision 8: the repository's top level must be exactly this directory. With a missing
    clone or a wrong path, git would otherwise act on Minato's own tree and the other chats' work."""
    require_inside(repository)
    result = subprocess.run(["git", "-C", repository, "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    top = result.stdout.strip()
    if result.returncode or os.path.realpath(top) != os.path.realpath(repository):
        raise SystemExit(f"build-angle: {repository} is not a repository of its own (top level: {top or 'none'}); stopping")


def run(command, **options):
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, env=ENVIRONMENT, **options)


def output(command, **options):
    return subprocess.run(command, check=True, env=ENVIRONMENT, capture_output=True, text=True, **options).stdout.strip()


def fetch_commit(directory, url, commit):
    """One commit without history, through plain git."""
    require_inside(directory)
    os.makedirs(directory, exist_ok=True)
    if not os.path.isdir(os.path.join(directory, ".git")):
        run(["git", "init", "-q", directory])
    require_nested(directory)
    head = subprocess.run(["git", "-C", directory, "rev-parse", "-q", "--verify", "HEAD"], capture_output=True, text=True)
    if head.stdout.strip() == commit:
        return
    run(["git", "-C", directory, "fetch", "-q", "--depth", "1", "--no-tags", url, commit])
    require_nested(directory)
    run(["git", "-C", directory, "checkout", "-q", "--detach", "FETCH_HEAD"])
    if output(["git", "-C", directory, "rev-parse", "HEAD"]) != commit:
        raise SystemExit(f"build-angle: {directory} is not at {commit} after the fetch")


def dependency_names(deps_file):
    """The keys of the DEPS file's `deps` dictionary. DEPS is Python from the network, so it is parsed
    as gclient parses it (gclient_eval), never run: the keys must be string literals."""
    with open(deps_file) as handle:
        tree = ast.parse(handle.read(), deps_file)
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and target.id == "deps"
                                                for target in node.targets):
            if not isinstance(node.value, ast.Dict):
                raise SystemExit(f"build-angle: deps in {deps_file} is not a dictionary")
            names = set()
            for key in node.value.keys:
                if not (isinstance(key, ast.Constant) and isinstance(key.value, str)):
                    raise SystemExit(f"build-angle: a key of deps in {deps_file} is not a string literal")
                names.add(key.value)
            return names
    raise SystemExit(f"build-angle: {deps_file} assigns no deps")


def write_gclient(directory, settings, url):
    """The .gclient of the solution in `directory`: every git entry of its DEPS that the entry does not
    keep is None. CIPD and GCS entries are not named here: gclient names them "path:package" and
    "path:object", and a None would not stop a CIPD package anyway, so IGNORED_TYPES leaves them out."""
    names = dependency_names(os.path.join(directory, "DEPS"))
    keep = set(settings["keep"])
    for name in sorted(keep):
        recursed = any(name.startswith(parent + "/") for parent in keep if parent in names)
        if name not in names and not recursed:
            raise SystemExit(f"build-angle: the kept dependency {name} is not in the DEPS of {directory}")
    left_out = {name: None for name in names - keep}
    left_out.update({name: None for name in settings["left_out_recursed"]})
    solution = {
        "name": ".",
        "url": url,
        "deps_file": "DEPS",
        "managed": False,
        "custom_deps": dict(sorted(left_out.items())),
        "custom_vars": settings["custom_vars"],
        "custom_hooks": [{"name": name} for name in settings["suppressed_hooks"]],
    }
    with open(os.path.join(directory, ".gclient"), "w") as handle:
        handle.write("# Written by tools/build-angle.py (ADR-0029, decision 2).\n")
        handle.write("solutions = " + pprint.pformat([solution], width=100) + "\n")
        handle.write("target_os = " + repr(settings["target_os"]) + "\n")
    return len(left_out)


# gclient skips every CIPD and GCS entry of DEPS and of the DEPS files it recurses into; the compiler and
# GN, the only such entries the build needs, come from fetch_file() instead.
IGNORED_TYPES = ["--ignore-dep-type", "cipd", "--ignore-dep-type", "gcs"]


def gclient(directory, *arguments, capture=False):
    command = [sys.executable, os.path.join(CHECKOUT, "third_party", "depot_tools", "gclient.py"), *arguments]
    if capture:
        return output(command, cwd=directory)
    run(command, cwd=directory)


def revinfo(directory, ignored=IGNORED_TYPES):
    """The paths gclient would sync in `directory`, read before it syncs. An entry that custom_deps sets
    to None is listed as "path: None" and not synced."""
    listed = {}
    for line in gclient(directory, "revinfo", *ignored, capture=True).splitlines():
        path, _, source = line.partition(": ")
        if source and source != "None":
            listed[path] = source
    return listed


def check_revinfo(directory, keep):
    """Only the kept dependencies, and the solution itself."""
    listed = revinfo(directory)
    for path, source in sorted(listed.items()):
        if path not in set(keep) | {"."}:
            raise SystemExit(f"build-angle: gclient would sync {path} ({source}), which the entry does not keep")
    print("gclient would sync: " + ", ".join(sorted(listed)), flush=True)
    return listed


def git_blob(content):
    return hashlib.sha1(b"blob %d\0" % len(content) + content).hexdigest()


def safe_member(name):
    parts = name.split("/")
    return not name.startswith("/") and ".." not in parts


MARKER = ".minato-fetched.json"


def present(item, target):
    """Whether the item is already in place and checked: a single file by its Git blob, an unpacked
    archive by the marker written after its SHA-256 matched."""
    if item["unpack"] == "base64":
        if not os.path.isfile(target):
            return False
        with open(target, "rb") as handle:
            return git_blob(handle.read()) == item["git_blob"]
    marker = os.path.join(target, MARKER)
    if not os.path.isfile(marker):
        return False
    with open(marker) as handle:
        return json.load(handle).get("sha256") == item["sha256"]


def fetch_file(item):
    """Downloads one file of the entry, checks it and puts it where gclient would. Returns the bytes
    that went over the network, or None when it was already in place."""
    target = os.path.join(CHECKOUT, item["path"])
    require_inside(target)
    if present(item, target):
        return None
    with tempfile.TemporaryDirectory(dir=BUILD) as scratch:
        download = os.path.join(scratch, "download")
        transferred = output(["curl", "-fsSL", "--compressed", "--retry", "5", "--retry-all-errors",
                              "--retry-delay", "5", "-o", download, "-w", "%{size_download}", item["url"]])
        with open(download, "rb") as handle:
            content = handle.read()
        if item["unpack"] == "base64":
            content = base64.b64decode(content)
            if git_blob(content) != item["git_blob"]:
                raise SystemExit(f"build-angle: {item['path']} does not match its Git blob {item['git_blob']}")
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as handle:
                handle.write(content)
            return int(float(transferred))
        if hashlib.sha256(content).hexdigest() != item["sha256"]:
            raise SystemExit(f"build-angle: {item['url']} does not match its SHA-256 {item['sha256']}")
        if os.path.isdir(target):
            shutil.rmtree(target)
        os.makedirs(target)
        if item["unpack"] == "tar":
            with tarfile.open(download) as archive:
                archive.extractall(target, filter="data")
        elif item["unpack"] == "zip":
            with zipfile.ZipFile(download) as archive:
                for member in archive.infolist():
                    if not safe_member(member.filename):
                        raise SystemExit(f"build-angle: refusing zip member {member.filename!r}")
                    path = archive.extract(member, target)
                    mode = (member.external_attr >> 16) & 0o777
                    if mode:
                        os.chmod(path, mode)
            shutil.rmtree(os.path.join(target, ".cipdpkg"), ignore_errors=True)
        else:
            raise SystemExit(f"build-angle: unknown kind of file {item['unpack']!r}")
        with open(os.path.join(target, MARKER), "w") as handle:
            json.dump({"url": item["url"], "sha256": item["sha256"]}, handle)
    return int(float(transferred))


def checkouts():
    """Every git repository below the checkout, relative to it, the checkout itself as "."."""
    found = []
    for folder, directories, _ in os.walk(CHECKOUT):
        if ".git" in directories:
            found.append(os.path.relpath(folder, CHECKOUT))
        directories[:] = [name for name in directories if name not in (".git", "out")]
    return sorted(found)


def kilobytes(path):
    return int(output(["du", "-sk", path]).split()[0])


def low_power_mode():
    """pmset's "powermode" (macOS 27; older systems say "lowpowermode"): 1 is Low Power Mode."""
    for line in output(["pmset", "-g"]).splitlines():
        words = line.split()
        if len(words) >= 2 and words[0] in ("powermode", "lowpowermode"):
            return words[1] == "1"
    return None


def read_stamp():
    if os.path.exists(STAMP):
        with open(STAMP) as handle:
            return json.load(handle)
    return {}


def write_stamp(stamp):
    with open(STAMP, "w") as handle:
        json.dump(stamp, handle, indent=2, sort_keys=True)
        handle.write("\n")


def fetch():
    entry = pins()
    os.makedirs(BUILD, exist_ok=True)
    times = {}
    start = time.monotonic()
    fetch_commit(CHECKOUT, entry["project"], entry["commit"])
    tools = entry["depot_tools"]
    fetch_commit(os.path.join(CHECKOUT, tools["path"]), tools["url"], tools["commit"])
    times["angle_and_depot_tools_s"] = round(time.monotonic() - start, 1)

    settings = entry["gclient"]
    left_out = write_gclient(CHECKOUT, settings, entry["project"] + ".git")
    check_revinfo(CHECKOUT, settings["keep"])
    step = time.monotonic()
    gclient(CHECKOUT, "sync", "--no-history", "--nohooks", "--jobs", "4", *IGNORED_TYPES)
    times["gclient_sync_s"] = round(time.monotonic() - step, 1)
    if not os.path.exists(os.path.join(CHECKOUT, "build", "config", "gclient_args.gni")):
        raise SystemExit("build-angle: gclient wrote no build/config/gclient_args.gni")

    # The two local hooks of DEPS that the build needs, run here rather than through gclient runhooks,
    # which would process the CIPD entries again. The others download (suppressed) or serve siso (off).
    step = time.monotonic()
    run([sys.executable, os.path.join(tools["path"], "update_depot_tools_toggle.py"), "--disable"], cwd=CHECKOUT)
    run([sys.executable, "build/util/lastchange.py", "-o", "build/util/LASTCHANGE"], cwd=CHECKOUT)
    times["hooks_s"] = round(time.monotonic() - step, 1)

    step = time.monotonic()
    files = {}
    for item in entry["files"]:
        print(f"+ fetch {item['path']}", flush=True)
        files[item["path"]] = fetch_file(item)
    times["files_s"] = round(time.monotonic() - step, 1)
    times["total_s"] = round(time.monotonic() - start, 1)

    allowed = set(entry["gclient"]["keep"]) | {".", tools["path"]}
    found = checkouts()
    unexpected = [path for path in found if path not in allowed]
    if unexpected:
        raise SystemExit("build-angle: unexpected checkouts: " + ", ".join(unexpected))
    missing = sorted(set(entry["gclient"]["keep"]) - set(found))
    if missing:
        raise SystemExit("build-angle: kept dependencies missing after the sync: " + ", ".join(missing))

    repositories = {path: {"commit": output(["git", "-C", os.path.join(CHECKOUT, path), "rev-parse", "HEAD"]),
                           "git_kb": kilobytes(os.path.join(CHECKOUT, path, ".git"))} for path in found}
    stamp = read_stamp()
    # A file already in place keeps what an earlier run recorded for it, else its size in the entry.
    earlier = stamp.get("fetch", {}).get("files_downloaded_bytes", {})
    sizes = {item["path"]: item["bytes"] for item in entry["files"]}
    files = {path: (earlier.get(path) or sizes[path]) if transferred is None else transferred
             for path, transferred in files.items()}
    downloaded = sum(size or 0 for size in files.values()) + 1024 * sum(
        repository["git_kb"] for repository in repositories.values())
    stamp["fetch"] = {
        "commit": entry["commit"],
        "depot_tools": tools["commit"],
        "python": sys.version.split()[0],
        "git": output(["git", "--version"]),
        "left_out_dependencies": left_out,
        "repositories": repositories,
        "files_downloaded_bytes": files,
        "downloaded_bytes_estimate": downloaded,
        "disk_kb": kilobytes(BUILD),
        "times": times,
        "low_power_mode": low_power_mode(),
        "finished": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    write_stamp(stamp)
    print(json.dumps(stamp["fetch"], indent=2, sort_keys=True))
    if downloaded > entry["cap_bytes"]:
        raise SystemExit(f"build-angle: {downloaded} bytes fetched, above the approved {entry['cap_bytes']}; stopping")


SAMPLE_DEPS = """# A sample DEPS for --self-test: git entries kept and left out, a CIPD and a GCS entry.
use_relative_paths = True
vars = {'git': 'https://example.invalid'}
deps = {
  'kept': Var('git') + '/kept.git@0123456789012345678901234567890123456789',
  'left_out': Var('git') + '/left_out.git@0123456789012345678901234567890123456789',
  'tools/package': {
    'packages': [{'package': 'gn/gn/mac-${{arch}}', 'version': 'git_revision:0'}],
    'dep_type': 'cipd',
  },
  'tools/object': {
    'dep_type': 'gcs',
    'bucket': 'bucket',
    'objects': [{'object_name': 'object', 'sha256sum': '%s', 'size_bytes': 1, 'generation': 1}],
  },
}
""" % ("0" * 64)


def expect(condition, what):
    if not condition:
        raise SystemExit("build-angle: self-test failed: " + what)
    print("self-test: " + what, flush=True)


def self_test():
    """The script's own guards, without the network: DEPS is parsed, never run; with IGNORED_TYPES gclient
    would sync only the kept git entries of a sample DEPS, without them its CIPD and GCS entries too; git
    never acts on a directory that is not a repository of its own. Needs depot_tools of the checkout."""
    if not os.path.exists(os.path.join(CHECKOUT, "third_party", "depot_tools", "gclient.py")):
        raise SystemExit("build-angle: no depot_tools yet; run tools/build-angle.sh --fetch first")
    directory = os.path.join(BUILD, "self-test")
    require_inside(directory)
    if os.path.isdir(directory):
        shutil.rmtree(directory)
    os.makedirs(directory)
    try:
        marker = os.path.join(directory, "side-effect")
        with open(os.path.join(directory, "DEPS"), "w") as handle:
            handle.write(f"__import__('pathlib').Path({marker!r}).write_text('ran')\n"
                         "deps = {'a': 'x', 'b': {'url': 'y'}}\n")
        names = dependency_names(os.path.join(directory, "DEPS"))
        expect(names == {"a", "b"} and not os.path.exists(marker), "DEPS is parsed, not run")

        try:
            require_nested(directory)
            stopped = False
        except SystemExit:
            stopped = True
        expect(stopped, "git refuses a directory inside Minato's tree that is no repository of its own")

        with open(os.path.join(directory, "DEPS"), "w") as handle:
            handle.write(SAMPLE_DEPS)
        run(["git", "init", "-q", directory])
        require_nested(directory)
        settings = {"keep": ["kept"], "left_out_recursed": [], "custom_vars": {}, "suppressed_hooks": [],
                    "target_os": ["mac"]}
        write_gclient(directory, settings, "https://example.invalid/solution.git")
        everything = revinfo(directory, ignored=[])
        expect(any(path.startswith("tools/package:") for path in everything)
               and any(path.startswith("tools/object:") for path in everything)
               and "left_out" not in everything,
               "without IGNORED_TYPES gclient takes the CIPD and GCS entries, never the git entry set to None"
               f" (it listed: {', '.join(sorted(everything)) or 'nothing'})")
        listed = check_revinfo(directory, settings["keep"])
        expect(set(listed) <= {".", "kept"} and "kept" in listed,
               f"with IGNORED_TYPES gclient takes only the kept git entry (it listed: {', '.join(sorted(listed))})")
    finally:
        shutil.rmtree(directory)
    print("self-test: passed", flush=True)


def apply_patches():
    """ADR-0029, decision 8: when the pin or the series changed since the last run, the checkout goes back
    to the pin (tracked files forced back, untracked ones removed, ignored ones and the dependencies'
    repositories kept), then the series applies in order."""
    names = sorted(name for name in os.listdir(PATCHES) if name.endswith(".patch")) if os.path.isdir(PATCHES) else []
    series = {}
    for name in names:
        with open(os.path.join(PATCHES, name), "rb") as handle:
            series[name] = hashlib.sha256(handle.read()).hexdigest()
    wanted = {"commit": pins()["commit"], "series": series}
    stamp = read_stamp()
    if stamp.get("patches") == wanted:
        return
    require_nested(CHECKOUT)
    run(["git", "-C", CHECKOUT, "checkout", "-q", "--force", wanted["commit"]])
    require_nested(CHECKOUT)
    run(["git", "-C", CHECKOUT, "clean", "-q", "-f", "-d"])
    for name in names:
        path = os.path.join(PATCHES, name)
        run(["git", "-C", CHECKOUT, "apply", "--check", path])
        run(["git", "-C", CHECKOUT, "apply", path])
    stamp["patches"] = wanted
    write_stamp(stamp)


def gen(slices):
    gn = os.path.join(CHECKOUT, "buildtools", "mac", "gn")
    if not os.path.exists(gn):
        raise SystemExit("build-angle: no checkout yet; run tools/build-angle.sh --fetch first (after the user's OK)")
    apply_patches()
    with open(os.path.join(ARGUMENTS, "common.gn")) as handle:
        common = handle.read()
    stamp = read_stamp()
    for name in slices:
        out = os.path.join(CHECKOUT, "out", name)
        require_inside(out)
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(ARGUMENTS, name + ".gn")) as handle:
            arguments = common + "\n" + handle.read()
        path = os.path.join(out, "args.gn")
        if not os.path.exists(path) or open(path).read() != arguments:
            with open(path, "w") as handle:
                handle.write(arguments)
        start = time.monotonic()
        run([gn, "gen", os.path.relpath(out, CHECKOUT)], cwd=CHECKOUT)
        stamp.setdefault("gen", {})[name] = {"seconds": round(time.monotonic() - start, 1),
                                             "finished": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
        write_stamp(stamp)


def binary(directory, name):
    """The Mach-O file of a slice's library below `directory`."""
    library = os.path.join(directory, LIBRARIES[name])
    return os.path.join(library, "libGLESv2") if library.endswith(".framework") else library


def minimum_os(path):
    """The minimum OS version of LC_BUILD_VERSION."""
    words = output(["otool", "-l", path]).split()
    for index, word in enumerate(words):
        if word == "minos":
            return tuple(int(part) for part in words[index + 1].split("."))
    raise SystemExit(f"build-angle: no LC_BUILD_VERSION in {path}")


def check_slice(name, path):
    """ADR-0029, decision 9: no private framework linked, and an iOS slice not above the app's target."""
    private = [line.strip() for line in output(["otool", "-L", path]).splitlines() if "/PrivateFrameworks/" in line]
    if private:
        raise SystemExit(f"build-angle: {name} links private frameworks: " + ", ".join(private))
    minos = minimum_os(path)
    if name != "macos" and minos > APP_DEPLOYMENT_TARGET:
        raise SystemExit(f"build-angle: {name} needs iOS {minos}, above the app's {APP_DEPLOYMENT_TARGET}")
    return ".".join(str(part) for part in minos)


def zipped_bytes(path):
    """What the library adds to a compressed app archive (an IPA is a ZIP)."""
    with tempfile.TemporaryDirectory(dir=BUILD) as scratch:
        archive = os.path.join(scratch, "library.zip")
        run(["ditto", "-c", "-k", "--keepParent", path, archive])
        return os.path.getsize(archive)


def package(stamp):
    """The stripped iOS slices, packed into build/ANGLE.xcframework; the unstripped ones stay in out/."""
    frameworks = []
    for name in ("ios", "ios-simulator"):
        source = os.path.join(CHECKOUT, "out", name, LIBRARIES[name])
        target = os.path.join(STRIPPED, name, LIBRARIES[name])
        require_inside(target)
        if os.path.isdir(target):
            shutil.rmtree(target)
        shutil.copytree(source, target, symlinks=True)
        run(["xcrun", "strip", "-x", binary(os.path.dirname(target), name)])
        sizes = stamp["build"][name]["bytes"]
        sizes["stripped"] = os.path.getsize(binary(os.path.dirname(target), name))
        sizes["installed"] = 1024 * kilobytes(target)
        sizes["zipped"] = zipped_bytes(target)
        frameworks += ["-framework", target]
    require_inside(XCFRAMEWORK)
    if os.path.isdir(XCFRAMEWORK):
        shutil.rmtree(XCFRAMEWORK)
    run(["xcodebuild", "-create-xcframework", *frameworks, "-output", XCFRAMEWORK])


# //build's Apple toolchain writes a library's table of contents with llvm-otool and llvm-nm from the
# compiler's folder. The pin's clang archive has neither (they come with the llvm-objdump archive of
# DEPS, which the download left out), so the two names point to Xcode's otool and nm, both LLVM's.
XCODE_TOOLS = {"llvm-otool": "otool", "llvm-nm": "nm"}


def link_xcode_tools():
    folder = os.path.join(CHECKOUT, "third_party", "llvm-build", "Release+Asserts", "bin")
    require_inside(folder)  # not the link itself, whose real path is Xcode's tool
    linked = {}
    for name, tool in XCODE_TOOLS.items():
        path = os.path.join(folder, name)
        if not os.path.lexists(path):
            os.symlink(output(["xcrun", "--find", tool]), path)
        if os.path.islink(path):
            linked[name] = os.readlink(path)
    return linked


def build(slices):
    """ADR-0029, step A2: patches, gn gen and ninja per slice, the checks, and the XCFramework once both
    iOS slices are built."""
    gen(slices)
    xcode_tools = link_xcode_tools()
    stamp = read_stamp()
    record = stamp.setdefault("build", {})
    record["xcode"] = " ".join(output(["xcodebuild", "-version"]).split())
    record["low_power_mode"] = low_power_mode()
    record["xcode_tools"] = xcode_tools
    for name in slices:
        out = os.path.join(CHECKOUT, "out", name)
        start = time.monotonic()
        run(["ninja", "-C", out, "-j", "6", "libGLESv2"])
        seconds = round(time.monotonic() - start, 1)
        path = binary(out, name)
        record[name] = {"seconds": seconds, "minos": check_slice(name, path),
                        "bytes": {"unstripped": os.path.getsize(path)},
                        "finished": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
        if name == "macos":
            record[name]["install_name"] = output(["otool", "-D", path]).splitlines()[-1].strip()
        write_stamp(stamp)
    if all(name in record for name in ("ios", "ios-simulator")):
        package(stamp)
        write_stamp(stamp)
    print(json.dumps(record, indent=2, sort_keys=True))


LICENSE_FILES = ("README.chromium", "LICENSE", "LICENSE.TXT", "LICENSE.txt", "LICENSE.md", "COPYING")


def licenses(name):
    """ADR-0029, decision 11: every file a slice's libGLESv2 was built from (the sources ninja compiled and
    the headers they read), grouped by the nearest directory with its own README.chromium or LICENSE.
    The compiler's own headers and those of the SDK are not code of the library."""
    out = os.path.join(CHECKOUT, "out", name)
    files = set()
    for line in output(["ninja", "-C", out, "-t", "deps"]).splitlines():
        if line.startswith("    "):
            files.add(os.path.normpath(os.path.join(out, line.strip())))
    groups = {}
    for path in files:
        if not inside(path, CHECKOUT) or inside(path, out) or inside(path, os.path.join(CHECKOUT, "third_party", "llvm-build")):
            continue
        # Up to the nearest license file, but never past the root of a repository of its own: Chromium's
        # //build has no license file and must not count as ANGLE's.
        folder = os.path.dirname(path)
        while inside(folder, CHECKOUT) and not any(os.path.exists(os.path.join(folder, candidate))
                                                   for candidate in LICENSE_FILES + (".git",)):
            folder = os.path.dirname(folder)
        key = os.path.relpath(folder, CHECKOUT)
        groups.setdefault(key, 0)
        groups[key] += 1
    report = {}
    for key, count in sorted(groups.items()):
        folder = os.path.join(CHECKOUT, key)
        found = [candidate for candidate in LICENSE_FILES if os.path.exists(os.path.join(folder, candidate))]
        entry = {"files": count, "license_files": found}
        if "README.chromium" in found:
            with open(os.path.join(folder, "README.chromium"), errors="replace") as handle:
                for line in handle:
                    field, _, value = line.partition(":")
                    if field in ("Name", "License", "Version"):
                        entry[field.lower()] = value.strip()
        report[key] = entry
    path = os.path.join(BUILD, f"licenses-{name}.json")
    with open(path, "w") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")
    for key, entry in report.items():
        print(f"{key}: {entry['files']} files, {entry.get('name', '')} {entry.get('license', '')} {entry['license_files']}")
    print(f"written to {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fetch", action="store_true", help="fetch the pinned checkout (network)")
    parser.add_argument("--gen", nargs="*", metavar="slice", help="gn gen for these slices (all without a name)")
    parser.add_argument("--build", nargs="*", metavar="slice", help="build these slices (all without a name)")
    parser.add_argument("--licenses", metavar="slice", help="list the licenses of what a built slice holds")
    parser.add_argument("--self-test", action="store_true", help="check the script's guards (no network)")
    parser.add_argument("--root", help="a copy of build/angle/ to work in instead, for mutation checks")
    options = parser.parse_args()
    if options.root:
        global BUILD, CHECKOUT, STAMP, STRIPPED
        BUILD = os.path.abspath(options.root)
        CHECKOUT = os.path.join(BUILD, "angle")
        STAMP = os.path.join(BUILD, "stamp.json")
        STRIPPED = os.path.join(BUILD, "stripped")
    actions = (options.fetch, options.gen is not None, options.build is not None, options.licenses, options.self_test)
    if not any(actions):
        parser.error("nothing to do: --self-test, --fetch, --gen, --build or --licenses")
    for chosen in (options.gen or []) + (options.build or []) + ([options.licenses] if options.licenses else []):
        if chosen not in SLICES:
            parser.error("unknown slice: " + chosen)
    if options.self_test:
        self_test()
    if options.fetch:
        fetch()
    if options.gen is not None:
        gen(options.gen or SLICES)
    if options.build is not None:
        build(options.build or SLICES)
    if options.licenses:
        licenses(options.licenses)


if __name__ == "__main__":
    main()
