#!/usr/bin/env bash
# Runs ADR-0039 step c2c3's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS
# into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s)
# and activity-start-test (seconds) every time, so it never tests an old binary; then starts activity-start-test
# through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in
# en-US, whose frame clock ticks Choreographer, and compares its log with Android 15's on that display without
# navigation keys (fixtures/expected/activity-start-test.logcat, from tools/android-reference.sh --app
# activity-start-test): the launcher activity behind its alias, through onCreate, the window configuration,
# setContentView in the window's layout, onStart to onResume and the top resumed state, then the window's first
# traversal (attach, layout, global layout), a View.post and the window's focus.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/activity-start-check.sh
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
tools/build-java-test-app.sh activity-start-test >/dev/null

python3 framework/tests/java_app_run.py activity-start-test io.github.kisame76.minato.test.activitystart \
  io.github.kisame76.minato.test.activitystart.StartApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/activity-start-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/activity-start-test/activity-start-test.apk \
  fixtures/expected/activity-start-test.logcat 1290x2796@3,en-US
