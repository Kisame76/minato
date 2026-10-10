#!/usr/bin/env bash
# Runs the test of the gap scanner's switch on the Mac (framework/tests/java_app_run.py): with MINATO_GAPSCAN=1 in its
# environment, an app's process scans its own base.apk before the Application is made (ActivityThread, a developer
# diagnostic), so that an app on the iPhone, where no other process can read its APK, is scanned too. Builds apk-run
# and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and resolve-log-test (seconds) every time, so it never tests an old binary; then starts
# resolve-log-test with the switch and compares what it prints and the scan's gapscan lines with
# fixtures/expected/gapscan-switch.txt, between the scan's first line and its last, which names the time it took.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/gapscan-switch-check.sh
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
tools/build-java-test-app.sh resolve-log-test >/dev/null

python3 framework/tests/java_app_run.py --env MINATO_GAPSCAN=1 --in-order "I/gapscan : scanning base.apk of " \
  --in-order "I/gapscan : done after " resolve-log-test \
  io.github.kisame76.minato.test.resolvelog io.github.kisame76.minato.test.resolvelog.ResolveLogApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/gapscan-switch-check \
  build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/resolve-log-test/resolve-log-test.apk \
  fixtures/expected/gapscan-switch.txt
