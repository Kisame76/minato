#!/usr/bin/env bash
# Runs ADR-0039 b3.4's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and resources-test (seconds) every time, so it never tests an old binary; then starts resources-test
# through app_process and compares its lines with fixtures/expected/resources-test.txt.
# Needs tools/build-art.sh, tools/build-bionic.sh and tools/build-framework-res.sh first.
# Usage: tools/resources-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh resources-test >/dev/null

python3 framework/tests/java_app_run.py resources-test io.github.kisame76.minato.test.resources \
  io.github.kisame76.minato.test.resources.ResourcesApplication build/art build/bionic/lib build/bionic/system \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/resources-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/resources-test/resources-test.apk \
  fixtures/expected/resources-test.txt
