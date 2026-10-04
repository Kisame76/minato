#!/usr/bin/env python3
"""The bundle's license rules for test 2 (ADR-0042 c) 2), CTest's godot.licenses: a benchmark build that embeds
MinatoGodot.framework carries Godot's LICENSE.txt and COPYRIGHT.txt in Licenses/, one that holds the Godot APKs in
Benchmark/ carries the texts of what the APKs hold, and tools/license-check.py sees both. It runs
tools/bundle-licenses.py and license-check.py's check on fake bundles, with third_party/licenses.json cut to one
section, so that it needs none of the other sections' fetched texts. For each of the two sections:
  - a bundle that holds the section's files, as bundle-licenses.py fills it: every text of the section's components,
    byte for byte as in the repository, and no problem;
  - the same bundle without the texts: the check names the section's rows and components;
  - a bundle without the files: none of the section's texts, and no problem.
The rows come from THIRD_PARTY.md; a section it does not have yet is checked without them, with a note.
A rule may name a file in the bundle (a path with a slash), as the APKs' does: a cut config of its own, with the rule
"Benchmark/x.apk", must be filled and checked when the bundle holds that file only.
Runs on /usr/bin/python3 (3.9)."""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Each section of test 2, with the files that embed it in a bundle (a trailing slash makes a folder).
SECTIONS = (("Test 2 with Godot", ["Frameworks/MinatoGodot.framework/"]),
            ("Test 2's APKs", ["Benchmark/godot-bench-gles.apk", "Benchmark/godot-bench-vulkan.apk"]))
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


def section_markdown(section):
    """THIRD_PARTY.md's section alone; empty when it has none of that name."""
    with open(os.path.join(ROOT, "THIRD_PARTY.md")) as handle:
        lines = handle.read().splitlines()
    start = next((index for index, line in enumerate(lines) if line.startswith(f"## {section}")), None)
    if start is None:
        return ""
    end = next((index for index in range(start + 1, len(lines)) if lines[index].startswith("## ")), len(lines))
    return "\n".join(lines[start:end]) + "\n"


def fake_repo(scratch, config):
    """A repository root with this licenses.json and the texts it names from the repository."""
    repo = os.path.join(scratch, "repo")
    for component in config["components"]:
        for text in component.get("texts", []):
            base, _, path = text["from"].partition(":")
            if base != "repo":
                raise SystemExit(f"license-check-test: {component['name']} takes a text from {base}, not the repo")
            os.makedirs(os.path.dirname(os.path.join(repo, path)), exist_ok=True)
            shutil.copy(os.path.join(ROOT, path), os.path.join(repo, path))
    os.makedirs(os.path.join(repo, "third_party"), exist_ok=True)
    with open(os.path.join(repo, "third_party", "licenses.json"), "w") as handle:
        json.dump(config, handle)
    return repo


def bundle(scratch, name, repo, files):
    """A built Minato.app as Xcode leaves it for bundle-licenses.py, holding these files, then filled by it."""
    build = os.path.join(scratch, name)
    app = os.path.join(build, "Minato.app")
    os.makedirs(os.path.join(app, "Frameworks"))
    for path in files:
        if path.endswith("/"):
            os.makedirs(os.path.join(app, path))
        else:
            os.makedirs(os.path.dirname(os.path.join(app, path)), exist_ok=True)
            open(os.path.join(app, path), "w").close()
    environment = dict(os.environ, SRCROOT=repo, TARGET_BUILD_DIR=build, UNLOCALIZED_RESOURCES_FOLDER_PATH="Minato.app",
                       FRAMEWORKS_FOLDER_PATH="Minato.app/Frameworks")
    filled = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "bundle-licenses.py")], env=environment,
                            capture_output=True, text=True)
    check(filled.returncode == 0, f"bundle-licenses.py fills the bundle {name} ({filled.stderr.strip()[-200:]})")
    return app


