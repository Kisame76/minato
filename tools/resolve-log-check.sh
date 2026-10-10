#!/usr/bin/env bash
# Runs the test of ART's log of failed resolutions on the Mac (framework/tests/java_app_run.py; the Unity test APK probe: a
# caught NoClassDefFoundError of a missing framework class named no gap): builds apk-run and apk-convert for macOS into
# build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and
# resolve-log-test (seconds) every time, so it never tests an old binary; then starts it through app_process and
# compares what it prints and ART's minato-resolve lines with fixtures/expected/resolve-log-test.txt, each of those
# lines exactly once: a missing class, method, static and instance field, each met twice and caught.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/resolve-log-check.sh
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

once=()
while read -r line; do
  once+=(--once "${line#logcat }")
done < <(grep '^logcat W/minato-resolve: ' fixtures/expected/resolve-log-test.txt)
python3 framework/tests/java_app_run.py "${once[@]}" resolve-log-test io.github.kisame76.minato.test.resolvelog \
  io.github.kisame76.minato.test.resolvelog.ResolveLogApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/resolve-log-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/resolve-log-test/resolve-log-test.apk fixtures/expected/resolve-log-test.txt
