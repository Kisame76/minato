#!/usr/bin/env bash
# Runs the test of android.webkit's classes and the legacy android.hardware.Camera on the Mac
# (framework/tests/java_app_run.py; the Unity test APK probe's batch C): builds apk-run and apk-convert for macOS into
# build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# webview-camera-test (seconds) every time, so it never tests an old binary; then starts webview-camera-test through
# app_process and compares its log with Android 15's on apk-runner-35-bare, a device without cameras
# (fixtures/expected/webview-camera-test.logcat, from tools/android-reference.sh --app webview-camera-test):
# a WebViewClient's defaults, WebResourceResponse with the arguments it refuses, a WebResourceRequest, WebSettings'
# enums, the WebView class, the camera count, Camera.open() and Camera.Area. The lines that depend on
# the device's WebView (webview-camera-test-device) are shown, and must be what Android 15 answers without the
# webview feature, which Minato does not report (WebViewFactory.getProvider at android-15.0.0_r1): no feature, an
# UnsupportedOperationException without a message for a new WebView, CookieManager and the default user agent, and no
# provider package; and RenderScript.create and RenderScriptGL's constructor (also through RSSurfaceView, which
# Android's hidden API policy blocks and Minato has none of), which the AVD runs on its drivers, must throw Minato's
# RSRuntimeException, which the blur's fallback catches (a named departure, framework/patches).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/webview-camera-check.sh
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
tools/build-java-test-app.sh webview-camera-test >/dev/null

mkdir -p build/webview-camera-check
python3 framework/tests/java_app_run.py webview-camera-test io.github.kisame76.minato.test.webviewcamera \
  io.github.kisame76.minato.test.webviewcamera.WebViewCameraApplication build/art build/bionic/lib \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/webview-camera-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/webview-camera-test/webview-camera-test.apk fixtures/expected/webview-camera-test.logcat \
  | tee build/webview-camera-check/run.txt
missing=0
unavailable="android.renderscript.RSRuntimeException: RenderScript is not available on this device"
for line in "the webview feature: false" "new WebView: java.lang.UnsupportedOperationException: null" \
    "CookieManager.getInstance: java.lang.UnsupportedOperationException: null" \
    "WebSettings.getDefaultUserAgent: java.lang.UnsupportedOperationException: null" \
    "WebView.getCurrentWebViewPackage: null" \
    "RenderScript.create: android.renderscript.RSRuntimeException: RenderScript is not available on this device" \
    "RenderScript blur: RSRuntimeException, the app's fallback" \
    "new RenderScriptGL: android.renderscript.RSRuntimeException: RenderScript is not available on this device" \
    "RSSurfaceView.createRenderScriptGL: $unavailable"; do
  if ! grep -qxF "  device: I/webview-camera-test-device: $line" build/webview-camera-check/run.txt; then
    echo "FAIL: no device line \"$line\" (Android 15 without the webview feature, Minato without RenderScript)"
    missing=1
  fi
done
exit "$missing"
