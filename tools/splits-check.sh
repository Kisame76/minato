#!/usr/bin/env bash
# Runs the test of split sets on the Mac (ADR-0048 c); framework/tests/java_app_run.py): builds apk-run and apk-convert
# for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about
# 10 s) and splits-test (bundletool, seconds) every time, so it never tests an old binary; then starts splits-test
# through app_process with the base and one --split for each split of the set, and compares its log with Android 15's
# on apk-runner-35 (fixtures/expected/splits-test.logcat, from tools/android-reference.sh --app splits-test): the split
# names and paths of ApplicationInfo and PackageInfo, a class of a feature split through the app's own class loader,
# the feature split's asset, the string only the language split holds, the arm64 split's library, and the assets
# through the NDK over base and splits. The guest libraries in build/bionic hold libandroid.so, whose asset manager
# reads the set; build them first when runtime/android changed.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/splits-check.sh
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
tools/build-splits-test.sh >/dev/null

splits=()
# In reverse order of the names, since ActivityThread lists them in string order whichever order it gets them in.
for split in $(ls -r build/splits-test/set/split_*.apk); do splits+=(--split "$split"); done
python3 framework/tests/java_app_run.py "${splits[@]}" splits-test io.github.kisame76.minato.test.splits \
  io.github.kisame76.minato.test.splits.SplitsApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/splits-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/splits-test/set/base.apk fixtures/expected/splits-test.logcat
