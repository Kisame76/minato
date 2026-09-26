#!/usr/bin/env python3
"""Fetches MoltenVK as released by Khronos, and vk.xml, for Minato (ADR-0030, decision 8), below build/moltenvk/.

Usage: tools/fetch-moltenvk.sh --self-test   check the script's guards (no network)
       tools/fetch-moltenvk.sh --fetch       fetch, check and unpack (network)
       tools/fetch-moltenvk.sh --licenses    fetch the license texts the release lacks (network)

The pins are the entries "moltenvk", "vulkan-headers" and "moltenvk-licenses" of third_party/sources.json.
Nothing is built: the two release archives are checked against their SHA-256 and unpacked with Python's
"data" filter, which refuses absolute paths, ".." and links that leave the archive; vk.xml and the
license texts are checked against their Git blob IDs. The dynamic framework of the iOS archive, device
only, becomes build/MoltenVK.xcframework, which the app build embeds in device builds; the macOS
archive's dylib and the headers go to fixed places in build/moltenvk/ for CTest and graphics/host. The
license texts of what MoltenVK's binary holds besides its own code go to third_party/moltenvk/licenses/,
which the repo keeps. The script writes only below build/moltenvk/, to build/MoltenVK.xcframework and
below third_party/moltenvk/licenses/.
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "moltenvk")
XCFRAMEWORK = os.path.join(ROOT, "build", "MoltenVK.xcframework")
LICENSES = os.path.join(ROOT, "third_party", "moltenvk", "licenses")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
MARKER = ".minato-unpacked.json"
APP_DEPLOYMENT_TARGET = (26, 0)


def manifest():
    with open(MANIFEST) as handle:
        data = json.load(handle)
    return data["moltenvk"], data["vulkan-headers"]


def license_manifest():
    with open(MANIFEST) as handle:
        return json.load(handle)["moltenvk-licenses"]


def inside(path, base=None):
    real, base = os.path.realpath(path), os.path.realpath(BUILD if base is None else base)
    return real == base or real.startswith(base + os.sep)


def require_inside(path):
    if not (inside(path) or inside(path, XCFRAMEWORK) or inside(path, LICENSES)):
        raise SystemExit(f"fetch-moltenvk: refusing to write outside {BUILD}, {XCFRAMEWORK} and {LICENSES}: {path}")


def run(command, **options):
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, **options)


def output(command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout.strip()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def git_blob(content):
    return hashlib.sha1(b"blob %d\0" % len(content) + content).hexdigest()


def download(url, target):
    """Downloads into a temporary file next to `target`; returns it and the bytes that went over the network."""
    require_inside(target)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    handle, temporary = tempfile.mkstemp(dir=os.path.dirname(target))
    os.close(handle)
    transferred = output(["curl", "-fsSL", "--retry", "5", "--retry-all-errors", "--retry-delay", "5",
                          "-o", temporary, "-w", "%{size_download}", url])
    return temporary, int(float(transferred))


def check_sha256(path, expected, what):
    if sha256_file(path) != expected:
        raise SystemExit(f"fetch-moltenvk: {what} does not match its SHA-256 {expected}")


def check_blob(content, expected, what):
    if git_blob(content) != expected:
        raise SystemExit(f"fetch-moltenvk: {what} does not match its Git blob {expected}")


def fetch_asset(asset):
    """One release archive into build/moltenvk/downloads/; returns the bytes fetched, 0 when it was there."""
    path = os.path.join(BUILD, "downloads", asset["name"])
    if os.path.exists(path) and sha256_file(path) == asset["sha256"]:
        return 0
    print(f"+ fetch {asset['url']}", flush=True)
    temporary, transferred = download(asset["url"], path)
    try:
        check_sha256(temporary, asset["sha256"], asset["name"])
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)
    return transferred


def fetch_file(item, base=BUILD):
    """A single file, checked against its Git blob, into `base` (vk.xml into build/moltenvk/, the license
    texts into third_party/moltenvk/licenses/); returns the bytes fetched, 0 when it was there."""
    path = os.path.join(base, item["path"])
    if os.path.exists(path):
        with open(path, "rb") as handle:
            if git_blob(handle.read()) == item["git_blob"]:
                return 0
    print(f"+ fetch {item['url']}", flush=True)
    temporary, transferred = download(item["url"], path)
    try:
        with open(temporary, "rb") as handle:
            check_blob(handle.read(), item["git_blob"], item["path"])
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)
    return transferred


def unpack(archive, destination, sha256):
    """Unpacks a checked archive with the "data" filter; skipped when its marker names the same SHA-256."""
    require_inside(destination)
    marker = os.path.join(destination, MARKER)
    if os.path.exists(marker):
        with open(marker) as handle:
            if json.load(handle).get("sha256") == sha256:
                return
    if os.path.isdir(destination):
        shutil.rmtree(destination)
    os.makedirs(destination)
    with tarfile.open(archive) as tar:
        # The "data" filter would make an absolute name relative; such an archive is refused instead,
        # as tools/fetch-sources.py does.
        for member in tar.getmembers():
            if member.name.startswith("/") or ".." in member.name.split("/"):
                raise SystemExit(f"fetch-moltenvk: refusing archive member {member.name!r}")
        tar.extractall(destination, filter="data")
    with open(marker, "w") as handle:
        json.dump({"sha256": sha256}, handle)


def find(top, suffix, shortest=False):
    """The one path below `top` that ends with `suffix`; with `shortest`, the shortest of several."""
    found = []
    for folder, directories, files in os.walk(top):
        for name in directories + files:
            path = os.path.join(folder, name)
            if path.endswith(os.sep + suffix):
                found.append(path)
    if shortest and found:
        return min(found, key=len)
    if len(found) != 1:
        raise SystemExit(f"fetch-moltenvk: expected one {suffix} below {top}, found {len(found)}")
    return found[0]


def license_files(top):
    return sorted(os.path.relpath(os.path.join(folder, name), BUILD) for folder, _, names in os.walk(top)
                  for name in names if name.upper().startswith("LICENSE"))


def copy(source, target):
    require_inside(target)
    if os.path.isdir(target):
        shutil.rmtree(target)
    elif os.path.exists(target):
        os.remove(target)
    if os.path.isdir(source):
        shutil.copytree(source, target, symlinks=True)
    else:
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(source, target)


def minimum_os(path):
    words = output(["otool", "-l", path]).split()
    for index, word in enumerate(words):
        if word == "minos":
            return tuple(int(part) for part in words[index + 1].split("."))
    raise SystemExit(f"fetch-moltenvk: no LC_BUILD_VERSION in {path}")


def check_binary(path, ios):
    """No private framework linked, and an iOS binary not above the app's deployment target."""
    private = [line.strip() for line in output(["otool", "-L", path]).splitlines() if "/PrivateFrameworks/" in line]
    if private:
        raise SystemExit(f"fetch-moltenvk: {path} links private frameworks: " + ", ".join(private))
    minos = minimum_os(path)
    if ios and minos > APP_DEPLOYMENT_TARGET:
        raise SystemExit(f"fetch-moltenvk: {path} needs iOS {minos}, above the app's {APP_DEPLOYMENT_TARGET}")
    return {"minos": ".".join(str(part) for part in minos), "architectures": output(["lipo", "-archs", path]),
            "install_name": output(["otool", "-D", path]).splitlines()[-1].strip(), "bytes": os.path.getsize(path)}


