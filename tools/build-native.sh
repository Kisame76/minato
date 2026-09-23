#!/usr/bin/env bash
# Builds the native code (runtime/, importer/converter/) with CMake and Ninja (ADR-0016).
# Usage:
#   tools/build-native.sh        NDK fixtures, macOS build with tests, then the part for the app
#   tools/build-native.sh --app  only the part for the app: iOS device and Simulator builds,
#                                packed into build/APKRuntime.xcframework and build/APKConvert.xcframework,
#                                which project.yml links
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

# Packs <library path in the build> with the headers in <directory> into build/<name>.xcframework:
# the runtime and the converter, which the app runs on the iPhone (ADR-0022). Packed again only
# when a library or header changed, so Xcode does not relink the app for nothing.
pack() {
  local xcframework="build/$1.xcframework"
  local libraries=("build/native/ios/$2" "build/native/ios-simulator/$2")
  if [[ ! -f "$xcframework/Info.plist" ]] \
    || [[ -n "$(find "${libraries[@]}" "$3" -newer "$xcframework/Info.plist" -print -quit)" ]]; then
    rm -rf "$xcframework"
    xcodebuild -create-xcframework \
      -library "${libraries[0]}" -headers "$3" \
      -library "${libraries[1]}" -headers "$3" \
      -output "$xcframework" >/dev/null
    echo "Packed $xcframework"
  fi
}
pack APKRuntime runtime/libapkrt.a runtime/include
pack APKConvert importer/converter/libapkconv.a importer/converter/capi
