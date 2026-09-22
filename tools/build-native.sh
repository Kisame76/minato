#!/usr/bin/env bash
# Builds the native code (runtime/, importer/converter/) with CMake and Ninja (ADR-0016).
# Usage:
#   tools/build-native.sh        NDK fixtures, macOS build with tests, then the part for the app
#   tools/build-native.sh --app  only the part for the app: iOS device and Simulator builds,
#                                packed into build/APKRuntime.xcframework, which project.yml links
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != "--app" ]]; then
  tools/build-fixtures.sh
  cmake --preset macos >/dev/null
  cmake --build --preset macos
  ctest --preset macos
fi

for preset in ios ios-simulator; do
  cmake --preset "$preset" >/dev/null
  cmake --build --preset "$preset"
done

# Packed again only when a library or header changed, so Xcode does not relink the app for nothing.
XCFRAMEWORK=build/APKRuntime.xcframework
LIBRARIES=(build/native/ios/runtime/libapkrt.a build/native/ios-simulator/runtime/libapkrt.a)
if [[ ! -f "$XCFRAMEWORK/Info.plist" ]] \
  || [[ -n "$(find "${LIBRARIES[@]}" runtime/include -newer "$XCFRAMEWORK/Info.plist" -print -quit)" ]]; then
  rm -rf "$XCFRAMEWORK"
  xcodebuild -create-xcframework \
    -library "${LIBRARIES[0]}" -headers runtime/include \
    -library "${LIBRARIES[1]}" -headers runtime/include \
    -output "$XCFRAMEWORK" >/dev/null
  echo "Packed $XCFRAMEWORK"
fi
