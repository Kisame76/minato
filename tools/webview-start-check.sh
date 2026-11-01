#!/usr/bin/env bash
# Runs the test of a WebView that starts (ADR-0056 step b1) on the Mac (framework/tests/java_app_run.py): builds apk-run
# and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and webview-start-test (seconds) every time, so it never tests an old binary; then starts
# webview-start-test through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/webview-start-test.logcat, from tools/android-reference.sh --app webview-start-test): the webview
# feature, a WebView made as the activity's content, its settings' defaults, its user agent's shape and Chrome version,
# the WebView package through WebView and PackageManager, the cookie store, the view's getters, and how it draws in
# software before a load (black) and after one (white). Of the device's lines (webview-start-test-device) the package's
# name must be a phone's, com.google.android.webview, where the emulator's AOSP image has com.android.webview. The
# provider's note that web content is not available yet must be in the process's log once, for a load and a script,
# and no page data with it.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/webview-start-check.sh
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
tools/build-java-test-app.sh webview-start-test >/dev/null

mkdir -p build/webview-start-check
python3 framework/tests/java_app_run.py \
  --once "W/MinatoWebView: web content not available yet: the WebView stays empty (ADR-0056 step c)" \
  --never "<html" \
  webview-start-test io.github.kisame76.minato.test.webviewstart \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/webview-start-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/webview-start-test/webview-start-test.apk \
  fixtures/expected/webview-start-test.logcat 1290x2796@3,en-US | tee build/webview-start-check/run.txt
line="WebView package: com.google.android.webview"
if ! grep -qxF "  device: I/webview-start-test-device: $line" build/webview-start-check/run.txt; then
  echo "FAIL: no device line \"$line\" (a phone's WebView package)"
  exit 1
fi
