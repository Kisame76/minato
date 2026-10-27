#!/usr/bin/env bash
# Runs the test of the device profiles (ADR-0065) on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar (about 10 s) and device-profile-test (seconds) every time, so it never tests an old binary; then
# starts device-profile-test once per profile, with apk-run's --device-profile. Its log under its own tag must be
# Android 15's on apk-runner-35 (fixtures/expected/device-profile-test.logcat, from tools/android-reference.sh --app
# device-profile-test): every name android.os.Build gives is set, the fingerprint is made of the brand, product, device
# and release. The names themselves cannot be Android's (the emulator's are its own, and ro.* properties cannot be
# changed on a running Android), so the check also wants the profile's values under the tag
# with -device, once each.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/device-profile-check.sh
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
app=device-profile-test
tools/build-java-test-app.sh "$app" >/dev/null

failed=0
# <profile>|<the lines under the -device tag, "|" between them>
release=15/AE3A.240806.019/12368160:user/release-keys
profiles=(
  "pixel|manufacturer: Google|brand: google|model: Pixel 9|device: tokay|product: tokay|hardware: tokay"
  "galaxy|manufacturer: samsung|brand: samsung|model: SM-S921B|device: e1s|product: e1sxxx|hardware: s5e9945"
)
build="type: user tags: release-keys"
socs=("soc manufacturer: Google|soc model: Tensor G4|$build|fingerprint: google/tokay/tokay:$release"
  "soc manufacturer: Samsung|soc model: S5E9945|$build|fingerprint: samsung/e1sxxx/e1s:$release")
for index in "${!profiles[@]}"; do
  profile="${profiles[$index]}|${socs[$index]}"
  IFS='|' read -r -a parts <<<"$profile"
  once_args=()
  for line in "${parts[@]:1}"; do once_args+=(--once "I/$app-device: $line"); done
  python3 framework/tests/java_app_run.py --device-profile "${parts[0]}" "${once_args[@]}" \
    "$app" io.github.kisame76.minato.test.devprofile \
    io.github.kisame76.minato.test.devprofile.ProfileApplication build/art build/bionic/lib build/bionic/system \
    build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" "build/device-profile-check-${parts[0]}" build/framework-native/app_process64 \
    build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US || failed=1
done
exit "$failed"
