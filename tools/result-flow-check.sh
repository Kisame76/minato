#!/usr/bin/env bash
# Runs ADR-0039 step b3.5b2-2a's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and result-flow-test (seconds) every time, so it never tests an old binary; then starts result-flow-test
# through app_process in a session on the M4 plan's display, as tools/activity-flow-check.sh does, and compares its
# log with Android 15's (fixtures/expected/result-flow-test.logcat, from tools/android-reference.sh --app
# result-flow-test): its activities return results to the ones that started them, also when one finishes in its
# onCreate, when FLAG_ACTIVITY_NEW_TASK cancels a result at once, when finishActivity ends one and when
# FLAG_ACTIVITY_FORWARD_RESULT hands a result on; finishAffinity ends the activities of one affinity, and
# finishAndRemoveTask from an activity that is not the task's root finishes only it, each activity's lifecycle in
# Android's order.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/result-flow-check.sh
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
tools/build-java-test-app.sh result-flow-test >/dev/null

python3 framework/tests/java_app_run.py result-flow-test io.github.kisame76.minato.test.resultflow \
  io.github.kisame76.minato.test.resultflow.ResultApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/result-flow-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/result-flow-test/result-flow-test.apk \
  fixtures/expected/result-flow-test.logcat 1290x2796@3,en-US
