#!/usr/bin/env bash
# Runs the link properties test on the Mac (framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and net-link-test (seconds) every time, so it never tests an old binary;
# then starts net-link-test through app_process under MINATO_FAKE_NETWORK (the session's fake path monitor: one fixed
# host network, wlan0 with 192.168.7.9/24, 2001:db8::9/64, default routes via 192.168.7.1 and fe80::1), whose
# Application reads getLinkProperties of the active network and logs its structure, and compares its log with Android
# 15's (fixtures/expected/net-link-test.logcat, from tools/android-reference.sh --app net-link-test): an interface,
# addresses with valid prefixes, routes on the interface with a default route, DNS servers, lists nobody can change, a
# toString that prints what the getters hold.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/net-link-check.sh
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
tools/build-java-test-app.sh net-link-test >/dev/null

# The session's fake path monitor (runtime/src/fake_path_monitor.cpp) starts under this variable, in apk-run's process.
export MINATO_FAKE_NETWORK=1
python3 framework/tests/java_app_run.py net-link-test \
  io.github.kisame76.minato.test.netlink \
  io.github.kisame76.minato.test.netlink.LinkApplication build/art "$BIONIC/lib" "$BIONIC/system" \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/net-link-test-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/net-link-test/net-link-test.apk \
  fixtures/expected/net-link-test.logcat 1290x2796@3,en-US
