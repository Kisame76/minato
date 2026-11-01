#!/usr/bin/env bash
# Runs the test of a WebView that loads pages on the Mac with the host's WKWebView (ADR-0056 step c1, the ADR's test
# 2): builds apk-run, apk-convert and graphics-run for macOS into build/native/macos-grafik, and app_process with
# libandroid_runtime.so (incremental), framework.jar and webview-test every time, so it never tests an old binary;
# then starts webview-test through graphics-run, whose window host answers the app's WebViews with graphics/host's
# WKWebView (webview.m), and compares the app's log with Android 15's on apk-runner-35
# (fixtures/expected/webview-test.logcat), the lines tools/webview-check.sh's fake host plays: an asset page served
# by the guest through the minato-asset scheme, its alert and title, two scripts, a missing asset and Chromium's error
# page, a page of 1.5 MiB from loadDataWithBaseURL, and the view's mirror and history. WebKit keeps the data store of
# graphics-run's process in the home folder; the run's is the check's own (CFFIXED_USER_HOME), removed before and
# after.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/webview-host-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh webview-test >/dev/null

OUT=build/webview-host-check
rm -rf "$OUT/home"
mkdir -p "$OUT/home"
status=0
CFFIXED_USER_HOME="$PWD/$OUT/home" python3 framework/tests/java_app_run.py \
  webview-test io.github.kisame76.minato.test.webview \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" "$OUT" build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/webview-test/webview-test.apk \
  fixtures/expected/webview-test.logcat 1290x2796@3,en-US || status=$?
rm -rf "$OUT/home"
exit $status
