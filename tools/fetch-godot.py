#!/usr/bin/env python3
"""Fetches Godot 4.7.2's macOS editor and export templates, the Android SDK's build-tools 35.0.1 and Godot's
license texts for test 2 (ADR-0042 a), below build/godot/ and third_party/godot/.
Usage: tools/fetch-godot.py --self-test   check the script's guards (no network)
       tools/fetch-godot.py --fetch       fetch, check and unpack (network)
The pins are the entries "godot" and "android-build-tools" of third_party/sources.json. An archive must have
its size and every digest its entry names: Godot's SHA-512 from the release's SHA512-SUMS.txt, Google's SHA-1,
and the SHA-256 kept at the first fetch. The license texts must match their Git blobs. Every member of an
archive is checked before anything is unpacked: no absolute name, no "..", and a link only to a place inside
the archive's own folder. Then:
  - the editor becomes build/godot/editor/Godot.app, with _sc_ beside it: Godot's self-contained mode keeps its
    settings, export templates and keystores in build/godot/editor/editor_data/, never in ~/Library;
  - of the templates only version.txt, android_release.apk and ios.zip go to
    editor_data/export_templates/4.7.2.stable/, the version their version.txt must name;
  - build-tools 35.0.1 becomes build/godot/sdk/build-tools/35.0.1, beside a link to the installed platform-tools
    ($ANDROID_HOME, else ~/Library/Android/sdk), so the installed SDK stays as it is (ADR-0042, departure 1);
  - the license texts go to third_party/godot/, which the repo keeps.
The script writes only below build/godot/ and third_party/godot/, and runs on macOS's /usr/bin/python3 (3.9).
"""
import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "godot")
DOWNLOADS = os.path.join(BUILD, "downloads")
EDITOR = os.path.join(BUILD, "editor")
SDK = os.path.join(BUILD, "sdk")
LICENSES = os.path.join(ROOT, "third_party", "godot")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
MARKER = ".minato-unpacked.json"
TEMPLATES = ("version.txt", "android_release.apk", "ios.zip")
DIGESTS = {"sha512": hashlib.sha512, "sha256": hashlib.sha256, "sha1": hashlib.sha1}
HEX_LENGTHS = {"sha512": 128, "sha256": 64, "sha1": 40, "git_blob": 40}
HOSTS = ("https://github.com/godotengine/godot/", "https://raw.githubusercontent.com/godotengine/godot/",
         "https://dl.google.com/android/repository/")


def manifest():
    with open(MANIFEST) as handle:
        data = json.load(handle)
    return data["godot"], data["android-build-tools"]


def inside(path, base):
    real, base = os.path.realpath(path), os.path.realpath(base)
    return real == base or real.startswith(base + os.sep)


def require_inside(path):
    if not (inside(path, BUILD) or inside(path, LICENSES)):
        raise SystemExit(f"fetch-godot: refusing to write outside {BUILD} and {LICENSES}: {path}")


def file_digest(path, algorithm):
    digest = DIGESTS[algorithm]()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def git_blob(content):
    return hashlib.sha1(b"blob %d\0" % len(content) + content).hexdigest()


def check_asset(path, asset):
    """Refuses a file whose size or any digest its entry names differs, and an entry that names none."""
    size = os.path.getsize(path)
    if size != asset["bytes"]:
        raise SystemExit(f"fetch-godot: {asset['name']} has {size} bytes, not {asset['bytes']}")
    named = [algorithm for algorithm in DIGESTS if algorithm in asset]
    if not named:
        raise SystemExit(f"fetch-godot: sources.json names no digest for {asset['name']}")
    for algorithm in named:
        if file_digest(path, algorithm) != asset[algorithm]:
            raise SystemExit(f"fetch-godot: {asset['name']} does not match its {algorithm.upper()} {asset[algorithm]}")


def check_blob(content, expected, what):
    if git_blob(content) != expected:
        raise SystemExit(f"fetch-godot: {what} does not match its Git blob {expected}")


