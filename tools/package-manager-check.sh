#!/usr/bin/env bash
# Runs ADR-0039 b3.5a's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and package-manager-test (seconds) every time, so it never tests an old binary; then starts
# package-manager-test through app_process and compares its log with Android 15's
# (fixtures/expected/package-manager-test.logcat, from tools/android-reference.sh --app package-manager-test).
# Needs tools/build-art.sh, tools/build-bionic.sh and tools/build-framework-res.sh first.
# Usage: tools/package-manager-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh package-manager-test >/dev/null

python3 framework/tests/java_app_run.py package-manager-test io.github.kisame76.minato.test.packagemanager \
  io.github.kisame76.minato.test.packagemanager.PackageManagerApplication build/art build/bionic/lib \
  build/bionic/system build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/package-manager-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/package-manager-test/package-manager-test.apk \
  fixtures/expected/package-manager-test.logcat
