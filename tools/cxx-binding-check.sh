#!/usr/bin/env bash
# Runs the test of an app library's C++ runtime on the Mac (framework/tests/java_app_run.py; the self-quit crash of
# Unity games, retest 12): builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with
# libandroid_runtime.so (incremental), framework.jar (about 10 s) and cxx-binding-test (seconds) every time, so it
# never tests an old binary; then starts it through app_process and compares its log with Android 15's on
# apk-runner-35 (fixtures/expected/cxx-binding-test.logcat, from tools/android-reference.sh --app cxx-binding-test): a
# JNI library with the NDK's static C++ library binds its personality routine, __cxa_throw, __cxa_begin_catch,
# operator new and std::terminate to its own copies, as Unity's libil2cpp.so must.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/cxx-binding-check.sh
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
tools/build-java-test-app.sh cxx-binding-test >/dev/null

python3 framework/tests/java_app_run.py cxx-binding-test io.github.kisame76.minato.test.cxxbinding \
  io.github.kisame76.minato.test.cxxbinding.CxxBindingApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/cxx-binding-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/cxx-binding-test/cxx-binding-test.apk \
  fixtures/expected/cxx-binding-test.logcat
