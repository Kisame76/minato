#!/usr/bin/env python3
"""tools/make-tzdata.py with --pin-from: bionic's tzdata is packed from the tz release that icu-data's files are pinned
for (third_party/sources.json), not from whatever release the Mac holds today. The source folder is, in this order:
the Mac's folder of the pinned release below /var/db/timezone/tz/<release>.*/zoneinfo (the highest suffix; a folder
without Etc/UTC or of another +VERSION does not count), then /usr/share/zoneinfo when its +VERSION is the pin, else
the tool stops and names the two ways out. MINATO_TZ_ROOT puts a fake root in front of both paths. Python 3.9."""
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import tempfile
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "make-tzdata.py")
SOURCES = os.path.join(HERE, "..", "..", "third_party", "sources.json")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        print("FAIL", what)
        failures += 1


spec = importlib.util.spec_from_file_location("make_tzdata", TOOL)
make_tzdata = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_tzdata)


def clean(path):
    if os.path.exists(path):
        os.remove(path)


def zone_folder(path, version, marker, utc=True):
    """A zoneinfo folder of `version` whose Etc/UTC and Europe/Berlin carry `marker`."""
    os.makedirs(os.path.join(path, "Etc"), exist_ok=True)
    os.makedirs(os.path.join(path, "Europe"), exist_ok=True)
    with open(os.path.join(path, "+VERSION"), "w") as f:
        f.write(version + "\n")
    if utc:
        with open(os.path.join(path, "Etc", "UTC"), "wb") as f:
            f.write(b"TZif2" + marker)
    with open(os.path.join(path, "Europe", "Berlin"), "wb") as f:
        f.write(b"TZif2" + marker)


def tz_root(root, active, folders):
    """A fake root: the active release in usr/share/zoneinfo, and var/db/timezone/tz/<name>/zoneinfo for each of
    `folders` ({"2026e.1.0": (version, marker, utc)})."""
    zone_folder(os.path.join(root, "usr", "share", "zoneinfo"), active, b"active-" + active.encode())
    for name, (version, marker, utc) in folders.items():
        zone_folder(os.path.join(root, "var", "db", "timezone", "tz", name, "zoneinfo"), version, marker, utc)


def sources(path, release):
    entry = {"name": "icu_tzdata", "commit": "0" * 40,
             "files": ["LICENSE"] + [f"tzdata/icunew/{release}/44/le/{n}.res"
                                     for n in ("zoneinfo64", "metaZones", "timezoneTypes", "windowsZones")]}
    with open(path, "w") as f:
        json.dump({"art": [{"name": "other"}, entry]}, f)


def run(root, output, release="2026e", pin_from=None):
    """(exit code, stdout+stderr, packed bytes or None) of the tool as build-bionic.sh calls it."""
    with tempfile.TemporaryDirectory() as scratch:
        listing = pin_from or os.path.join(scratch, "sources.json")
        if pin_from is None:
            sources(listing, release)
        env = dict(os.environ, MINATO_TZ_ROOT=root)
        done = subprocess.run([sys.executable, TOOL, output, "--pin-from", listing], env=env,
                              capture_output=True, text=True)
    data = open(output, "rb").read() if os.path.exists(output) else None
    return done.returncode, done.stdout + done.stderr, data


