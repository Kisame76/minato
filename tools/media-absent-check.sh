#!/usr/bin/env bash
# Runs the Unity test APK gap sort category 2 test on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and media-absent-test every time, so it never tests an old binary; then starts
# media-absent-test through app_process in a session on the M4 plan's display and compares its log with Android
# 15's (fixtures/expected/media-absent-test.logcat, from tools/android-reference.sh --app media-absent-test):
# what an app meets in android.media's classes that need hardware or native parts Minato lacks: DRM and crypto objects
# for a scheme without a plugin, the codec list, the retriever and the extractor without a source, the image reader and
# the TV input service. What depends on the device's codecs and DRM plugins is only shown.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/media-absent-check.sh
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
tools/build-java-test-app.sh media-absent-test >/dev/null

python3 framework/tests/java_app_run.py media-absent-test io.github.kisame76.minato.test.mediaabsent \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/media-absent-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/media-absent-test/media-absent-test.apk fixtures/expected/media-absent-test.logcat \
  1290x2796@3,en-US
