#!/usr/bin/env bash
# Runs ADR-0048 c4's NativeActivity half on the Mac: activity-test as a real split set, the set of
# tools/build-activity-splits.sh (a base APK and a config split that holds the NativeActivity's library, put
# together and split by bundletool as Play serves an app), through activity-host with the base and the split as
# Android's installer names them. The scripted host of runtime/tests/activity_test.cpp plays the lifecycle, and the
# app's lines must be Android 15's (fixtures/expected/activity-test.logcat, from two equal runs of the set on the AVD:
# APK_SET=build/activity-splits/activity-splits.apks tools/android-reference.sh --app activity-test). The library
# of the split must be the fixture's, byte for byte, so that the run uses what the set holds.
# Needs tools/build-fixtures.sh, tools/build-bionic.sh and tools/build-framework-res.sh first.
# Usage: tools/activity-splits-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-activity-splits
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target activity_test apk-convert >/dev/null
tools/build-activity-splits.sh >/dev/null 2>&1
SET=build/activity-splits/set
LIBRARY=build/fixtures/bin/libactivity-test.so
if ! /usr/bin/python3 - "$SET/split_config.arm64_v8a.apk" "$LIBRARY" <<'PY'
import sys
import zipfile

split, library = sys.argv[1:]
with zipfile.ZipFile(split) as archive:
    inside = archive.read("lib/arm64-v8a/libactivity-test.so")
    base = zipfile.ZipFile(split.replace("split_config.arm64_v8a.apk", "base.apk"))
    if any(name.startswith("lib/") for name in base.namelist()):
        sys.exit("the base holds a library: the set does not put it in the split")
if inside != open(library, "rb").read():
    sys.exit("the split's library is not the fixture's")
PY
then
  echo "FAIL: activity-splits-check: the set is not as expected"
  exit 1
fi
"$NATIVE/runtime/activity_test" "$NATIVE/activity_splits_scratch" build/fixtures/bin build/bionic/lib \
  build/bionic/system fixtures/expected set "$SET"
echo "activity-splits-check: passed"
