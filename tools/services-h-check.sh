#!/usr/bin/env bash
# Runs the test of bundle B of the service inventory (services-h-test) on the Mac (framework/tests/java_app_run.py):
# fifteen system services that Minato had no manager for (app search, blob store, bugreport, companion device,
# credential, cross-profile apps, device lock, display hash, domain verification, file integrity, health connect, media
# communication, overlay, people and virtual device), each asked for by name and by class, with every zero-argument
# getter and predicate of its class called. It builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process, framework.jar and the test every time, then compares the app's log with Android 15's on
# apk-runner-35-bare (fixtures/expected/services-h-test.logcat, from tools/android-reference.sh --app services-h-test).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/services-h-check.sh
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
tools/build-java-test-app.sh services-h-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start services-h-test \
  io.github.kisame76.minato.test.servicesh io.github.kisame76.minato.test.servicesh.ServicesHApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/services-h-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/services-h-test/services-h-test.apk fixtures/expected/services-h-test.logcat 1290x2796@3,en-US
