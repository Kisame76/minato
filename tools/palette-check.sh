#!/usr/bin/env bash
# Runs ADR-0043 step f)'s test of Monet's palette on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and palette-test (seconds) every time, so it never tests an old binary; then starts
# palette-test through app_process. It logs every public system color as the app's Resources and the system's give
# it, and a TextView's default colors; its log must be Android 15's (fixtures/expected/palette-test.logcat, from
# tools/android-reference.sh --app palette-test), whose SystemUI lays Monet's overlays over framework-res.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/palette-check.sh
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
tools/build-java-test-app.sh palette-test >/dev/null

python3 framework/tests/java_app_run.py palette-test io.github.kisame76.minato.test.palette \
  io.github.kisame76.minato.test.palette.PaletteApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/palette-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/palette-test/palette-test.apk \
  fixtures/expected/palette-test.logcat
