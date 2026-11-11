#!/usr/bin/env python3
"""The license texts for what THIRD_PARTY.md lists, on a bundle as the app build fills it (CTest tools.license_rows).

tools/license-check.py checks a built Minato.app, and tools/release.sh runs it, but a new row in THIRD_PARTY.md that no
component of third_party/licenses.json covers would only show at the next release. This runs the build's own steps on
a fake bundle that embeds every section (a folder for each framework a rule names, an empty file for each path a rule
names): tools/bundle-licenses.py with the repository's licenses.json and the texts where the build fetched them, then
license-check.py on the result. It fails with the check's own lines (a row without a component, a component without a
text, a section without a rule), and with bundle-licenses.py's error when a text is not where the config says.
It needs build/sources (tools/fetch-sources.py); without it the test is skipped (exit 77, which CTest lists as
Skipped). Runs on /usr/bin/python3 (3.9)."""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIPPED = 77

if not os.path.isdir(os.path.join(ROOT, "build", "sources")):
    print("skipped: build/sources is missing (tools/fetch-sources.py fetches it)")
    sys.exit(SKIPPED)

with open(os.path.join(ROOT, "third_party", "licenses.json")) as handle:
    rules = set(json.load(handle)["sections"].values())

with tempfile.TemporaryDirectory(prefix="license-rows-") as target:
    app = os.path.join(target, "Minato.app")
    os.makedirs(os.path.join(app, "Frameworks"))
    for rule in sorted(rules - {"always", "never"}):
        path = os.path.join(app, rule) if "/" in rule else os.path.join(app, "Frameworks", rule)
        if "/" in rule:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "wb").close()
        else:
            os.makedirs(path)
    env = dict(os.environ, SRCROOT=ROOT, TARGET_BUILD_DIR=target, UNLOCALIZED_RESOURCES_FOLDER_PATH="Minato.app",
               FRAMEWORKS_FOLDER_PATH="Minato.app/Frameworks")
    filled = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "bundle-licenses.py")], env=env,
                            capture_output=True, text=True)
    if filled.returncode != 0:
        print(filled.stderr.strip())
        print("FAILED  tools/bundle-licenses.py cannot fill a bundle that embeds every section")
        sys.exit(1)
    checked = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "license-check.py"), app],
                             capture_output=True, text=True)
    if checked.returncode != 0:
        print((checked.stdout + checked.stderr).strip())
        print("FAILED  license-check.py finds THIRD_PARTY.md rows or sections without a license text")
        sys.exit(1)
    print("ok      every row of THIRD_PARTY.md has its license text and every section has a rule")