def fingerprint(asset):
    """The strongest digest an entry names, which an unpacked folder's marker keeps."""
    return asset.get("sha512") or asset.get("sha256") or asset["sha1"]


def download(url, target):
    """Downloads into a temporary file next to `target` and returns it."""
    require_inside(target)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    handle, temporary = tempfile.mkstemp(dir=os.path.dirname(target))
    os.close(handle)
    print(f"+ fetch {url}", flush=True)
    subprocess.run(["curl", "-fsSL", "--retry", "5", "--retry-all-errors", "--retry-delay", "5",
                    "-o", temporary, url], check=True)
    return temporary


def fetch_asset(asset):
    """One archive into build/godot/downloads/, checked; a file that is there already and checks is kept."""
    path = os.path.join(DOWNLOADS, asset["name"])
    if os.path.exists(path):
        try:
            check_asset(path, asset)
            print(f"have {asset['name']}", flush=True)
            return path
        except SystemExit as error:
            print(f"{error}; fetching it again", flush=True)
    temporary = download(asset["url"], path)
    try:
        check_asset(temporary, asset)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)
    return path


def fetch_file(item):
    """A license text, checked against its Git blob, into third_party/godot/; kept when it is there and checks."""
    path = os.path.join(LICENSES, item["path"])
    if os.path.exists(path):
        with open(path, "rb") as handle:
            if git_blob(handle.read()) == item["git_blob"]:
                return
    temporary = download(item["url"], path)
    try:
        with open(temporary, "rb") as handle:
            check_blob(handle.read(), item["git_blob"], item["path"])
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def members(archive):
    """The names in a zip archive, refused when one would land outside the archive's folder: an absolute name,
    a "..", or a link whose target leaves it. macOS's "__MACOSX" folder of resource forks is left out."""
    names = []
    with zipfile.ZipFile(archive) as zipped:
        for info in zipped.infolist():
            name = info.filename
            if name.startswith("/") or "\\" in name or ".." in name.split("/"):
                raise SystemExit(f"fetch-godot: refusing archive member {name!r} of {os.path.basename(archive)}")
            if stat.S_ISLNK(info.external_attr >> 16):
                link = zipped.read(info).decode()
                landing = os.path.normpath(os.path.join(os.path.dirname(name.rstrip("/")), link))
                if link.startswith("/") or landing == ".." or landing.startswith("../"):
                    raise SystemExit(f"fetch-godot: refusing link {name!r} to {link!r} of {os.path.basename(archive)}")
            if name.split("/")[0] != "__MACOSX":
                names.append(name)
    return names


def unpacked(marker, value):
    if not os.path.exists(marker):
        return False
    with open(marker) as handle:
        return json.load(handle).get("fingerprint") == value


def unpack_folder(archive, value, target, name=None):
    """Unpacks a checked zip archive that holds one folder (named `name`, when given) and puts that folder at
    `target`; kept when its marker names the same fingerprint. ditto keeps the links and modes that Python's
    zipfile drops, which an app bundle and the build-tools need."""
    require_inside(target)
    marker = target + MARKER
    if unpacked(marker, value) and os.path.isdir(target):
        return
    tops = {member.split("/")[0] for member in members(archive)}
    if len(tops) != 1 or (name is not None and tops != {name}):
        raise SystemExit(f"fetch-godot: {os.path.basename(archive)} holds {sorted(tops)}, not one folder"
                         + (f" named {name}" if name else ""))
    os.makedirs(os.path.dirname(target), exist_ok=True)
    scratch = tempfile.mkdtemp(dir=os.path.dirname(target))
    try:
        subprocess.run(["ditto", "-x", "-k", archive, scratch], check=True)
        if os.path.lexists(target):
            shutil.rmtree(target)
        os.replace(os.path.join(scratch, tops.pop()), target)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    with open(marker, "w") as handle:
        json.dump({"fingerprint": value}, handle)


