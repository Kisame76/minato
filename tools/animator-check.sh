#!/usr/bin/env bash
# Runs ADR-0041 a)'s animator test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and animator-test (seconds) every time, so it never tests an old binary; then starts animator-test through
# app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US, whose
# frame clock ticks Choreographer, and compares its log with Android 15's on that display
# (fixtures/expected/animator-test.logcat, from tools/android-reference.sh --app animator-test): interpolators,
# evaluators and play times without frames, setters found by name through PropertyValuesHolder's natives, animators,
# a LayoutTransition and a state list animator inflated from XML, then an AnimatorSet, a TimeAnimator and
# View.animate() on the frames.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/animator-check.sh
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
tools/build-java-test-app.sh animator-test >/dev/null

python3 framework/tests/java_app_run.py animator-test io.github.kisame76.minato.test.animator \
  io.github.kisame76.minato.test.animator.AnimatorApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/animator-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/animator-test/animator-test.apk fixtures/expected/animator-test.logcat \
  1290x2796@3,en-US
