#!/usr/bin/env bash
# Runs the VpnService test on the Mac (group 1 of the framework breadth, framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and vpn-test (seconds) every time, so it never tests an old binary; then
# starts vpn-test through app_process, whose Application logs VpnService's shape, what prepare() asks for, and what its
# own VpnService does when it is started, bound and asked for a tunnel, in an app the user did not allow to be a VPN
# (the phone where the user said no to the consent dialog), and compares its log with Android 15's (fixtures/expected/
# vpn-test.logcat, from tools/android-reference.sh --app vpn-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/vpn-test-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=${NATIVE:-build/native/macos-app}
BIONIC=${BIONIC:-build/bionic}
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh vpn-test >/dev/null

python3 framework/tests/java_app_run.py vpn-test io.github.kisame76.minato.test.vpn \
  io.github.kisame76.minato.test.vpn.VpnApplication build/art "$BIONIC/lib" "$BIONIC/system" \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/vpn-test-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/vpn-test/vpn-test.apk \
  fixtures/expected/vpn-test.logcat 1290x2796@3,en-US
