#!/usr/bin/env bash
# Runs ADR-0039 step b3.5b2-1's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and activity-flow-test (seconds) every time, so it never tests an old binary; then starts activity-flow-test
# through app_process in a session on the M4 plan's display, as tools/activity-start-check.sh does, and compares its
# log with Android 15's (fixtures/expected/activity-flow-test.logcat, from tools/android-reference.sh --app
# activity-flow-test): the launcher starts Main and finishes, Main starts Second, which finishes, and the Application
# starts Third with FLAG_ACTIVITY_NEW_TASK into the app's one task, which finishes too, each activity's lifecycle in
# Android's order, with the task's root and the starts that Android refuses.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/activity-flow-check.sh
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
tools/build-java-test-app.sh activity-flow-test >/dev/null

python3 framework/tests/java_app_run.py activity-flow-test io.github.kisame76.minato.test.activityflow \
  io.github.kisame76.minato.test.activityflow.FlowApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/activity-flow-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/activity-flow-test/activity-flow-test.apk \
  fixtures/expected/activity-flow-test.logcat 1290x2796@3,en-US
