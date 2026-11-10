#!/usr/bin/env bash
# Runs lan-test on the Mac (framework/tests/java_app_run.py): an app talking to another device on the same network, with
# tools/lan-server.py on the Mac's loopback standing in for it: a TCP echo, an HTTP request, a UDP echo, and the
# connection to an address nobody answers as the control. It builds apk-run and apk-convert for macOS into
# build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar and lan-test every
# time, so it never tests an old binary; then starts lan-test through app_process, with the server's address in
# MINATO_LAN_HOST, and compares its log with Android 15's on apk-runner-35 (fixtures/expected/lan-test.logcat, from
# tools/android-reference.sh --app lan-test, which reaches the server at the emulator's 10.0.2.2).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/lan-check.sh
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
tools/build-java-test-app.sh lan-test >/dev/null

python3 tools/lan-server.py --seconds 120 >/dev/null &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
sleep 1

python3 framework/tests/java_app_run.py --env MINATO_LAN_HOST=127.0.0.1 lan-test io.github.kisame76.minato.test.lan \
  io.github.kisame76.minato.test.lan.LanApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/lan-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/lan-test/lan-test.apk fixtures/expected/lan-test.logcat
