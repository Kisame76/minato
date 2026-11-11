#!/usr/bin/env bash
# Runs the network value types' test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and net-types-test (seconds) every time, so it never tests an old binary; then starts net-types-test through
# app_process, whose Application builds IpPrefix, LinkAddress and RouteInfo, logs what they answer, print, compare and
# carry through a Parcel, and what LinkProperties of the active network hands out, and compares its log with Android
# 15's (fixtures/expected/net-types-test.logcat, from tools/android-reference.sh --app net-types-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/net-types-test-check.sh
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
tools/build-java-test-app.sh net-types-test >/dev/null

python3 framework/tests/java_app_run.py net-types-test io.github.kisame76.minato.test.nettypes \
  io.github.kisame76.minato.test.nettypes.TypesApplication build/art "$BIONIC/lib" "$BIONIC/system" \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/net-types-test-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/net-types-test/net-types-test.apk \
  fixtures/expected/net-types-test.logcat
