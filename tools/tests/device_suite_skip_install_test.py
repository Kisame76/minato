#!/usr/bin/env python3
"""tools/device-suite.sh with SKIP_INSTALL=1 builds nothing and installs nothing, and runs the test of the app that is
already on the device (for a run in airplane mode); without the flag it builds and installs first. The script runs in a
private copy of the tree whose build, install and device tools are stubs that log their calls, with fake xcrun, cmake
and codesign in front of PATH. Python 3.9.

Usage: tools/tests/device_suite_skip_install_test.py"""
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def write(path, text, mode=0o755):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


def make_tree(root):
    log = os.path.join(root, "calls.log")
    os.makedirs(os.path.join(root, "tools"), exist_ok=True)
    shutil.copy(os.path.join(REPO, "tools", "device-suite.sh"), os.path.join(root, "tools", "device-suite.sh"))
    for stub in ("build-fixtures.sh", "build-bionic.sh", "deploy.sh", "sim.sh"):
        write(os.path.join(root, "tools", stub), f'#!/bin/sh\necho "{stub}" >> "{log}"\n')
    write(os.path.join(root, "tools", "require-device.py"), "#!/usr/bin/env python3\n")
    write(os.path.join(root, "tools", "find-device.py"), '#!/usr/bin/env python3\nprint("DEVICE-1 iPhone")\n')
    write(os.path.join(root, "fixtures", "tests.txt"), "demo-test system\n", 0o644)
    write(os.path.join(root, "fixtures", "expected", "demo-test.out"), "hello\n", 0o644)
    bin_dir = os.path.join(root, "fakebin")
    write(os.path.join(bin_dir, "cmake"), f'#!/bin/sh\necho "cmake $*" >> "{log}"\n')
    write(os.path.join(bin_dir, "codesign"), "#!/bin/sh\nexit 1\n")
    # devicectl: a launch writes the JSON devicectl would; a copy makes the program's output folder.
    write(os.path.join(bin_dir, "xcrun"), f'''#!/usr/bin/env python3
import json, os, sys
a = sys.argv[1:]
open({log!r}, "a").write("xcrun " + " ".join(a[:4]) + "\\n")
if a[:4] == ["devicectl", "device", "process", "launch"]:
    out = a[a.index("--json-output") + 1]
    json.dump({{"result": {{"terminationResult": {{}}}}}}, open(out, "w"))
elif a[:4] == ["devicectl", "device", "copy", "from"]:
    dest = a[a.index("--destination") + 1]
    os.makedirs(dest)
    for name, text in (("stdout", "hello\\n"), ("stderr", ""), ("status", "0\\n"), ("traced", "0\\n")):
        open(os.path.join(dest, name), "w").write(text)
''')
    return log


def run(skip):
    with tempfile.TemporaryDirectory() as root:
        log = make_tree(root)
        env = dict(os.environ, PATH=os.path.join(root, "fakebin") + os.pathsep + os.environ["PATH"])
        env.pop("SKIP_INSTALL", None)
        if skip:
            env["SKIP_INSTALL"] = "1"
        done = subprocess.run(["bash", os.path.join(root, "tools", "device-suite.sh"), "demo-test"], cwd=root, env=env,
                              capture_output=True, text=True)
        calls = open(log).read().split("\n") if os.path.exists(log) else []
        return done, [c for c in calls if c]


failures = []


def check(condition, what):
    if not condition:
        failures.append(what)


done, calls = run(skip=False)
check(done.returncode == 0, "without the flag the run passes: " + done.stdout + done.stderr)
check("deploy.sh" in calls, "without the flag the app is installed")
check("build-fixtures.sh" in calls and "build-bionic.sh" in calls, "without the flag the bundle is built")

done, calls = run(skip=True)
check(done.returncode == 0, "with the flag the run passes: " + done.stdout + done.stderr)
check("deploy.sh" not in calls, "with the flag nothing is installed: " + repr(calls))
check("sim.sh" not in calls, "with the flag no Simulator install either")
check("build-fixtures.sh" not in calls and "build-bionic.sh" not in calls and not any(c.startswith("cmake") for c in calls),
      "with the flag nothing is built: " + repr(calls))
check("xcrun devicectl device process launch" in calls, "with the flag the test is still launched")
check("xcrun devicectl device copy from" in calls, "with the flag its output is still fetched")
check("same     demo-test" in done.stdout, "with the flag the output is still compared: " + done.stdout)

if failures:
    for failure in failures:
        print("FAILED:", failure)
    sys.exit(1)
print("device-suite SKIP_INSTALL: fine")
