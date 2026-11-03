#!/usr/bin/env bash
# M6 e0: where the start of a Java test app on the Mac goes. looper-test
# (the smallest app that boots the whole framework: ActivityThread, Looper) is started through
# framework/tests/java_app_run.py in four configurations of app_process's measurement switches, alternating, <runs>
# each. Not the run's wall time, which is mostly the harness (conversion of the guest libraries, waits after the
# app's last line), but two times from apk-run's start, from the lines' times (MINATO_LINE_TIMES): ART's first log
# line (the VM is up in the process) and the app's first line ("looper true", its code runs), and their medians are
# printed:
#   nterp    as an app runs: nterp, the interpreter M6 a made fast
#   switch   with -Xint (MINATO_MEASURE_ART_OPTIONS): ART forces the switch interpreter for every method
#   noverify nterp and -Xverify:none: what run-time verification of the boot classes costs
#   switch-noverify  -Xint and -Xverify:none
# MINATO_MEASURE_ART_OPTIONS is read by app_process for this measure only; no app's launch sets it, and -Xverify:none
# never becomes a default.
# The framework.jar of the run is a copy made at the start, so that another tool's build cannot remove it mid-run.
# BOOT_START_FRAMEWORK_JAR=<jar> takes that framework.jar and builds none (another tool's build may be failing).
# It stops unless the Mac is on AC power and waits, up to 30 minutes, for the load to fall under 10.
# Usage: tools/boot-start-measure.sh [<runs, default 5>]
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${BOOT_START_CAFFEINATED:-}" ]]; then
  BOOT_START_CAFFEINATED=1 exec caffeinate -i "$0" "$@"
fi
runs=${1:-5}
power() { pmset -g batt | head -1 | sed "s/Now drawing from //"; }
load() { uptime | sed "s/.*load averages*: //"; }
if [[ "$(power)" != *"AC Power"* ]]; then echo "stopped: the Mac is on $(power); plug it in"; exit 1; fi

NATIVE=build/native/macos-compiled
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
[[ -n "${BOOT_START_FRAMEWORK_JAR:-}" ]] || tools/build-framework.sh >/dev/null
if [[ -z "${BOOT_START_FRAMEWORK_JAR:-}" || ! -f build/looper-test/looper-test.apk ]]; then
  tools/build-java-test-app.sh looper-test >/dev/null
fi
work=build/boot-start-measure
rm -rf "$work"
mkdir -p "$work"
cp "${BOOT_START_FRAMEWORK_JAR:-build/framework/framework.jar}" "$work/framework.jar"

waited=0
while (($(load | cut -d. -f1) >= 10)); do
  if ((waited >= 1800)); then echo "stopped: load $(load) did not fall under 10 in 30 minutes"; exit 1; fi
  sleep 20
  waited=$((waited + 20))
done
echo "power at the start: $(power); load: $(load)"

run() { # run <configuration> <env>...
  local name=$1
  shift
  local env=()
  for entry in "$@"; do env+=(--env "$entry"); done
  rm -f "$work/times.txt"
  MINATO_LINE_TIMES="$PWD/$work/times.txt" python3 framework/tests/java_app_run.py ${env[@]+"${env[@]}"} \
    looper-test io.github.kisame76.minato.test.looper io.github.kisame76.minato.test.looper.LooperApplication \
    build/art build/bionic/lib build/bionic/system build/hwui/lib \
    build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" "$work/run-$name" build/framework-native/app_process64 "$work/framework.jar" \
    build/framework-res/framework-res build/looper-test/looper-test.apk fixtures/expected/looper-test.logcat \
    1290x2796@3,en-US >"$work/$name.log" 2>&1 || echo "$name: the run failed, see $work/$name.log"
  awk -F'\t' '/heap window holds/ && !art { art = $1 } /looper-test: looper true/ && !app { app = $1 }
    END { print art, app }' "$work/times.txt" >>"$work/$name.txt"
  rm -rf "$work/run-$name"
}
for ((n = 1; n <= runs; n++)); do
  run switch "MINATO_MEASURE_ART_OPTIONS=-Xint"
  run nterp
  run noverify "MINATO_MEASURE_ART_OPTIONS=-Xverify:none"
  run switch-noverify "MINATO_MEASURE_ART_OPTIONS=-Xint -Xverify:none"
done
python3 - "$work" <<'PY'
import statistics
import sys
for name in ("switch", "nterp", "noverify", "switch-noverify"):
    rows = [[float(value) for value in line.split()] for line in open(f"{sys.argv[1]}/{name}.txt")]
    art, app = [statistics.median(column) for column in zip(*rows)]
    spread = max(row[1] for row in rows) - min(row[1] for row in rows)
    print(f"{name:16} n={len(rows)} ART's first line at {art:.2f} s, the app's at {app:.2f} s "
          f"(ART to app {app - art:.2f} s; app time spread {spread:.2f})")
PY
echo "power at the end: $(power); load: $(load)"
