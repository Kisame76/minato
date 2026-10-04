#!/usr/bin/env python3
"""The bundle's license rule for test 2's twin (ADR-0042 c) 2), CTest's godot.licenses: a benchmark build that embeds
MinatoGodot.framework carries Godot's LICENSE.txt and COPYRIGHT.txt in Licenses/, and tools/license-check.py sees it.
It runs tools/bundle-licenses.py and license-check.py's check on fake bundles, with third_party/licenses.json's section
"Test 2 with Godot" alone, so that it needs none of the other sections' fetched texts:
  - the twin's bundle as bundle-licenses.py fills it: Godot's two texts, byte for byte, and no problem;
  - the twin's bundle without them: the check names the export templates' row;
  - a bundle without the twin: no Godot texts, and no problem.
Runs on /usr/bin/python3 (3.9)."""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECTION = "Test 2 with Godot"
TEXTS = ("LICENSE.txt", "COPYRIGHT.txt")
failures = []


def check(condition, what):
    print(("ok      " if condition else "FAILED  ") + what, flush=True)
    if not condition:
        failures.append(what)


def license_check():
    spec = importlib.util.spec_from_file_location("license_check", os.path.join(ROOT, "tools", "license-check.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def section_markdown():
    """THIRD_PARTY.md's section of test 2 alone."""
    with open(os.path.join(ROOT, "THIRD_PARTY.md")) as handle:
        lines = handle.read().splitlines()
    start = next(index for index, line in enumerate(lines) if line.startswith(f"## {SECTION}"))
    end = next((index for index in range(start + 1, len(lines)) if lines[index].startswith("## ")), len(lines))
    return "\n".join(lines[start:end]) + "\n"


def fake_repo(scratch):
    """A repository root with licenses.json cut down to the section, and Godot's texts where the section names them."""
    with open(os.path.join(ROOT, "third_party", "licenses.json")) as handle:
        config = json.load(handle)
    cut = {"sections": {SECTION: config["sections"][SECTION]},
           "components": [component for component in config["components"] if component["section"] == SECTION]}
    repo = os.path.join(scratch, "repo")
    os.makedirs(os.path.join(repo, "third_party"))
    shutil.copytree(os.path.join(ROOT, "third_party", "godot"), os.path.join(repo, "third_party", "godot"))
    with open(os.path.join(repo, "third_party", "licenses.json"), "w") as handle:
        json.dump(cut, handle)
    return repo, cut


def bundle(scratch, name, repo, twin):
    """A built Minato.app as Xcode leaves it for bundle-licenses.py, with the twin's framework or without."""
    build = os.path.join(scratch, name)
    app = os.path.join(build, "Minato.app")
    os.makedirs(os.path.join(app, "Frameworks", "MinatoGodot.framework") if twin else os.path.join(app, "Frameworks"))
    environment = dict(os.environ, SRCROOT=repo, TARGET_BUILD_DIR=build, UNLOCALIZED_RESOURCES_FOLDER_PATH="Minato.app",
                       FRAMEWORKS_FOLDER_PATH="Minato.app/Frameworks")
    filled = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "bundle-licenses.py")], env=environment,
                            capture_output=True, text=True)
    check(filled.returncode == 0, f"bundle-licenses.py fills the {name} bundle ({filled.stderr.strip()[-200:]})")
    return app


def main():
    checker = license_check()
    markdown = section_markdown()
    rows = next((found for heading, found in checker.sections(markdown).items() if heading.startswith(SECTION)), [])
    check(len(rows) >= 3, f"THIRD_PARTY.md's section has its rows ({len(rows)})")
    scratch = tempfile.mkdtemp(prefix="license-check-test-")
    try:
        repo, config = fake_repo(scratch)
        app = bundle(scratch, "twin", repo, twin=True)
        same = all(os.path.isfile(os.path.join(app, "Licenses", "godot", text)) and
                   open(os.path.join(app, "Licenses", "godot", text), "rb").read() ==
                   open(os.path.join(ROOT, "third_party", "godot", text), "rb").read() for text in TEXTS)
        check(same, "the twin's bundle carries Godot's LICENSE.txt and COPYRIGHT.txt as third_party/godot/ has them")
        problems = checker.check(app, config, markdown)
        check(not problems, f"the license check passes the twin's bundle ({problems})")
        shutil.rmtree(os.path.join(app, "Licenses"))
        os.makedirs(os.path.join(app, "Licenses"))
        with open(os.path.join(app, "Licenses", "licenses.json"), "w") as handle:
            json.dump({"components": []}, handle)
        problems = checker.check(app, config, markdown)
        check(any("export templates" in problem for problem in problems),
              f"without Godot's texts the check names the export templates' row ({problems})")
        app = bundle(scratch, "plain", repo, twin=False)
        check(not os.path.exists(os.path.join(app, "Licenses", "godot")), "a bundle without the twin gets no Godot texts")
        problems = checker.check(app, config, markdown)
        check(not problems, f"and the license check passes it ({problems})")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    if failures:
        raise SystemExit(f"license-check-test: {len(failures)} failed")
    print("license-check-test: passed", flush=True)


if __name__ == "__main__":
    main()
