#!/usr/bin/env python3
"""Fetches the AOSP sources of the guest libc listed in third_party/sources.json (ADR-0021).

Each source is an archive of one commit, or single files and folders of it, from
android.googlesource.com, extracted to build/sources/<name>/. The content is checked against the pinned digest in the list:
SHA-256 over the sorted lines "<path> NUL <SHA-256 of the file>", which does not depend on how the
server packs the archive. A source whose digest matches is not fetched again.

Usage: tools/fetch-sources.py [--pin] [name...]
  --pin   write the digests of what was fetched into the list (only for a new or changed entry)
"""
import base64
import hashlib
import io
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
DESTINATION = os.path.join(ROOT, "build", "sources")


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def digest(directory):
    lines = []
    for folder, _, names in os.walk(directory):
        for name in names:
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, directory)
            if os.path.islink(path):
                content = ("link:" + os.readlink(path)).encode()
            else:
                with open(path, "rb") as handle:
                    content = handle.read()
            lines.append(relative.encode() + b"\0" + hashlib.sha256(content).hexdigest().encode() + b"\n")
    return hashlib.sha256(b"".join(sorted(lines))).hexdigest()


def extract(archive, directory):
    """Extracts a .tar.gz, refusing absolute paths, '..' and anything but files, folders and links.
    Links that point out of the archive (AOSP has a few into sibling projects) are left out."""
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        for member in tar.getmembers():
            parts = member.name.split("/")
            if member.name.startswith("/") or ".." in parts:
                raise SystemExit(f"refusing archive member {member.name!r}")
            if not (member.isfile() or member.isdir() or member.issym()):
                raise SystemExit(f"refusing archive member {member.name!r} of an unexpected kind")
            if member.issym():
                resolved = os.path.normpath(os.path.join(os.path.dirname(member.name), member.linkname))
                if member.linkname.startswith("/") or resolved == ".." or resolved.startswith("../"):
                    print(f"  left out link {member.name} -> {member.linkname}")
                    continue
            tar.extract(member, directory)


def main():
    arguments = sys.argv[1:]
    pin = "--pin" in arguments
    wanted = [argument for argument in arguments if argument != "--pin"]
    with open(MANIFEST) as handle:
        manifest = json.load(handle)
    host = manifest["host"]
    changed = False
    os.makedirs(DESTINATION, exist_ok=True)

    for source in manifest["sources"]:
        name = source["name"]
        if wanted and name not in wanted:
            continue
        target = os.path.join(DESTINATION, name)
        if source["digest"] and os.path.isdir(target) and digest(target) == source["digest"]:
            print(f"{name}: up to date")
            continue

        staging = tempfile.mkdtemp(dir=DESTINATION, prefix=f".{name}-")
        try:
            downloaded = 0
            if "files" in source or "directories" in source:
                for directory in source.get("directories", []):
                    data = fetch(f"{host}/{source['project']}/+archive/{source['commit']}/{directory}.tar.gz")
                    downloaded += len(data)
                    extract(data, os.path.join(staging, directory))
                for path in source.get("files", []):
                    url = f"{host}/{source['project']}/+/{source['commit']}/{path}?format=TEXT"
                    data = fetch(url)
                    downloaded += len(data)
                    file_path = os.path.join(staging, path)
                    os.makedirs(os.path.dirname(file_path), exist_ok=True)
                    with open(file_path, "wb") as handle:
                        handle.write(base64.b64decode(data))
            else:
                data = fetch(f"{host}/{source['project']}/+archive/{source['commit']}.tar.gz")
                downloaded = len(data)
                extract(data, staging)

            actual = digest(staging)
            if source["digest"] and actual != source["digest"]:
                raise SystemExit(f"{name}: digest {actual} does not match the pinned {source['digest']}")
            if not source["digest"]:
                if not pin:
                    raise SystemExit(f"{name}: no pinned digest; check the source, then run with --pin")
                source["digest"] = actual
                changed = True
            shutil.rmtree(target, ignore_errors=True)
            os.rename(staging, target)
            print(f"{name}: {downloaded / 1e6:.2f} MB fetched, digest {actual}")
        finally:
            shutil.rmtree(staging, ignore_errors=True)

    if changed:
        with open(MANIFEST, "w") as handle:
            json.dump(manifest, handle, indent=2)
            handle.write("\n")
        print(f"pinned new digests in {os.path.relpath(MANIFEST, ROOT)}")


if __name__ == "__main__":
    main()
