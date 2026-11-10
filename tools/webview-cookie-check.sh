#!/usr/bin/env bash
# Runs ADR-0056 c3's cookie test on the Mac with the host's WKWebView: builds what tools/webview-host-check.sh builds,
# with webview-cookie-test, and starts it through graphics-run, whose window host answers its WebViews and its
# CookieManager's channel with graphics/host's WKWebView and the package's WKWebsiteDataStore (webview.m); then
# compares the app's log with Android 15's on apk-runner-35 (fixtures/expected/webview-cookie-test.logcat):
# CookieManager before any WebView, a page at an https base whose script sets cookies, an app's setCookie that the page
# reads, and removeAllCookies. WebKit keeps graphics-run's data store in the home folder; the run's is the check's own
# (CFFIXED_USER_HOME), removed before and after.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/webview-cookie-check.sh
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
tools/build-java-test-app.sh webview-cookie-test >/dev/null

OUT=build/webview-cookie-check
rm -rf "$OUT/home"
mkdir -p "$OUT/home"
status=0
CFFIXED_USER_HOME="$PWD/$OUT/home" python3 framework/tests/java_app_run.py \
  webview-cookie-test io.github.kisame76.minato.test.webviewcookie \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" "$OUT" build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/webview-cookie-test/webview-cookie-test.apk \
  fixtures/expected/webview-cookie-test.logcat 1290x2796@3,en-US || status=$?
rm -rf "$OUT/home"
exit $status