with tempfile.TemporaryDirectory() as base:
    out = os.path.join(base, "out", "tzdata")

    # 1. The Mac moved on (active 2026f), the pinned release is still held: it is packed, and the line says so.
    root = os.path.join(base, "r1")
    tz_root(root, "2026f", {"2026e.1.0": ("2026e", b"pinned-e", True), "2026f.1.0": ("2026f", b"newer-f", True)})
    code, text, data = run(root, out)
    check(code == 0, "pinned folder present: exit 0, got %s: %s" % (code, text))
    check(data is not None and data.startswith(b"tzdata2026e\0"), "pinned folder: the file is tzdata2026e")
    check(data is not None and b"pinned-e" in data and b"newer-f" not in data and b"active-2026f" not in data,
          "pinned folder: only the pinned release's zones are packed")
    check("active release is 2026f" in text and "packing the pinned 2026e" in text and "2026e.1.0" in text,
          "pinned folder: the one line names the active release, the pin and the folder: %r" % text)

    # 2. Two folders of the pinned release: the highest suffix wins, numerically (1.10 above 1.9).
    clean(out)
    root = os.path.join(base, "r2")
    tz_root(root, "2026f", {"2026e.1.9": ("2026e", b"one-nine", True), "2026e.1.10": ("2026e", b"one-ten", True),
                            "2026e.1.0": ("2026e", b"one-zero", True)})
    code, text, data = run(root, out)
    check(code == 0 and data is not None and b"one-ten" in data and b"one-nine" not in data
          and b"one-zero" not in data, "two folders: 2026e.1.10 wins: %s %r" % (code, text))

    # 3. The pinned folder is gone, the active release is the pin: /usr/share/zoneinfo is packed.
    clean(out)
    root = os.path.join(base, "r3")
    tz_root(root, "2026e", {})
    code, text, data = run(root, out)
    check(code == 0 and data is not None and data.startswith(b"tzdata2026e\0") and b"active-2026e" in data,
          "active equals the pin: packed from /usr/share/zoneinfo: %s %r" % (code, text))
    check("usr/share/zoneinfo" in text and "packing the pinned 2026e" in text,
          "active equals the pin: the line says it")

    # 4. Neither holds the pin: it stops, names the active release and both ways out, and writes nothing.
    clean(out)
    root = os.path.join(base, "r4")
    tz_root(root, "2026f", {"2026f.1.0": ("2026f", b"newer-f", True)})
    code, text, data = run(root, out)
    check(code != 0 and data is None, "neither: exit != 0 and no file: %s" % code)
    check("pin icu-data's files of 2026f" in text and "tools/fetch-sources.py" in text and "user's OK" in text
          and "/var/db/timezone/tz/2026e" in text, "neither: the message names both ways out: %r" % text)

    # 5. The pinned folder has no Etc/UTC: it does not count. With no equal active release the tool stops and says why;
    # with one it falls back to /usr/share/zoneinfo.
    root = os.path.join(base, "r5")
    tz_root(root, "2026f", {"2026e.1.0": ("2026e", b"broken", False)})
    code, text, data = run(root, out)
    check(code != 0 and data is None and "no Etc/UTC" in text, "pinned folder without Etc/UTC: stops: %r" % text)
    root = os.path.join(base, "r5b")
    tz_root(root, "2026e", {"2026e.1.0": ("2026e", b"broken", False)})
    code, text, data = run(root, out)
    check(code == 0 and data is not None and b"active-2026e" in data and b"broken" not in data,
          "pinned folder without Etc/UTC, active equal: falls back: %s %r" % (code, text))

    # 6. A folder named for the pin whose +VERSION says another release does not count either.
    clean(out)
    root = os.path.join(base, "r6")
    tz_root(root, "2026f", {"2026e.1.0": ("2026d", b"mislabelled", True)})
    code, text, data = run(root, out)
    check(code != 0 and data is None, "a folder with another +VERSION does not count")

    # 7. The pin is read from the list: one release in icu_tzdata's files, else an error.
    with tempfile.TemporaryDirectory() as lists:
        path = os.path.join(lists, "s.json")
        sources(path, "2026e")
        check(make_tzdata.pinned_release(path) == "2026e", "pinned_release reads 2026e")
        with open(path, "w") as f:
            json.dump({"art": [{"name": "icu_tzdata", "files": [
                "tzdata/icunew/2026e/44/le/a.res", "tzdata/icunew/2026f/44/le/b.res"]}]}, f)
        try:
            make_tzdata.pinned_release(path)
            check(False, "two releases in the list: SystemExit")
        except SystemExit:
            pass
        with open(path, "w") as f:
            json.dump({"art": [{"name": "unrelated", "files": []}]}, f)
        try:
            make_tzdata.pinned_release(path)
            check(False, "no icu_tzdata entry: SystemExit")
        except SystemExit:
            pass
    check(re.fullmatch(r"\d{4}[a-z]", make_tzdata.pinned_release(SOURCES)) is not None,
          "the repository's list names one release")

    # 8. Without --pin-from the tool is what it was: the folder argument, and its +VERSION, as before.
    folder = os.path.join(base, "plain")
    zone_folder(folder, "2026c", b"plain-c")
    done = subprocess.run([sys.executable, TOOL, out, folder], capture_output=True, text=True)
    with open(out, "rb") as f:
        check(done.returncode == 0 and f.read(11) == b"tzdata2026c", "no pin: the folder argument as before")

print("make_tzdata_test:", "FAILED %d" % failures if failures else "ok")
sys.exit(1 if failures else 0)
