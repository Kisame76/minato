#!/usr/bin/env bash
# Runs ADR-0039's test 3 on the Mac (b3.4c, framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and config-test (seconds) every time, so it never tests an old binary; then starts config-test through
# app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US,
# and compares its log with Android 15's on that display (fixtures/expected/config-test.logcat, from
# tools/android-reference.sh --app config-test).
# Needs tools/build-art.sh, tools/build-bionic.sh and tools/build-framework-res.sh first.
# Usage: tools/config-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh config-test >/dev/null

python3 framework/tests/java_app_run.py config-test io.github.kisame76.minato.test.config \
  io.github.kisame76.minato.test.config.ConfigApplication build/art build/bionic/lib build/bionic/system \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/config-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/config-test/config-test.apk \
  fixtures/expected/config-test.logcat 1290x2796@3,en-US
