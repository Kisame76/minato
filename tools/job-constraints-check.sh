#!/usr/bin/env bash
# Runs the test of the jobs that lose a constraint while they run on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and job-constraints-test (seconds)
# every time, so it never tests an old binary; then starts the app's own Application through app_process on apk-run's
# fixed battery (100 %, charging), changes the host's battery in the steps of tools/android-reference.sh's
# job-constraints-test (unplugged, to 10 %, plugged in, to 50 %) through apk-run's lifecycle, and compares the app's log
# with Android 15's on apk-runner-35-bare (fixtures/expected/job-constraints-test.logcat, from
# tools/android-reference.sh --app job-constraints-test): the stop reasons of a job that needs the device to charge
# (6) and of ones that need a battery that is not low (5), the ones started again once the constraint holds, and the
# one that is not because its onStopJob returned false; and that a lost network (20 s) stops no job. The run takes
# about 60 s.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/job-constraints-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh job-constraints-test >/dev/null

lifecycle=start,network:3,pause:3000,battery:100:1,battery:10:1,battery:10:2,battery:50:2,pause:3000
lifecycle+=,network:0,pause:20000,network:3
python3 framework/tests/java_app_run.py --lifecycle "$lifecycle" job-constraints-test \
  io.github.kisame76.minato.test.jobconstraints \
  io.github.kisame76.minato.test.jobconstraints.ConstraintsApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/job-constraints-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/job-constraints-test/job-constraints-test.apk fixtures/expected/job-constraints-test.logcat 1290x2796@3,en-US
