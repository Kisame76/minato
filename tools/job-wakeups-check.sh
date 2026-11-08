#!/usr/bin/env bash
# Measures what the job scheduler costs while it only waits (the event-driven runner): runs job-wakeups-test on the Mac
# (framework/tests/java_app_run.py): one job that is not due for an hour is pending, the app is idle for 60 s, and the
# test's device line says how many times the scheduler's runner woke up in that time. The other lines are compared with
# fixtures/expected/job-wakeups-test.logcat (the same on Android, which has no counter). It builds apk-run and
# apk-convert into build/native/macos-app, app_process, framework.jar and the test every time. The run takes about 70 s.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/job-wakeups-check.sh
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
tools/build-java-test-app.sh job-wakeups-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start job-wakeups-test \
  io.github.kisame76.minato.test.jobwakeups io.github.kisame76.minato.test.jobwakeups.WakeupsApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/job-wakeups-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/job-wakeups-test/job-wakeups-test.apk fixtures/expected/job-wakeups-test.logcat 1290x2796@3,en-US
