#!/usr/bin/env bash
# Runs the test of a WebView that loads pages (ADR-0056 step b2, the ADR's test 1, its guest half) on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and webview-test (seconds) every
# time, so it never tests an old binary; then starts webview-test through app_process with apk-run's fake WebView host
# (--webview-fake), which plays the effects of the test's pages as Android 15's WebView shows them, and compares the
# app's log with Android 15's on apk-runner-35 (fixtures/expected/webview-test.logcat, from tools/android-reference.sh
# --app webview-test): the clients' callbacks of an asset page with an alert, two scripts, a missing asset and a page
# of 1.5 MiB from loadDataWithBaseURL, which reaches the host in parts, and the view's mirror and history.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/webview-check.sh
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
tools/build-java-test-app.sh webview-test >/dev/null

mkdir -p build/webview-check
python3 framework/tests/java_app_run.py --webview-fake \
  webview-test io.github.kisame76.minato.test.webview \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/webview-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/webview-test/webview-test.apk \
  fixtures/expected/webview-test.logcat 1290x2796@3,en-US
