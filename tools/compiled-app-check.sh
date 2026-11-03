#!/usr/bin/env bash
# M6 c end to end: an app through the framework's start (framework/tests/java_app_run.py, which mounts the app's oat
# as GuestStart does) runs compiled code. compute-app-test (compute-bench's loops in its Application) is compiled with
# our dex2oat for the app's boot class path and class loader context (runtime/tests/dex2oat_make.py --framework), then
# started <runs> times without and with the odex, alternating. Both must log what Android 15 logs
# (fixtures/expected/compute-app-test.logcat); ART's -verbose:oat log must say the odex is the best oat file and runs
# executable (the proof that the speedup is compiled code); the compute time (the sieve line to done, the harness's
# MINATO_LINE_TIMES) of each series is printed with its median, minimum and maximum, and the ratio of the medians.
# Waits, up to 30 minutes, for the load to fall under 10 and stops unless the Mac is on AC power; runs under caffeinate.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-framework.sh and tools/build-framework-native.sh first.
# Usage: tools/compiled-app-check.sh [<runs, default 5>]
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${COMPILED_APP_CAFFEINATED:-}" ]]; then
  COMPILED_APP_CAFFEINATED=1 exec caffeinate -i "$0" "$@"
fi
runs=${1:-5}
power() { pmset -g batt | head -1 | sed "s/Now drawing from //"; }
load() { uptime | sed "s/.*load averages*: //"; }
if [[ "$(power)" != *"AC Power"* ]]; then echo "stopped: the Mac is on $(power); plug it in"; exit 1; fi
for _ in $(seq 1 60); do
  first=$(load | cut -d' ' -f1 | cut -d. -f1)
  [[ "$first" -lt 10 ]] && break
  sleep 30
done
[[ "$first" -lt 10 ]] || { echo "stopped: the load stays at $(load)"; exit 1; }

NATIVE=build/native/macos-compiled-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
app=compute-app-test
package=io.github.kisame76.minato.test.computeapp
tools/build-java-test-app.sh "$app" >/dev/null
apk=build/$app/$app.apk
work=build/compiled-app-check
rm -rf "$work"
mkdir -p "$work"
location=$(python3 -c "
import sys
sys.path.insert(0, 'framework/tests')
from app_process_run import code_path
print(code_path('$apk', '$package') + '/base.apk')")
python3 runtime/tests/dex2oat_make.py build/art build/bionic/lib build/bionic/system \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" "$work/dex2oat" "$apk" "$location" "$work/compiled" --filter speed \
  --framework build/framework/framework.jar

run() { # run <series>: the app once; prints the compute seconds
  local series=$1 options=()
  [[ "$series" == plain ]] || options=(--env MINATO_MEASURE_ART_OPTIONS=-verbose:oat --odex "$work/compiled.odex")
  rm -f "$work/lines-$series.txt"
  MINATO_LINE_TIMES="$PWD/$work/lines-$series.txt" python3 framework/tests/java_app_run.py \
    ${options[@]+"${options[@]}"} \
    "$app" "$package" "$package.ComputeApplication" build/art build/bionic/lib build/bionic/system build/hwui/lib \
    build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" "$work/run" build/framework-native/app_process64 build/framework/framework.jar \
    build/framework-res/framework-res "$apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US \
    >"$work/$series.log" 2>&1 || { echo "$series: the run failed, see $work/$series.log" >&2; return 1; }
  if [[ "$series" == compiled ]] && ! grep -q "/oat/arm64/base.odex (executable=1)" "$work/lines-$series.txt"; then
    echo "compiled: ART does not run the odex" >&2
    return 1
  fi
  awk -F'\t' '/compute-app-test: sieve/ && !a { a = $1 } /compute-app-test: done/ { b = $1 } END { print b - a }' \
    "$work/lines-$series.txt"
}

: >"$work/plain.times"
: >"$work/compiled.times"
for _ in $(seq 1 "$runs"); do
  for series in plain compiled; do
    run "$series" >>"$work/$series.times"
  done
done
stats() { # stats <series>: "<median> <min> <max> <count>" of its compute seconds
  sort -n "$work/$1.times" | awk '{ v[NR] = $1 }
    END { m = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2; print m, v[1], v[NR], NR }'
}
read -r pm pmin pmax pn < <(stats plain)
read -r cm cmin cmax cn < <(stats compiled)
echo "plain (nterp): median $pm s, min $pmin s, max $pmax s ($pn runs)"
echo "compiled: median $cm s, min $cmin s, max $cmax s ($cn runs)"
awk -v p="$pm" -v c="$cm" 'BEGIN { printf "compiled/plain: %.2f\n", c / p }'
rm -rf "$work/run"
