#!/usr/bin/env bash
# Runs the test of toasts, toast-test, on the Mac (framework/tests/java_app_run.py; ADR-0041): builds
# graphics-run (apk-run with graphics/host's providers) and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and toast-test (seconds) every time,
# so it never tests an old binary; then starts toast-test through app_process in a session on the M4 plan's display and
# compares its log with Android 15's on apk-runner-35-iphone-gesture (fixtures/expected/toast-test.logcat, from
# tools/android-reference.sh --app toast-test): text toasts and their callbacks, how long they show, their queue, a
# cancel and the package's limit, a toast with a view of its own, and what a toast window without the system's token
# and a toast on a thread without a looper meet. The windows have graphics-run's layers in decision 1's order
# (MINATO_SESSION_LAYERS=1, MINATO_WINDOW_WATCH=hidden), whose lines it checks among the app's in order:
# each toast a window of type 2005, a text toast at the default gravity and offset at the display's bottom, one at a
# time, and the toast with a view of its own above a dialog shown after it, as toasts lie above every group.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/toast-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh toast-test >/dev/null

MINATO_SESSION_LAYERS=1 MINATO_WINDOW_WATCH=hidden python3 framework/tests/java_app_run.py --lifecycle start \
  --in-order "I/toast-test: text toast: view none" \
  --in-order "window host: window 1, type 2005: at 486, 2523, 317 x 129, alpha 1.00, dim 0.00, not opaque" \
  --in-order "window host: from the bottom: the window, window 1" \
  --in-order "window host: window 3 goes" --in-order "window host: window 4, type 2005" \
  --in-order "window host: from the bottom: the window, dim, window 12, window 11" \
  --in-order "window host: window 11 goes" \
  toast-test io.github.kisame76.minato.test.toast \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/toast-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/toast-test/toast-test.apk fixtures/expected/toast-test.logcat \
  1290x2796@3,en-US