def unpack_templates(archive, value, version, editor=EDITOR):
    """Puts version.txt, android_release.apk and ios.zip of the export templates into the editor's
    editor_data/export_templates/<version>/; their version.txt must name `version`. The other platforms'
    templates stay packed."""
    target = os.path.join(editor, "editor_data", "export_templates", version)
    require_inside(target)
    marker = os.path.join(target, MARKER)
    if unpacked(marker, value):
        return
    names = set(members(archive))
    missing = [name for name in TEMPLATES if "templates/" + name not in names]
    if missing:
        raise SystemExit(f"fetch-godot: {os.path.basename(archive)} lacks {', '.join(missing)}")
    with zipfile.ZipFile(archive) as zipped:
        found = zipped.read("templates/version.txt").decode().strip()
        if found != version:
            raise SystemExit(f"fetch-godot: the templates' version.txt names {found!r}, not {version!r}")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        scratch = tempfile.mkdtemp(dir=os.path.dirname(target))
        try:
            for name in TEMPLATES:
                with zipped.open("templates/" + name) as source, open(os.path.join(scratch, name), "wb") as copy:
                    shutil.copyfileobj(source, copy, 1 << 20)
            os.chmod(scratch, 0o755)
            if os.path.lexists(target):
                shutil.rmtree(target)
            os.replace(scratch, target)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
    with open(marker, "w") as handle:
        json.dump({"fingerprint": value}, handle)


def platform_tools():
    """The installed platform-tools, where the export looks for adb: $ANDROID_HOME, else ~/Library/Android/sdk.
    An ANDROID_HOME of build/godot/sdk, which tools/build-godot.sh sets, is this script's own SDK folder."""
    home = os.environ.get("ANDROID_HOME") or os.path.expanduser("~/Library/Android/sdk")
    if inside(home, SDK):
        raise SystemExit(f"fetch-godot: ANDROID_HOME names {home}, this script's own SDK folder: name the installed SDK")
    tools = os.path.join(home, "platform-tools")
    if not os.access(os.path.join(tools, "adb"), os.X_OK):
        raise SystemExit(f"fetch-godot: no adb in {tools}: install platform-tools there first (docs/STATUS.md)")
    return tools


def link_platform_tools(tools, sdk=SDK):
    """build/godot/sdk/platform-tools as a link to the installed platform-tools."""
    link = os.path.join(sdk, "platform-tools")
    require_inside(link)
    os.makedirs(sdk, exist_ok=True)
    if os.path.islink(link):
        if os.readlink(link) == tools:
            return
        os.remove(link)
    elif os.path.exists(link):
        raise SystemExit(f"fetch-godot: {link} is not a link: remove it first")
    os.symlink(tools, link)


def asset_named(entry, suffix):
    found = [asset for asset in entry["assets"] if asset["name"].endswith(suffix)]
    if len(found) != 1:
        raise SystemExit(f"fetch-godot: sources.json names {len(found)} assets ending in {suffix}, not one")
    return found[0]


def fetch():
    godot, build_tools = manifest()
    editor = asset_named(godot, "_macos.universal.zip")
    templates = asset_named(godot, "_export_templates.tpz")
    tools = build_tools["assets"][0]
    version = build_tools["package"].split(";")[1]
    for item in godot["files"]:
        fetch_file(item)
    paths = {asset["name"]: fetch_asset(asset) for asset in (editor, templates, tools)}
    for asset in (editor, templates, tools):
        print(f"sha256 {asset['name']} {file_digest(paths[asset['name']], 'sha256')}", flush=True)
    unpack_folder(paths[editor["name"]], fingerprint(editor), os.path.join(EDITOR, "Godot.app"), name="Godot.app")
    with open(os.path.join(EDITOR, "_sc_"), "a"):
        pass
    unpack_templates(paths[templates["name"]], fingerprint(templates), godot["tag"].replace("-", "."))
    unpack_folder(paths[tools["name"]], fingerprint(tools), os.path.join(SDK, "build-tools", version))
    link_platform_tools(platform_tools())
    print(f"fetch-godot: the editor in {EDITOR}, the SDK in {SDK}, the license texts in {LICENSES}", flush=True)


