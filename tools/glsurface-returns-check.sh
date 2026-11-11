#!/usr/bin/env bash
# The returns check on the Mac (framework/tests/java_app_run.py): glsurface-returns-test's GLSurfaceView with a depth
# buffer, which draws continuously, through four rounds of the background and the return, each of which gives it a new
# surface. Builds graphics-run (apk-run with graphics/host's providers) and apk-convert for macOS into
# build/native/macos-checks, and app_process with libandroid_runtime.so (incremental), framework.jar and the app every
# time, so it never tests an old binary; then starts it on the Mac's ANGLE in a session on the M4 plan's display, whose
# host plays the start and the four rounds, and compares its log with Android 15's
# (fixtures/expected/glsurface-returns-test.logcat, from tools/android-reference.sh --app glsurface-returns-test).
# With MINATO_LIVE_LAYERS=1 the session's layers (graphics/tests/session_layers.m) print once a second the surfaces in
# the tree, the surface layers made that are still alive and the Metal device's allocated MiB. Before each of the four
# backgrounds (after the start's surface and the first three returns'; the fifth surface's app finishes at once), the
# last such line must say 1 surface and 1 layer alive, and the device's MiB at most RETURNS_GROWTH (32) above the
# first's: a window that went took its layer and its drawables with it.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh and tools/build-angle.sh first.
# Usage: tools/glsurface-returns-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh glsurface-returns-test >/dev/null

WORK=build/glsurface-returns-check
mkdir -p "$WORK"
# Each round: 2 s more in the foreground once the step before is answered, so the new surface draws its 30 frames
# and its first frames after the return commit what the background let go.
round=pause:2000,background,foreground
MINATO_ANGLE="$ANGLE" MINATO_SESSION_LAYERS=1 MINATO_LIVE_LAYERS=1 python3 framework/tests/java_app_run.py \
  --lifecycle "start,$round,$round,$round,$round" --log "$WORK/process.log" \
  glsurface-returns-test io.github.kisame76.minato.test.glsurfacereturns \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" "$WORK" build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/glsurface-returns-test/glsurface-returns-test.apk fixtures/expected/glsurface-returns-test.logcat \
  1290x2796@3,en-US

python3 - "${RETURNS_GROWTH:-32}" "$WORK/process.log" <<'EOF'
import re
import sys

growth, log = float(sys.argv[1]), open(sys.argv[2], encoding="utf-8", errors="replace").read().splitlines()
live = re.compile(r"window host: live surfaces (\d+), layers alive (\d+) of (\d+), gpu ([-0-9.]+)$")
# The last live line before each background's onPause after a surface's 30 frames, up to the app's "done".
readings, last, surface = [], None, 0
for line in log:
    match = live.search(line)
    drawn = re.search(r"glsurface-returns-test: surface (\d+): 30 frames$", line)
    if match:
        last = match
    elif drawn:
        surface, last = int(drawn[1]), None
    elif re.search(r"glsurface-returns-test: done$", line):
        break
    elif surface and re.search(r"glsurface-returns-test: onPause$", line):
        readings.append((surface, last))
        surface = 0
for number, match in readings:
    print(f"      after surface {number}: " + (match.group(0) if match else "no live line"))
bad = [n for n, m in readings if not m or (m[1], m[2]) != ("1", "1")]
if len(readings) != 4 or bad:
    print(f"FAIL: the session holds more than one surface and layer after surfaces {bad or 'missing'}")
    sys.exit(1)
grew = max(float(m[4]) for _, m in readings[1:]) - float(readings[0][1][4])
if grew > growth:
    print(f"FAIL: the Metal device's bytes grew {grew:.1f} MiB over three returns (more than {growth:.0f})")
    sys.exit(1)
print(f"glsurface returns check: fine (one surface and one layer alive after each return, the device's bytes "
      f"{grew:+.1f} MiB)")
EOF
