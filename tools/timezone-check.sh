#!/usr/bin/env bash
# Runs the time zone test on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert
# for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar and
# timezone-test every time, so it never tests an old binary; then starts timezone-test through app_process in a
# session on the M4 plan's display and compares its log with Android 15's (fixtures/expected/timezone-test.logcat,
# from tools/android-reference.sh --app timezone-test): the tzdata module's files as Android 15 lays them out, and what
# java.util.TimeZone, java.time, ICU and the date formats answer from them. The device's zone, which its log has with
# the device's tag, must be the Mac's in all three, as the runtime gives it in persist.sys.timezone.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh and
# tools/fetch-sources.py --set art (system/timezone's lookups and icu-data's time zone resources) first.
# Usage: tools/timezone-check.sh
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
tools/build-java-test-app.sh timezone-test >/dev/null

output=$(python3 framework/tests/java_app_run.py timezone-test io.github.kisame76.minato.test.timezone \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/timezone-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/timezone-test/timezone-test.apk fixtures/expected/timezone-test.logcat \
  1290x2796@3,en-US) || { echo "$output"; exit 1; }
echo "$output"
if grep -q "^skipped" <<<"$output"; then exit 0; fi
# The Mac's zone, as its /etc/localtime names it.
zone=$(readlink /etc/localtime | sed 's#.*/zoneinfo/##')
if ! grep -qxF "  device: I/timezone-test-device: device zone: $zone, java.time $zone, icu $zone" <<<"$output"; then
  echo "FAIL: timezone-test's zone is not the Mac's, $zone"
  exit 1
fi
echo "timezone-test's zone is the Mac's, $zone"
# One IANA release for bionic, libcore and ICU (ADR-0033 decision 9): the release of bionic's tzdata is the module's
# tz_version and the data version of ICU's resources.
release=$(head -c 12 build/bionic/system/usr/share/zoneinfo/tzdata | tail -c 6 | tr -d '\0')
for line in "tz_version: 008.001|$release|001" "icu data version: $release"; do
  if ! grep -qxF "  device: I/timezone-test-device: $line" <<<"$output"; then
    echo "FAIL: timezone-test did not log \"$line\""
    exit 1
  fi
done
echo "timezone-test: bionic, libcore and ICU have the release $release"
