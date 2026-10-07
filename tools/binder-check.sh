#!/usr/bin/env bash
# Runs ADR-0043 step d2b)'s test of the app's own binders on the Mac (framework/tests/java_app_run.py): builds apk-run
# and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and binder-test (seconds) every time, so it never tests an old binary; then starts
# binder-test through app_process. Its classes extend Binder and ResultReceiver and implement IBinder, which ART checks
# against the boot class path's IBinder, ShellCallback and ResultReceiver among its types; it logs what its binders
# and receivers answer, and its log must be Android 15's (fixtures/expected/binder-test.logcat, from
# tools/android-reference.sh --app binder-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/binder-check.sh
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
tools/build-java-test-app.sh binder-test >/dev/null

python3 framework/tests/java_app_run.py binder-test io.github.kisame76.minato.test.binder \
  io.github.kisame76.minato.test.binder.BinderApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/binder-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/binder-test/binder-test.apk \
  fixtures/expected/binder-test.logcat
