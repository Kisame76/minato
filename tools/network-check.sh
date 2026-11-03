#!/usr/bin/env bash
# Runs network-test on the Mac (ADR-0054 step d)): builds what tools/webview-host-check.sh builds, with network-test,
# and starts it through graphics-run, whose graphics host reports the Mac's network from Network.framework's path
# monitor (graphics/host/network.m); then compares the app's log with Android 15's on apk-runner-35 with its mobile
# data off (fixtures/expected/network-test.logcat): the active network, its NetworkInfo and capabilities, and the
# default network callback's first calls. The Mac must be online over Wi-Fi or Ethernet.
# The run's home folder is the check's own (CFFIXED_USER_HOME), removed before and after.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/network-check.sh
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
tools/build-java-test-app.sh network-test >/dev/null

OUT=build/network-check
rm -rf "$OUT/home"
mkdir -p "$OUT/home"
status=0
CFFIXED_USER_HOME="$PWD/$OUT/home" python3 framework/tests/java_app_run.py \
  network-test io.github.kisame76.minato.test.network \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" "$OUT" build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/network-test/network-test.apk \
  fixtures/expected/network-test.logcat 1290x2796@3,en-US || status=$?
rm -rf "$OUT/home"
exit $status