def expect(condition, what):
    if not condition:
        raise SystemExit("fetch-godot: self-test failed: " + what)
    print("self-test: " + what, flush=True)


def refused(action):
    try:
        action()
    except SystemExit:
        return True
    return False


def write_zip(path, entries):
    """A zip archive of (name, content, mode) entries; a mode with S_IFLNK makes a link to the content."""
    with zipfile.ZipFile(path, "w") as zipped:
        for name, content, mode in entries:
            info = zipfile.ZipInfo(name)
            info.external_attr = mode << 16
            zipped.writestr(info, content)


def self_test():
    """The script's guards, without the network: where it writes, the sizes and digests it takes, the archive
    members it refuses, what it unpacks and where, the platform-tools it links, and the pins' form."""
    os.makedirs(BUILD, exist_ok=True)
    directory = tempfile.mkdtemp(prefix="self-test-", dir=BUILD)  # one per run: the chats' CTest runs may overlap
    file_mode, tool_mode, link_mode = stat.S_IFREG | 0o644, stat.S_IFREG | 0o755, stat.S_IFLNK | 0o777
    try:
        expect(refused(lambda: require_inside(os.path.join(ROOT, "runtime", "sample"))) and
               refused(lambda: require_inside(MANIFEST)) and
               not refused(lambda: require_inside(os.path.join(BUILD, "sample"))) and
               not refused(lambda: require_inside(os.path.join(LICENSES, "sample"))),
               "only writes below build/godot/ and third_party/godot/ are taken")

        sample = os.path.join(directory, "sample")
        with open(sample, "wb") as handle:
            handle.write(b"sample")
        good = {"name": "sample", "bytes": 6, **{algorithm: file_digest(sample, algorithm) for algorithm in DIGESTS}}
        expect(not refused(lambda: check_asset(sample, good)), "a file with its size and every digest is taken")
        expect(refused(lambda: check_asset(sample, dict(good, bytes=7))), "a file of another size is refused")
        for algorithm in DIGESTS:
            expect(refused(lambda: check_asset(sample, dict(good, **{algorithm: "0" * HEX_LENGTHS[algorithm]}))),
                   f"a file that does not match its {algorithm.upper()} is refused")
        expect(refused(lambda: check_asset(sample, {"name": "sample", "bytes": 6})), "an entry without a digest is refused")
        expect(refused(lambda: check_blob(b"sample", "0" * 40, "sample")) and
               not refused(lambda: check_blob(b"sample", git_blob(b"sample"), "sample")),
               "content that does not match its Git blob is refused, content that does is taken")

        target = os.path.join(directory, "unpacked", "tool")
        for entries, kind in (([("top/../outside", b"x", file_mode)], "a member with '..'"),
                              ([("/tmp/absolute", b"x", file_mode)], "an absolute member"),
                              ([("top/link", b"../../outside", link_mode)], "a link that leaves the archive's folder"),
                              ([("top/a", b"x", file_mode), ("other/b", b"x", file_mode)], "two top folders")):
            archive = os.path.join(directory, "bad.zip")
            write_zip(archive, entries)
            expect(refused(lambda: unpack_folder(archive, "bad", target)) and not os.path.lexists(target),
                   f"an archive with {kind} is refused before anything is unpacked")

        archive = os.path.join(directory, "good.zip")
        write_zip(archive, [("top/bin/tool", b"#!/bin/sh\n", tool_mode), ("top/lib/current", b"../bin", link_mode),
                            ("top/notes.txt", b"notes", file_mode)])
        expect(refused(lambda: unpack_folder(archive, "good", target, name="Godot.app")),
               "an archive whose folder has another name than the one asked for is refused")
        unpack_folder(archive, "good", target)
        tool = os.path.join(target, "bin", "tool")
        expect(os.path.isfile(tool) and os.access(tool, os.X_OK) and
               os.path.islink(os.path.join(target, "lib", "current")) and
               os.readlink(os.path.join(target, "lib", "current")) == "../bin",
               "an archive's one folder becomes the target, with its modes and links kept")
        inode = os.stat(target).st_ino
        unpack_folder(archive, "good", target)
        expect(os.stat(target).st_ino == inode, "a folder whose marker names the same digest is kept")

        editor = os.path.join(directory, "editor")
        tpz = os.path.join(directory, "templates.tpz")
        templates = [("templates/" + name, name.encode(), file_mode) for name in TEMPLATES[1:]]
        write_zip(tpz, [("templates/version.txt", b"4.7.2.stable\n", file_mode), *templates,
                        ("templates/linux_release.x86_64", b"linux", tool_mode)])
        unpack_templates(tpz, "tpz", "4.7.2.stable", editor)
        folder = os.path.join(editor, "editor_data", "export_templates", "4.7.2.stable")
        expect(os.path.isdir(folder) and sorted(name for name in os.listdir(folder) if name != MARKER) == sorted(TEMPLATES),
               "of the templates only version.txt, android_release.apk and ios.zip are unpacked, into the version's folder")
        write_zip(tpz, [("templates/version.txt", b"4.7.1.stable\n", file_mode), *templates])
        expect(refused(lambda: unpack_templates(tpz, "other", "4.7.2.stable", os.path.join(directory, "editor2"))),
               "templates whose version.txt names another version are refused")
        write_zip(tpz, [("templates/version.txt", b"4.7.2.stable\n", file_mode), *templates[:1]])
        expect(refused(lambda: unpack_templates(tpz, "short", "4.7.2.stable", os.path.join(directory, "editor3"))),
               "templates without ios.zip are refused")

        home = os.path.join(directory, "android-sdk")
        os.makedirs(os.path.join(home, "platform-tools"))
        saved = os.environ.get("ANDROID_HOME")
        os.environ["ANDROID_HOME"] = home
        try:
            expect(refused(platform_tools), "installed platform-tools without adb are refused")
            adb = os.path.join(home, "platform-tools", "adb")
            with open(adb, "w") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(adb, 0o755)
            os.environ["ANDROID_HOME"] = SDK
            expect(refused(platform_tools), "an ANDROID_HOME of build/godot/sdk is not taken as the installed SDK")
            os.environ["ANDROID_HOME"] = home
            sdk = os.path.join(directory, "sdk")
            link_platform_tools(platform_tools(), sdk)
            expect(os.path.islink(os.path.join(sdk, "platform-tools")) and
                   os.path.realpath(os.path.join(sdk, "platform-tools", "adb")) == os.path.realpath(adb),
                   "the SDK folder links the installed platform-tools")
        finally:
            if saved is None:
                os.environ.pop("ANDROID_HOME", None)
            else:
                os.environ["ANDROID_HOME"] = saved

        godot, build_tools = manifest()
        assets = godot["assets"] + build_tools["assets"]
        expect(all(asset["url"].startswith(HOSTS) and asset["bytes"] > 0 and
                   any(algorithm in asset for algorithm in DIGESTS) and
                   all(len(asset[key]) == length and int(asset[key], 16) >= 0
                       for key, length in HEX_LENGTHS.items() if key in asset) for asset in assets) and
               all(item["url"].startswith(HOSTS) and len(item["git_blob"]) == 40 for item in godot["files"]) and
               build_tools["package"].startswith("build-tools;"),
               "the pins in sources.json name their hosts, sizes and digests")
    finally:
        shutil.rmtree(directory)
    print("self-test: passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fetch", action="store_true", help="fetch, check and unpack (network)")
    parser.add_argument("--self-test", action="store_true", help="check the script's guards (no network)")
    options = parser.parse_args()
    if not options.fetch and not options.self_test:
        parser.error("nothing to do: --self-test or --fetch")
    if options.self_test:
        self_test()
    if options.fetch:
        fetch()


if __name__ == "__main__":
    main()
