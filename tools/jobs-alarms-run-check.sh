#!/usr/bin/env bash
# Runs the test of alarms and jobs that run, beyond run-test's on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and jobs-alarms-run-test (seconds)
# every time, so it never tests an old binary; then starts the app's own Application through app_process and compares
# its log with Android 15's on apk-runner-35-bare (fixtures/expected/jobs-alarms-run-test.logcat, from
# tools/android-reference.sh --app jobs-alarms-run-test): the exact alarms of an app with USE_EXACT_ALARM, an alarm
# clock, a window and a listener's alarm; an expedited job and its params, one rescheduled by its backoff, one
# cancelled while it runs, one with work items, and the charging, battery-not-low and storage-not-low constraints.
# The run takes about 30 s: the alarms are due at 6 to 9 s and the summary comes at 26 s.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/jobs-alarms-run-check.sh
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
tools/build-java-test-app.sh jobs-alarms-run-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start jobs-alarms-run-test \
  io.github.kisame76.minato.test.jobsalarms io.github.kisame76.minato.test.jobsalarms.JobsAlarmsApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/jobs-alarms-run-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/jobs-alarms-run-test/jobs-alarms-run-test.apk fixtures/expected/jobs-alarms-run-test.logcat 1290x2796@3,en-US
