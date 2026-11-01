#!/usr/bin/env bash
# M6 c3: our dex2oat's compiled code (--compiler-filter=speed) against the interpreter, on the Mac, for our own test
# programs on ART (runtime/tests/art_run.py; never a user's APK). For art-hello, nterp-test and codegen-test it checks
# that the compiled run prints what the interpreter prints and that ART's own -verbose:oat log says it opened our odex
# (best oat file, executable, up to date with the filter speed), so a speedup stands on that. For compute-bench
# (integer loops, matrix work, calls, strings, collections, allocation; no timing in its output) it then times the
# guest alone, nterp and compiled alternating, <runs> each, and prints the median, minimum and maximum of each.
# It builds apk-run and apk-convert into its own build directory and the test jars into its work folder, so no other
# tool's build can pull a file from under a run. It waits, up to 30 minutes, for the load to fall under 10, and stops
# unless the Mac is on AC power; it runs under caffeinate -i.
# Needs tools/build-art.sh, tools/build-bionic.sh and tools/fetch-sources.py --set art.
# Usage: tools/compiled-apps-check.sh [<runs, default 10>]
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${COMPILED_APPS_CAFFEINATED:-}" ]]; then
  COMPILED_APPS_CAFFEINATED=1 exec caffeinate -i "$0" "$@"
fi
runs=${1:-10}
power() { pmset -g batt | head -1 | sed "s/Now drawing from //"; }
load() { uptime | sed "s/.*load averages*: //"; }
if [[ "$(power)" != *"AC Power"* ]]; then echo "stopped: the Mac is on $(power); plug it in"; exit 1; fi

NATIVE=build/native/macos-compiled
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
JDK=$(ls -d build/tools/jdk-17/*/Contents/Home | head -1)
work=build/compiled-apps-check
rm -rf "$work"
mkdir -p "$work"
for program in art-hello nterp-test codegen-test compute-bench; do
  python3 runtime/art/test_jars.py "$JDK" build/sources/r8/r8.jar build/art/java "$work/$program.jar" \
    "fixtures/java/$program/Main.java"
done

run() { # run <program> <options>...: art_run.py's lines
  local program=$1
  shift
  python3 runtime/tests/art_run.py build/art build/bionic/lib build/bionic/system \
    build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" "$work/art-runs" "$work/$program.jar" Main "fixtures/expected/$program.out" "$@"
}

failed=0
for program in art-hello nterp-test codegen-test compute-bench; do
  echo "== $program compiled, with the oat proof"
  run "$program" --compiled --oat-proof | tee "$work/proof.txt" || failed=1
  grep -q "printed what Android 15 prints" "$work/proof.txt" || failed=1
done

waited=0
while (($(load | cut -d. -f1) >= 10)); do
  if ((waited >= 1800)); then echo "stopped: load $(load) did not fall under 10 in 30 minutes"; exit 1; fi
  sleep 20
  waited=$((waited + 20))
done
echo "power at the start: $(power); load: $(load)"
for ((n = 1; n <= runs; n++)); do
  run compute-bench --nterp --time | sed -n "s/^guest time: \(.*\) s$/\1/p" >>"$work/nterp.txt"
  run compute-bench --compiled --time | sed -n "s/^guest time: \(.*\) s$/\1/p" >>"$work/compiled.txt"
done
python3 - "$work" <<'PY'
import statistics
import sys
medians = {}
for name in ("nterp", "compiled"):
    values = [float(line) for line in open(f"{sys.argv[1]}/{name}.txt")]
    medians[name] = statistics.median(values)
    print(f"{name:9} n={len(values)} median {medians[name]:.3f} s, min {min(values):.3f}, max {max(values):.3f}")
print(f"compiled / nterp: {medians['compiled'] / medians['nterp']:.2f}")
PY
echo "power at the end: $(power); load: $(load)"
exit "$failed"
