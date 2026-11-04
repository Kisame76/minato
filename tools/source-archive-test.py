#!/usr/bin/env python3
"""The source archive of a release (ADR-0033's GPL-2.0 with the Classpath Exception parts; tools/make-source-archive.py,
tools/bundle-licenses.py's SOURCE.txt, tools/license-check.py), CTest's release.source_archive, on a fake tree:
  - the archive holds README-SOURCE.txt, the pinned inputs and the scripts under one folder, sorted, with time 0, owner
    0 and the same modes, so that making it twice gives the same bytes and the same SHA-256 file;
  - an input whose content digest is not the pinned one stops it, and no archive is left;
  - a missing script of ours stops it too;
  - SOURCE.txt names the release page and the archive's SHA-256 when the build knows them, and says a build is no
    release when it does not;
  - license-check.py fails a bundle whose GPL component lacks SOURCE.txt.
Runs on /usr/bin/python3 (3.9)."""
import hashlib
import importlib.util
import json
import os
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failures = []


def check(condition, what):
    print(("ok      " if condition else "FAILED  ") + what, flush=True)
    if not condition:
        failures.append(what)


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(text)


def main():
    archive = load("make_source_archive", "make-source-archive.py")
    fetch = load("fetch_sources", "fetch-sources.py")
    with tempfile.TemporaryDirectory() as folder:
        root = os.path.join(folder, "repo")
        sources = os.path.join(folder, "sources")
        write(os.path.join(sources, "libcore", "ojluni", "A.java"), "class A {}\n")
        write(os.path.join(sources, "libcore", "LICENSE"), "GPL text\n")
        write(os.path.join(sources, "art", "openjdkjvm", "jvm.cc"), "int jvm;\n")
        os.symlink("A.java", os.path.join(sources, "libcore", "ojluni", "Link.java"))
        write(os.path.join(root, "runtime", "art", "patches", "0001.patch"), "patch\n")
        write(os.path.join(root, "tools", "art-plan.py"), "plan\n")
        os.chmod(os.path.join(root, "tools", "art-plan.py"), 0o755)
        pins = {"art": [{"name": name, "commit": commit * 40, "digest": fetch.digest(os.path.join(sources, name))}
                        for name, commit in (("libcore", "a"), ("art", "b"))]}
        write(os.path.join(root, "third_party", "sources.json"), json.dumps(pins))
        ours = ["runtime/art", "tools/art-plan.py", "third_party/sources.json"]
        arguments = dict(root=root, sources=sources, version="1.0.0-beta.1", release_url="https://example.test/r/v1",
                         names=["libcore", "art"], ours=ours)

        first = archive.make(out=os.path.join(folder, "one"), **arguments)
        second = archive.make(out=os.path.join(folder, "two"), **arguments)
        with open(first, "rb") as one, open(second, "rb") as two:
            first_bytes, second_bytes = one.read(), two.read()
        check(first_bytes == second_bytes, "making the archive twice gives the same bytes")
        check(os.path.basename(first) == "Minato-1.0.0-beta.1-source.tar.gz", "the archive is named for the version")
        with open(first + ".sha256") as handle:
            recorded = handle.read().split()
        check(recorded == [hashlib.sha256(first_bytes).hexdigest(), os.path.basename(first)],
              "the .sha256 file names the archive's SHA-256")

        with tarfile.open(first) as tar:
            members = tar.getmembers()
        names = [member.name for member in members]
        top = "Minato-1.0.0-beta.1-source/"
        check(all(name.startswith(top) or name == top.rstrip("/") for name in names), "everything is under one folder")
        for wanted in ("README-SOURCE.txt", "inputs/libcore/ojluni/A.java", "inputs/libcore/LICENSE",
                       "inputs/art/openjdkjvm/jvm.cc", "ours/runtime/art/patches/0001.patch", "ours/tools/art-plan.py",
                       "ours/third_party/sources.json"):
            check(top + wanted in names, f"the archive holds {wanted}")
        check(names == sorted(names), "the members are sorted by name")
        check(all(m.mtime == 0 and m.uid == 0 and m.gid == 0 and m.uname == "" and m.gname == "" for m in members),
              "time, owner and group are fixed")
        modes = {m.name[len(top):]: m.mode for m in members if m.isfile()}
        check(modes.get("ours/tools/art-plan.py") == 0o755 and modes.get("inputs/libcore/LICENSE") == 0o644,
              "modes are 0644 and 0755 only")
        links = [m for m in members if m.issym()]
        check([m.linkname for m in links] == ["A.java"], "a link of an input stays a link")
        with tarfile.open(first) as tar:
            readme = tar.extractfile(top + "README-SOURCE.txt").read().decode()
        check("https://example.test/r/v1" in readme and "a" * 40 in readme and "b" * 40 in readme,
              "README-SOURCE.txt names the release page and the pinned commits")
        check("offer" not in readme.lower(), "README-SOURCE.txt makes no written offer (3(a): same page as the IPA)")

        # A changed input: the pinned digest no longer matches.
        write(os.path.join(sources, "libcore", "ojluni", "A.java"), "class A { int changed; }\n")
        try:
            archive.make(out=os.path.join(folder, "bad"), **arguments)
            refused = False
        except SystemExit:
            refused = True
        check(refused and not os.path.exists(os.path.join(folder, "bad", "Minato-1.0.0-beta.1-source.tar.gz")),
              "an input that is not the pinned one stops the archive")
        write(os.path.join(sources, "libcore", "ojluni", "A.java"), "class A {}\n")
        try:
            archive.make(out=os.path.join(folder, "gone"), **dict(arguments, ours=["tools/missing.py"]))
            refused = False
        except SystemExit:
            refused = True
        check(refused, "a missing script of ours stops the archive")

    # SOURCE.txt, as bundle-licenses.py writes it, and license-check.py's view of a bundle without it.
    bundle = load("bundle_licenses", "bundle-licenses.py")
    note = bundle.source_note("core-oj", "https://example.test/r/v1", "c" * 64)
    check("https://example.test/r/v1" in note and "c" * 64 in note and "core-oj" in note
          and "Minato-" in note and "GPL-2.0" in note,
          "SOURCE.txt names the component, the release page and the SHA-256")
    plain = bundle.source_note("core-oj", "", "")
    check("not a release build" in plain and "https://" not in plain,
          "SOURCE.txt of a build that is no release says so")

    license_check = load("license_check", "license-check.py")
    config = {"sections": {"S": "always"},
              "components": [{"name": "core-oj", "section": "S", "license": "GPL-2.0 WITH Classpath-exception-2.0",
                              "rows": ["row"], "texts": [{"generate": "source", "as": "SOURCE.txt"},
                                                         {"from": "x", "as": "LICENSE"}]}]}
    with tempfile.TemporaryDirectory() as folder:
        app = os.path.join(folder, "Minato.app")
        write(os.path.join(app, "Licenses", "core-oj", "LICENSE"), "text\n")
        index = {"components": [{"name": "core-oj", "section": "S", "license": "x", "rows": ["row"],
                                 "files": ["LICENSE"]}]}
        write(os.path.join(app, "Licenses", "licenses.json"), json.dumps(index))
        problems = license_check.check(app, config, "## S\n\n| Source | License |\n|---|---|\n| `row` | x |\n")
        check(any("SOURCE.txt" in problem for problem in problems),
              "a GPL component without SOURCE.txt fails the check")
        write(os.path.join(app, "Licenses", "core-oj", "SOURCE.txt"), "where\n")
        index["components"][0]["files"] = ["SOURCE.txt", "LICENSE"]
        write(os.path.join(app, "Licenses", "licenses.json"), json.dumps(index))
        check(license_check.check(app, config, "## S\n\n| Source | License |\n|---|---|\n| `row` | x |\n") == [],
              "with SOURCE.txt there is no problem")

    print(f"{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