def zipped_bytes(path):
    with tempfile.TemporaryDirectory(dir=BUILD) as scratch:
        archive = os.path.join(scratch, "framework.zip")
        run(["ditto", "-c", "-k", "--keepParent", path, archive])
        return os.path.getsize(archive)


def kilobytes(path):
    return int(output(["du", "-sk", path]).split()[0])


def fetch():
    moltenvk, headers = manifest()
    os.makedirs(BUILD, exist_ok=True)
    start = time.monotonic()
    fetched = {asset["name"]: fetch_asset(asset) for asset in moltenvk["assets"]}
    fetched.update({item["path"]: fetch_file(item) for item in headers["files"]})
    download_seconds = round(time.monotonic() - start, 1)
    if sum(fetched.values()) > moltenvk["cap_bytes"]:
        raise SystemExit(f"fetch-moltenvk: {sum(fetched.values())} bytes fetched, above the approved "
                         f"{moltenvk['cap_bytes']}; stopping")

    unpacked = {}
    for asset in moltenvk["assets"]:
        destination = os.path.join(BUILD, asset["name"][:-len(".tar")])
        unpack(os.path.join(BUILD, "downloads", asset["name"]), destination, asset["sha256"])
        unpacked[asset["name"]] = destination

    ios, macos = unpacked["MoltenVK-ios.tar"], unpacked["MoltenVK-macos.tar"]
    copy(find(ios, os.path.join("dynamic", "MoltenVK.xcframework")), XCFRAMEWORK)
    copy(find(macos, os.path.join("dynamic", "dylib", "macOS", "libMoltenVK.dylib")),
         os.path.join(BUILD, "macos", "libMoltenVK.dylib"))
    header = find(ios, os.path.join("include", "vulkan", "vulkan_core.h"), shortest=True)
    copy(os.path.dirname(os.path.dirname(header)), os.path.join(BUILD, "include"))

    slices = sorted(name for name in os.listdir(XCFRAMEWORK) if os.path.isdir(os.path.join(XCFRAMEWORK, name)))
    framework = {}
    for name in slices:
        directory = os.path.join(XCFRAMEWORK, name, "MoltenVK.framework")
        framework[name] = check_binary(os.path.join(directory, "MoltenVK"), ios=True)
        framework[name]["installed"] = 1024 * kilobytes(directory)
        framework[name]["zipped"] = zipped_bytes(directory)
    stamp = {
        "moltenvk": {"tag": moltenvk["tag"], "commit": moltenvk["commit"]},
        "vulkan_headers": {"tag": headers["tag"], "commit": headers["commit"]},
        "fetched_bytes": fetched,
        "download_seconds": download_seconds,
        "xcframework_slices": slices,
        "framework": framework,
        "macos_dylib": check_binary(os.path.join(BUILD, "macos", "libMoltenVK.dylib"), ios=False),
        "licenses": license_files(ios),
        "disk_kb": kilobytes(BUILD),
        "finished": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    with open(os.path.join(BUILD, "stamp.json"), "w") as handle:
        json.dump(stamp, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(json.dumps(stamp, indent=2, sort_keys=True))


def licenses():
    """The license texts of the projects inside MoltenVK's binary that its release does not carry, at the
    commits of MoltenVK's ExternalRevisions/, into third_party/moltenvk/licenses/."""
    entry = license_manifest()
    fetched = {item["path"]: fetch_file(item, LICENSES) for item in entry["files"]}
    if sum(fetched.values()) > entry["cap_bytes"]:
        raise SystemExit(f"fetch-moltenvk: {sum(fetched.values())} bytes fetched, above the approved "
                         f"{entry['cap_bytes']}")
    print(json.dumps({"fetched_bytes": fetched}, indent=2, sort_keys=True))


def expect(condition, what):
    if not condition:
        raise SystemExit("fetch-moltenvk: self-test failed: " + what)
    print("self-test: " + what, flush=True)


def refused(action):
    try:
        action()
    except (SystemExit, tarfile.FilterError):
        return True
    return False


def self_test():
    """The script's own guards, without the network: writes stay inside build/moltenvk/ and the
    XCFramework, an archive that leaves its folder is refused, and a file that does not match its
    SHA-256 or Git blob is refused."""
    directory = os.path.join(BUILD, "self-test")
    require_inside(directory)
    if os.path.isdir(directory):
        shutil.rmtree(directory)
    os.makedirs(directory)
    try:
        expect(refused(lambda: require_inside(os.path.join(ROOT, "runtime"))),
               "a write outside build/moltenvk/ and build/MoltenVK.xcframework is refused")
        expect(refused(lambda: require_inside(MANIFEST)) and
               not refused(lambda: require_inside(os.path.join(LICENSES, "sample"))),
               "in third_party/ only a write below third_party/moltenvk/licenses/ is taken")

        for member, kind in (("../outside", "a member with '..'"), ("/tmp/absolute", "an absolute member")):
            archive = os.path.join(directory, "bad.tar")
            with tarfile.open(archive, "w") as tar:
                content = b"x"
                info = tarfile.TarInfo(member)
                info.size = len(content)
                tar.addfile(info, io.BytesIO(content))
            expect(refused(lambda: unpack(archive, os.path.join(directory, "out"), "0" * 64)),
                   f"an archive with {kind} is refused")

        archive = os.path.join(directory, "link.tar")
        with tarfile.open(archive, "w") as tar:
            info = tarfile.TarInfo("link")
            info.type = tarfile.SYMTYPE
            info.linkname = "../../outside"
            tar.addfile(info)
        expect(refused(lambda: unpack(archive, os.path.join(directory, "out"), "0" * 64)),
               "an archive with a link that leaves it is refused")

        sample = os.path.join(directory, "sample")
        with open(sample, "wb") as handle:
            handle.write(b"sample")
        expect(refused(lambda: check_sha256(sample, "0" * 64, "sample")) and
               not refused(lambda: check_sha256(sample, sha256_file(sample), "sample")),
               "a file that does not match its SHA-256 is refused, one that does is taken")
        expect(refused(lambda: check_blob(b"sample", "0" * 40, "sample")) and
               not refused(lambda: check_blob(b"sample", git_blob(b"sample"), "sample")),
               "content that does not match its Git blob is refused, content that does is taken")
    finally:
        shutil.rmtree(directory)
    print("self-test: passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fetch", action="store_true", help="fetch, check and unpack (network)")
    parser.add_argument("--self-test", action="store_true", help="check the script's guards (no network)")
    parser.add_argument("--licenses", action="store_true", help="fetch the license texts the release lacks (network)")
    options = parser.parse_args()
    if not options.fetch and not options.self_test and not options.licenses:
        parser.error("nothing to do: --self-test, --fetch or --licenses")
    if options.self_test:
        self_test()
    if options.fetch:
        fetch()
    if options.licenses:
        licenses()


if __name__ == "__main__":
    main()
