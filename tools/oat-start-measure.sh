#!/usr/bin/env bash
# M6 c1's start-time measure (ADR-0053): godot-bench-gles through the Java path on the Mac
# (framework/tests/godot_java_run.py), whose 5.1 MB of dex is the largest in the repo's apps, N times with and N times
# without the verify odex our dex2oat makes for it at its code path, alternating (without, with, without, ...) so that
# drift cancels. Prints the odex's and vdex's sizes, dex2oat's time, and each series' median, minimum, maximum and
# spread (maximum minus minimum) of the run's wall time, with the power state and the load at the start and the end.
# It stops, with a message, unless the Mac is on AC power and its load is under 10, and runs under caffeinate -i, as a
# sleeping Mac makes the numbers meaningless. The wall time includes the bench's own run
# (the same in both series), so the difference is what the odex saves of the start.
# Needs what tools/godot-java-check.sh needs.
# Usage: tools/oat-start-measure.sh [<runs per series, default 10>]
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${OAT_MEASURE_CAFFEINATED:-}" ]]; then
  OAT_MEASURE_CAFFEINATED=1 exec caffeinate -i "$0" "$@"
fi
runs=${1:-10}
power() { pmset -g batt | head -1 | sed "s/Now drawing from //"; }
load() { uptime | sed "s/.*load averages*: //"; }
if [[ "$(power)" != *"AC Power"* ]]; then echo "stopped: the Mac is on $(power); plug it in"; exit 1; fi
if (($(load | cut -d. -f1) >= 10)); then echo "stopped: load $(load) is not under 10"; exit 1; fi

ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi
MOLTENVK="$PWD/build/moltenvk/macos/libMoltenVK.dylib"
NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert apk-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
app=godot-bench-gles
package=io.github.kisame76.minato.godotbench.gles
apk=build/godot/$app.apk
work=build/oat-start-measure
rm -rf "$work"
mkdir -p "$work"
location=$(python3 -c "
import sys
sys.path.insert(0, 'framework/tests')
from app_process_run import code_path
print(code_path('$apk', '$package') + '/base.apk')")
echo "power at the start: $(power); load: $(load)"
# The wall clock, as two Python processes take the times: time.monotonic() starts near 0 in each on macOS.
began=$(python3 -c "import time; print(time.time())")
python3 runtime/tests/dex2oat_make.py build/art build/bionic/lib build/bionic/system \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" "$work/dex2oat" "$apk" "$location" "$work/odex"
echo "dex2oat: $(python3 -c "import time; print(f'{time.time() - $began:.1f}')") s;" \
  "odex $(stat -f %z "$work/odex.odex") bytes, vdex $(stat -f %z "$work/odex.vdex") bytes"

series() { # series <with|without> <n>
  local odex=()
  [[ "$1" == without ]] || odex=(--odex "$work/odex.odex")
  MINATO_ANGLE="$ANGLE" MINATO_MOLTENVK="$MOLTENVK" MINATO_SESSION_LAYERS=1 python3 framework/tests/godot_java_run.py \
    ${odex[@]+"${odex[@]}"} "$app" "$package" build/art build/bionic/lib build/bionic/system build/hwui/lib \
    build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/graphics/tests/graphics-run" "$work/run-$1" build/framework-native/app_process64 \
    build/framework/framework.jar build/framework-res/framework-res "$apk" "fixtures/expected/$app.logcat" \
    | sed -n "s/^wall: \(.*\) s$/\1/p" >>"$work/$1.txt"
}
for ((n = 1; n <= runs; n++)); do
  series without "$n"
  series with "$n"
done
python3 - "$work" <<'PY'
import statistics
import sys
for name in ("without", "with"):
    values = [float(line) for line in open(f"{sys.argv[1]}/{name}.txt")]
    print(f"{name:8} n={len(values)} median {statistics.median(values):.3f} s, min {min(values):.3f}, "
          f"max {max(values):.3f}, spread {max(values) - min(values):.3f}")
PY
echo "power at the end: $(power); load: $(load)"
