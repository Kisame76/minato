#!/usr/bin/env bash
# Checks an activity chain of results (result-chain-test): A starts B, B starts C, C ends at once with a result, B
# answers A from inside its own onActivityResult with finish(), and A gets B's result. Runs on the Mac
# (framework/tests/java_app_run.py) and compares with fixtures/expected/result-chain-test.logcat (Android 15's log).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/result-chain-check.sh
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
tools/build-java-test-app.sh result-chain-test >/dev/null

python3 framework/tests/java_app_run.py result-chain-test \
  io.github.kisame76.minato.test.resultchain io.github.kisame76.minato.test.resultchain.ChainApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/result-chain-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/result-chain-test/result-chain-test.apk fixtures/expected/result-chain-test.logcat 1290x2796@3,en-US


