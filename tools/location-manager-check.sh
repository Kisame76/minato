#!/usr/bin/env bash
# Runs the test of android.location.LocationManager on the runtime's location hub (ADR-0063) on the Mac
# (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into build/native/macos-app, and
# app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s) and the four apps (seconds) every
# time, so it never tests an old binary; then starts each with apk-run's fake location host replaying
# fixtures/location-scripts/location-manager-test.txt and the grant its name says, and compares its log with
# Android 15's on apk-runner-35 (fixtures/expected/<app>.logcat, from tools/android-reference.sh --app <app>):
#   location-manager-test         requests no permission: the providers, and a SecurityException for every call that
#                                 needs one
#   location-manager-test-denied  requests both, granted neither
#   location-manager-test-fine    requests both, granted both: updates, a current location and a GNSS callback, with
#                                 accurate fixes
#   location-manager-test-coarse  requests the coarse one, granted it: the same with the coarse fixes of Android 15
#                                 (2000 m, a few km from the true position) and no GNSS callback
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/location-manager-check.sh
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

failed=0
# <app> <package suffix> <the grant for the host, none for no --grant-location>
for twin in "location-manager-test locman none" "location-manager-test-denied locmandenied denied" \
  "location-manager-test-fine locmanfine fine" "location-manager-test-coarse locmancoarse coarse"; do
  read -r app suffix grant <<<"$twin"
  tools/build-java-test-app.sh "$app" >/dev/null
  grant_args=()
  if [[ "$grant" != none ]]; then grant_args=(--grant-location "$grant"); fi
  python3 framework/tests/java_app_run.py ${grant_args[@]+"${grant_args[@]}"} \
    --location-script fixtures/location-scripts/location-manager-test.txt \
    "$app" "io.github.kisame76.minato.test.$suffix" \
    io.github.kisame76.minato.test.locman.LocationApplication build/art build/bionic/lib build/bionic/system \
    build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" "build/location-check-$app" build/framework-native/app_process64 \
    build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US || failed=1
done
exit "$failed"
