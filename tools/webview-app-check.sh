#!/usr/bin/env bash
# Runs the WebView app fixtures of ADR-0056 on the Mac with the host's WKWebView, as tools/webview-bridge-check.sh
# runs its test: built with what tools/webview-host-check.sh builds and started through graphics-run, whose window
# host answers the WebViews with graphics/host's WKWebView (webview.m); then compares the app's log with Android 15's
# on apk-runner-35 (fixtures/expected/<app>.logcat). The apps: webview-localhost-test (a page the app serves itself
# over https from shouldInterceptRequest, its splash hidden through addJavascriptInterface),
# webview-prompt-bridge-test (an asset page that calls the app through prompt()) and webview-splash-test (a splash,
# then a WebView) and webview-redirect-test (redirects a page meets on the network, from the app's own loopback
# server). The WebView trace is on (MINATO_WEBVIEW_TRACE=1), as a check's guest log has it; its lines are
# not compared. The run's home folder is the check's own (CFFIXED_USER_HOME), removed before and after.
# webview-prompt-bridge-test's page sees location.protocol "minato-asset:" where Android's sees "file:" (WebKit takes
# no handler for file URLs: a named limit), so its expected line is Android's with that protocol.
# webview-localhost-test runs twice: as it is (http and https through the host's scheme handler, ADR-0056's note of
# 2026-11-11), and with MINATO_WEBVIEW_HTTPS=rewrite, as if WebKit refused that handler (fallback B), against
# Android's log with B's two named differences: the page's origin is minato-https://, and WebKit tells nothing of a
# subresource the network failed.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/webview-app-check.sh [app...]   (default: all three)
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $# -eq 0 ]]; then
  set -- webview-localhost-test webview-prompt-bridge-test webview-splash-test webview-redirect-test
fi

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

status=0
runs=()
for app in "$@"; do
  runs+=("$app")
  [[ "$app" != webview-localhost-test ]] || runs+=("$app:rewrite")
done
for run in "${runs[@]}"; do
  app=${run%:rewrite}
  mode=
  [[ "$run" == "$app" ]] || mode=rewrite
  case "$app" in
    webview-localhost-test) package=io.github.kisame76.minato.test.webviewlocalhost ;;
    webview-prompt-bridge-test) package=io.github.kisame76.minato.test.webviewprompt ;;
    webview-splash-test) package=io.github.kisame76.minato.test.webviewsplash ;;
    webview-redirect-test) package=io.github.kisame76.minato.test.webviewredirect ;;
    *) echo "webview-app-check: unknown app $app" >&2; exit 2 ;;
  esac
  tools/build-java-test-app.sh "$app" >/dev/null
  OUT=build/webview-app-check/$app${mode:+-$mode}
  rm -rf "$OUT/home"
  mkdir -p "$OUT/home"
  expected=fixtures/expected/$app.logcat
  if [[ "$app" == webview-prompt-bridge-test ]]; then
    expected=$OUT/expected.logcat
    sed 's/protocol file:/protocol minato-asset:/' "fixtures/expected/$app.logcat" > "$expected"
  elif [[ -n "$mode" ]]; then
    expected=$OUT/expected.logcat
    sed -e 's#origin https://#origin minato-https://#' -e 's#errors \[.*\]#errors []#' \
      "fixtures/expected/$app.logcat" > "$expected"
  fi
  echo "== $run"
  MINATO_WEBVIEW_HTTPS=$mode CFFIXED_USER_HOME="$PWD/$OUT/home" python3 framework/tests/java_app_run.py \
    --env MINATO_WEBVIEW_TRACE=1 \
    "$app" "$package" \
    android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
    build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/graphics/tests/graphics-run" "$OUT" build/framework-native/app_process64 \
    build/framework/framework.jar build/framework-res/framework-res "build/$app/$app.apk" \
    "$expected" 1290x2796@3,en-US || status=1
  rm -rf "$OUT/home"
done
exit $status