def section_cases(checker, scratch, section, files):
    with open(os.path.join(ROOT, "third_party", "licenses.json")) as handle:
        config = json.load(handle)
    rule = config["sections"].get(section)
    check(rule is not None and rule not in ("always", "never"), f"{section}: a rule by a bundle's file ({rule})")
    config = {"sections": {section: rule},
              "components": [component for component in config["components"] if component["section"] == section]}
    texts = [(component["name"], text) for component in config["components"] for text in component.get("texts", [])]
    check(bool(texts), f"{section}: its components name texts ({len(texts)})")
    markdown = section_markdown(section)
    rows = next((found for heading, found in checker.sections(markdown).items() if heading.startswith(section)), [])
    if not rows:
        print(f"note    {section}: THIRD_PARTY.md has no rows of it yet, so they are not checked", flush=True)
    else:
        named = {row for component in config["components"] for row in component["rows"]}
        check(set(rows) <= named,
              f"{section}: a component covers each row of THIRD_PARTY.md ({sorted(set(rows) - named)})")
    scratch = os.path.join(scratch, section.replace("'", "").replace(" ", "-"))
    repo = fake_repo(scratch, config)
    app = bundle(scratch, "with", repo, files)
    same = all(os.path.isfile(os.path.join(app, "Licenses", name, text["as"])) and
               open(os.path.join(app, "Licenses", name, text["as"]), "rb").read() ==
               open(os.path.join(ROOT, text["from"].partition(":")[2]), "rb").read() for name, text in texts)
    check(same, f"{section}: the bundle carries every text of its components, as the repository has it")
    problems = checker.check(app, config, markdown)
    check(not problems, f"{section}: the license check passes the bundle ({problems})")
    for name, _ in texts:
        shutil.rmtree(os.path.join(app, "Licenses", name), ignore_errors=True)
    with open(os.path.join(app, "Licenses", "licenses.json"), "w") as handle:
        json.dump({"components": []}, handle)
    problems = checker.check(app, config, markdown)
    exempt = {row for component in config["components"] if component.get("none") for row in component["rows"]}
    named = [row for row in rows if row not in exempt] + sorted({name for name, _ in texts})
    check(all(any(name in problem for problem in problems) for name in named),
          f"{section}: without the texts the check names its rows and components ({problems})")
    app = bundle(scratch, "without", repo, [])
    check(not any(os.path.exists(os.path.join(app, "Licenses", name)) for name, _ in texts),
          f"{section}: a bundle without its files gets none of its texts")
    problems = checker.check(app, config, markdown)
    check(not problems, f"{section}: and the license check passes it ({problems})")


def path_rule(checker, scratch):
    """A rule that names a file in the bundle, on a cut config of its own."""
    markdown = "## Test APKs\n\n| Part | License |\n|---|---|\n| Part in the APK | MIT |\n"
    config = {"sections": {"Test APKs": "Benchmark/x.apk"},
              "components": [{"name": "x", "section": "Test APKs", "license": "MIT", "rows": ["Part in the APK"],
                              "texts": [{"from": "repo:third_party/godot/LICENSE.txt", "as": "LICENSE.txt"}]}]}
    scratch = os.path.join(scratch, "path-rule")
    repo = fake_repo(scratch, config)
    for name, files in (("with-apk", ["Benchmark/x.apk"]), ("without-apk", [])):
        app = bundle(scratch, name, repo, files)
        copied = os.path.isfile(os.path.join(app, "Licenses", "x", "LICENSE.txt"))
        check(copied == bool(files), f"a path rule's texts go in {'with' if files else 'only with'} the file")
        problems = checker.check(app, config, markdown)
        check(not problems, f"and the check passes that bundle ({problems})")
        if files:
            shutil.rmtree(os.path.join(app, "Licenses", "x"), ignore_errors=True)
            problems = checker.check(app, config, markdown)
            check(any("Part in the APK" in problem for problem in problems),
                  f"without the texts the check names the APK's row ({problems})")


def main():
    checker = license_check()
    scratch = tempfile.mkdtemp(prefix="license-check-test-")
    try:
        for section, files in SECTIONS:
            section_cases(checker, scratch, section, files)
        path_rule(checker, scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    if failures:
        raise SystemExit(f"license-check-test: {len(failures)} failed")
    print("license-check-test: passed", flush=True)


if __name__ == "__main__":
    main()
