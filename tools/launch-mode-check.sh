#!/usr/bin/env bash
# Runs ADR-0039 step b3.5b2-2b's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and launch-mode-test (seconds) every time, so it never tests an old binary; then starts launch-mode-test
# through app_process in a session on the M4 plan's display, as tools/activity-flow-check.sh does, and compares its
# log with Android 15's (fixtures/expected/launch-mode-test.logcat, from tools/android-reference.sh --app
# launch-mode-test): a singleTop activity that starts itself gets the intent, its result cancelled; CLEAR_TOP with
# SINGLE_TOP finishes the activities above the target, which gets the intent, and CLEAR_TOP alone a standard target
# too, which starts anew; REORDER_TO_FRONT brings an activity forward with the intent; a singleTask activity clears
# the activities above it; a noHistory activity goes once it stops; navigateUpTo finishes up to a standard parent,
# which starts anew; NEW_TASK with CLEAR_TASK finishes the task before the new root; each activity's lifecycle in
# Android's order.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/launch-mode-check.sh
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
tools/build-java-test-app.sh launch-mode-test >/dev/null

python3 framework/tests/java_app_run.py launch-mode-test io.github.kisame76.minato.test.launchmode \
  io.github.kisame76.minato.test.launchmode.ModeApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/launch-mode-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/launch-mode-test/launch-mode-test.apk \
  fixtures/expected/launch-mode-test.logcat 1290x2796@3,en-US
