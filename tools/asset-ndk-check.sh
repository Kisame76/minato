#!/usr/bin/env bash
# Runs ADR-0041 decision 7's test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for
# macOS into build/native/macos-checks2 and asset-ndk-test, then starts asset-ndk-test through app_process. Its JNI
# library takes the Context's AssetManager through libandroid's AAssetManager_fromJava, as Godot takes its project's,
# and reads its assets; its log must be Android 15's (fixtures/expected/asset-ndk-test.logcat, from
# tools/android-reference.sh --app asset-ndk-test). app_process, libandroid_runtime.so and framework.jar are the shared
# builds as they are (tools/build-framework-native.sh, tools/build-framework.sh), which this check does not make.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh and those two
# first.
# Usage: tools/asset-ndk-check.sh
# Optional environment:
#   BIONIC_LIB  the guest's libraries, libandroid.so among them, default: build/bionic/lib
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
for built in build/framework-native/app_process64 build/framework/framework.jar; do
  if [[ ! -f "$built" ]]; then echo "skipped: $built is not built"; exit 0; fi
done

NATIVE=build/native/macos-checks2
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-java-test-app.sh asset-ndk-test >/dev/null

python3 framework/tests/java_app_run.py asset-ndk-test io.github.kisame76.minato.test.assetndk \
  io.github.kisame76.minato.test.assetndk.AssetNdkApplication build/art "${BIONIC_LIB:-build/bionic/lib}" \
  build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/asset-ndk-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/asset-ndk-test/asset-ndk-test.apk fixtures/expected/asset-ndk-test.logcat
