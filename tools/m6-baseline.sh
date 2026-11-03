#!/usr/bin/env bash
# M6's baseline measurements on the Mac, so that every
# step that changes how ART runs Java (nterp, compiled code) is compared with the same measures:
#   1. the run-tests sample (runtime/tests/art_run_tests.py --sample): the sum of the tests' own seconds;
#   2. java-test's start: apk-run's spawn to the app's first onResume, without a session
#      (java_app_run.py --until onResume, line times from MINATO_LINE_TIMES);
#   3. frame-bench's frame times: 240 frames of a view tree whose texts change and relayout every frame, and 240 of the
#      same tree under animators; a frame's time is Choreographer's frame time to its commit callback, after the
#      traversal (p50, p99 and the largest of each phase). No other test measures frames: view-tree-test and
#      animator-test compare logs.
# Builds apk-run and apk-convert for macOS into build/native/macos-app, app_process with libandroid_runtime.so,
# framework.jar and the apps every time, so it never measures an old binary, then runs each measure RUNS times (5 by
# default) and prints the numbers, which are also kept in build/m6-baseline/. They depend on the machine's load: run it
# while no other build or check runs, and read the load average printed with each run. The first run of the sample
# builds its jars, and the first start of java-test is cold; both are named in the output. Needs tools/build-art.sh,
# tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/m6-baseline.sh (RUNS=3 tools/m6-baseline.sh for fewer runs)
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this script does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

RUNS=${RUNS:-5}
NATIVE=build/native/macos-app
OUT=build/m6-baseline
rm -rf "$OUT"
mkdir -p "$OUT"

cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh java-test >/dev/null
tools/build-java-test-app.sh frame-bench >/dev/null

load() { uptime | sed 's/.*load averages*: //'; }
ICU=build/sources/icu/icu4c/source/stubdata/icudt75l.dat
COMMON=(build/art build/bionic/lib build/bionic/system build/hwui/lib "$ICU" "$NATIVE/importer/converter/apk-convert"
  "$NATIVE/runtime/apk-run")

echo "== 1. run-tests sample, the sum of the tests' seconds"
for run in $(seq "$RUNS"); do
  python3 runtime/tests/art_run_tests.py build/art build/bionic/lib build/bionic/system "$ICU" \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "$OUT/run-tests-work" . --sample \
    --times "$OUT/run-tests-$run.times" > "$OUT/run-tests-$run.out" 2>&1 \
    || { echo "run $run failed"; tail -5 "$OUT/run-tests-$run.out"; exit 1; }
  python3 - "$OUT/run-tests-$run.times" "$run" "$(load)" <<'PY'
import sys
times = [float(line.split()[-1]) for line in open(sys.argv[1]) if line.strip()]
print(f"run {sys.argv[2]}: {len(times)} tests, {sum(times):.2f} s, slowest {max(times):.2f} s, load {sys.argv[3]}"
      + (" (builds the jars)" if sys.argv[2] == "1" else ""))
PY
done

echo "== 2. java-test's start, spawn to the first onResume"
for run in $(seq "$RUNS"); do
  MINATO_LINE_TIMES="$OUT/java-test-$run.times" python3 framework/tests/java_app_run.py --until onResume java-test \
    io.github.kisame76.minato.test.javatest android.app.Application "${COMMON[@]}" "$OUT/java-test-work" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    build/java-test/java-test.apk fixtures/expected/java-test.logcat > "$OUT/java-test-$run.out" 2>&1 \
    || { echo "run $run failed"; tail -5 "$OUT/java-test-$run.out"; exit 1; }
  at=$(grep -m1 'java-test-device: onResume at' "$OUT/java-test-$run.times" | cut -f1)
  echo "run $run: ${at:?no onResume line} s, load $(load)$([[ $run == 1 ]] && echo ' (cold)')"
done

echo "== 3. frame-bench, a frame's time from Choreographer's frame time to its commit"
for run in $(seq "$RUNS"); do
  MINATO_LINE_TIMES="$OUT/frame-bench-$run.times" python3 framework/tests/java_app_run.py --lifecycle start \
    frame-bench \
    io.github.kisame76.minato.test.framebench android.app.Application "${COMMON[@]}" "$OUT/frame-bench-work" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    build/frame-bench/frame-bench.apk fixtures/expected/frame-bench.logcat 1290x2796@3,en-US \
    > "$OUT/frame-bench-$run.out" 2>&1 || { echo "run $run failed"; tail -5 "$OUT/frame-bench-$run.out"; exit 1; }
  grep 'frame-bench-device' "$OUT/frame-bench-$run.times" | cut -f3 | sed -e "s/^I\/frame-bench-device: /run $run: /" \
    -e "s/\$/ (load $(load))/"
done
