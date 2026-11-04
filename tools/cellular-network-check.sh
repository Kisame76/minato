#!/usr/bin/env bash
# Runs a cellular network (ADR-0054 step d)) on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar and cellular-network-test every time, so it never tests an old binary; then starts the app's activity
# through app_process without a network and plays the host's network through apk-run's lifecycle: a metered cellular
# path over HSPA (network:2581, radio 10 in bits 8 to 15), as apk-runner-35's cellular network is. The app's log is
# compared with Android 15's there with its Wi-Fi off (fixtures/expected/cellular-network-test.logcat): the network's
# legacy info with its subtype, its capabilities and metering.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/cellular-network-check.sh
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

app=cellular-network-test
tools/build-java-test-app.sh "$app" >/dev/null
package=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "fixtures/apps/$app/AndroidManifest.xml" | head -n 1)
python3 framework/tests/java_app_run.py \
  --lifecycle start,network:2581 "$app" "$package" \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" "build/cellular-network-check/$app" build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
