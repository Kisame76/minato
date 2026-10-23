#!/usr/bin/env bash
# Runs the test of android.hardware.SensorManager on the sensor hub (ADR-0061 step 2) on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and sensors-java-test (seconds) every
# time, so it never tests an old binary; then starts sensors-java-test with apk-run's fake sensor host replaying
# fixtures/sensor-scripts/sensors-java-test.txt (50 Hz for each of the nine types), whose log must be what Android 15
# logs on apk-runner-35 (fixtures/expected/sensors-java-test.logcat, from tools/android-reference.sh --app
# sensors-java-test): for each type the sensor's shape, that a listener gets events with finite values and increasing
# fresh timestamps from the same sensor on the main thread, and that none arrive after unregistering.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/sensors-java-check.sh
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
tools/build-java-test-app.sh sensors-java-test >/dev/null

python3 framework/tests/java_app_run.py --sensor-script fixtures/sensor-scripts/sensors-java-test.txt \
  sensors-java-test io.github.kisame76.minato.test.sensorsjava \
  io.github.kisame76.minato.test.sensorsjava.SensorsApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/sensors-java-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/sensors-java-test/sensors-java-test.apk fixtures/expected/sensors-java-test.logcat \
  1290x2796@3,en-US
