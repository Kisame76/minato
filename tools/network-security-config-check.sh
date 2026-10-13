#!/usr/bin/env bash
# Runs the test of an app's network_security_config.xml, network-security-config-test, on the Mac
# (framework/tests/java_app_run.py; framework breadth, step (d)): builds apk-run and apk-convert for macOS into
# build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# network-security-config-test (seconds) every time, so it never tests an old binary; then starts it through app_process
# and compares its log with Android 15's on apk-runner-35 (fixtures/expected/network-security-config-test.logcat, from
# tools/android-reference.sh --app network-security-config-test): the policy per host for a config with a base config,
# domain configs, a pin-set and debug-overrides, and the trust manager's verdict on a public root for pinned hosts.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/network-security-config-check.sh
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
tools/build-java-test-app.sh network-security-config-test >/dev/null

python3 framework/tests/java_app_run.py network-security-config-test io.github.kisame76.minato.test.networksecurityconfig \
  io.github.kisame76.minato.test.networksecurityconfig.NetworkSecurityConfigApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/network-security-config-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/network-security-config-test/network-security-config-test.apk fixtures/expected/network-security-config-test.logcat
