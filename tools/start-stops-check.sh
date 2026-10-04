#!/usr/bin/env bash
# Runs ADR-0042 b's test of Godot's first three start stops past 7a on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and start-stops-test (seconds) every time, so it never tests an old binary;
# then starts start-stops-test through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's
# 1290 x 2796 px at scale 3 in en-US, and compares its log with Android 15's on that display with gesture navigation
# (fixtures/expected/start-stops-test.logcat, from tools/android-reference.sh --app start-stops-test): two Rationals,
# the window's insets controller before and after the attach, a platform fragment and an AutoTransition.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh and
# tools/build-front-libs.sh first.
# Usage: tools/start-stops-check.sh
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
tools/build-java-test-app.sh start-stops-test >/dev/null

python3 framework/tests/java_app_run.py start-stops-test io.github.kisame76.minato.test.startstops \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/start-stops-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/start-stops-test/start-stops-test.apk \
  fixtures/expected/start-stops-test.logcat 1290x2796@3,en-US
