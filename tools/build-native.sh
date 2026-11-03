#!/usr/bin/env bash
# Builds the native code (runtime/, importer/converter/) with CMake and Ninja (ADR-0016).
# Usage:
#   tools/build-native.sh                  NDK fixtures, macOS build with tests, then the part for the app
#   tools/build-native.sh --app            only the part for the app: iOS device and Simulator builds,
#                                          packed into build/APKRuntime.xcframework and
#                                          build/APKConvert.xcframework, which project.yml links
#   tools/build-native.sh --app --release  the same, optimized, for the benchmark build (ADR-0037); the next
#                                          run without --release packs the debug builds again
set -euo pipefail
cd "$(dirname "$0")/.."

app=no
configuration=Debug
suffix=""
for argument in "$@"; do
  case "$argument" in
    --app) app=yes ;;
    --release) configuration=Release suffix=-release ;;
    *)
      echo "usage: tools/build-native.sh [--app [--release]]" >&2
      exit 2
      ;;
  esac
done
if [[ "$configuration" == Release && "$app" != yes ]]; then
  echo "--release only goes with --app" >&2
  exit 2
fi

if [[ "$app" != yes ]]; then
  tools/build-fixtures.sh
  cmake --preset macos >/dev/null
  cmake --build --preset macos
  ctest --preset macos
fi

# Keep the Mac's build paths out of the app's native code: __FILE__ (CHECK/LOG macros) and debug info would
# otherwise bake in the absolute build root (tools/check-no-build-paths.sh enforces it on a release).
pfx="$(tools/prefix-map-flags.sh)"
for preset in ios ios-simulator; do
  cmake --preset "$preset$suffix" \
    -DCMAKE_C_FLAGS="$pfx" -DCMAKE_CXX_FLAGS="$pfx" -DCMAKE_ASM_FLAGS="$pfx" >/dev/null
  cmake --build --preset "$preset$suffix"
done

# Packs <library path in the build> with the headers in <directory> into build/<name>.xcframework:
# the runtime and the converter, which the app runs on the iPhone (ADR-0022), with the configuration in
# build/<name>.xcframework.configuration. Packed again only when a library or header changed or the
# configuration differs, so Xcode does not relink the app for nothing and never links a mix.
pack() {
  local xcframework="build/$1.xcframework"
  local libraries=("build/native/ios$suffix/$2" "build/native/ios-simulator$suffix/$2")
  if [[ ! -f "$xcframework/Info.plist" ]] \
    || [[ "$(cat "$xcframework.configuration" 2>/dev/null)" != "$configuration" ]] \
    || [[ -n "$(find "${libraries[@]}" "$3" -newer "$xcframework/Info.plist" -print -quit)" ]]; then
    rm -rf "${xcframework:?}"
    xcodebuild -create-xcframework \
      -library "${libraries[0]}" -headers "$3" \
      -library "${libraries[1]}" -headers "$3" \
      -output "$xcframework" >/dev/null
    echo "$configuration" >"$xcframework.configuration"
    echo "Packed $xcframework ($configuration)"
  fi
}
pack APKRuntime runtime/libapkrt.a runtime/include
pack APKConvert importer/converter/libapkconv.a importer/converter/capi
