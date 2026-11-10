#!/usr/bin/env bash
# Runs webview-late-test on the Mac with the host's WKWebView: builds what tools/webview-host-check.sh builds, with
# webview-late-test, and starts it through graphics-run, whose window host answers its WebView with graphics/host's
# WKWebView (webview.m); then compares the app's log with Android 15's on apk-runner-35
# (fixtures/expected/webview-late-test.logcat): a WebView made late, after what HTML5 app wrappers wait on first (a
# delay, an AsyncTask's copy of the page to the app's files, Safe Browsing, the cookies), and the copied page as a
# file:// URL, refused with file access off (net::ERR_ACCESS_DENIED) and run with it on.
# The run's home folder is the check's own (CFFIXED_USER_HOME), removed before and after.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/webview-late-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh webview-late-test >/dev/null

OUT=build/webview-late-check
rm -rf "$OUT/home"
mkdir -p "$OUT/home"
status=0
CFFIXED_USER_HOME="$PWD/$OUT/home" python3 framework/tests/java_app_run.py \
  webview-late-test io.github.kisame76.minato.test.webviewlate \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" "$OUT" build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/webview-late-test/webview-late-test.apk \
  fixtures/expected/webview-late-test.logcat 1290x2796@3,en-US || status=$?
rm -rf "$OUT/home"
exit $status
