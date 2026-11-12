#!/usr/bin/env python3
# Tests tools/footprint_ratchet.py: it reads a session log's per-return lines (BackgroundTrace's "trip N back after
# ...: foot=.. tex=.. depth=.. before, foot=.. .. 10 s after") and the 1 Hz "I/apkrt : mem foot=.. gfx=.. other=.."
# lines, and FAILS (exit 1) when the footprint does not come back flat per return, when the per-return baseline climbs
# trip over trip (the ratchet), when the un-ledgered GPU remainder (other) climbs, or when textures/depth grow per
# trip. A flat run passes (exit 0). Runs on /usr/bin/python3 (3.9).
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "footprint_ratchet.py")
PY = sys.executable

FLAT = """\
I/minato   : trip 1 back after 15.0 s: foot=812 tex=5/2 depth=1/0 before, foot=820 tex=5/2 depth=1/0 10 s after
I/apkrt   : mem foot=820 int=100 comp=50 gfx=200 media=0 purg=0 purgc=0 other=20 ext=0
I/minato   : trip 2 back after 30.0 s: foot=815 tex=5/2 depth=1/0 before, foot=822 tex=5/2 depth=1/0 10 s after
I/apkrt   : mem foot=822 int=100 comp=50 gfx=202 media=0 purg=0 purgc=0 other=21 ext=0
I/minato   : trip 3 back after 15.0 s: foot=814 tex=5/2 depth=1/0 before, foot=821 tex=5/2 depth=1/0 10 s after
I/apkrt   : mem foot=821 int=100 comp=50 gfx=201 media=0 purg=0 purgc=0 other=20 ext=0
"""

# trip 2 does not reclaim on the return: foot grows 815 -> 900 (+85 > 32)
RETURN_GROWS = FLAT.replace(
    "trip 2 back after 30.0 s: foot=815 tex=5/2 depth=1/0 before, foot=822",
    "trip 2 back after 30.0 s: foot=815 tex=5/2 depth=1/0 before, foot=900")

# the per-return baseline climbs trip over trip: F0 812 -> 850 -> 900 (ratchet)
BASELINE_CLIMBS = (FLAT
    .replace("trip 2 back after 30.0 s: foot=815", "trip 2 back after 30.0 s: foot=850")
    .replace("trip 3 back after 15.0 s: foot=814", "trip 3 back after 15.0 s: foot=900"))

# the un-ledgered GPU remainder climbs: other 20 -> 60 -> 120 (trip 3's mem line has the unique foot=821)
OTHER_CLIMBS = (FLAT
    .replace("other=21 ext=0", "other=60 ext=0")                                   # trip 2: 20 -> 60
    .replace("foot=821 int=100 comp=50 gfx=201 media=0 purg=0 purgc=0 other=20",
             "foot=821 int=100 comp=50 gfx=201 media=0 purg=0 purgc=0 other=120"))  # trip 3: 20 -> 120

# textures left orphaned by the return: tex_after 5/2 -> 9/2 -> 15/2
TEX_GROWS = (FLAT
    .replace("foot=822 tex=5/2 depth=1/0 10 s after", "foot=822 tex=9/2 depth=1/0 10 s after")
    .replace("foot=821 tex=5/2 depth=1/0 10 s after", "foot=821 tex=15/2 depth=1/0 10 s after"))


def run(text):
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write(text); path = f.name
    try:
        p = subprocess.run([PY, TOOL, path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return p.returncode, p.stdout
    finally:
        os.unlink(path)


def main():
    ok = True
    code, out = run(FLAT)
    if code != 0:
        print("FAIL [flat]: expected exit 0 (ratchet held), got %d:\n%s" % (code, out)); ok = False
    for label, text, want in (("return-grows", RETURN_GROWS, "trip 2"),
                              ("baseline-climbs", BASELINE_CLIMBS, "climb"),
                              ("other-climbs", OTHER_CLIMBS, "other"),
                              ("tex-grows", TEX_GROWS, "tex")):
        code, out = run(text)
        if code == 0:
            print("FAIL [%s]: expected a ratchet breach (exit 1), got exit 0:\n%s" % (label, out)); ok = False
        if want.lower() not in out.lower():
            print("FAIL [%s]: expected %r in the breach report:\n%s" % (label, want, out)); ok = False
    print("PASS: footprint_ratchet flags each breach and passes the flat run" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
