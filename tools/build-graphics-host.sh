#!/usr/bin/env bash
# Builds graphics/host for the app (ADR-0030): the providers of the front libs' host tables, the game view's layer
# with the benchmark's recorder (ADR-0037) and the probe of V3's check, which only debug builds link (project.yml),
# for the iOS device and the Simulator, packed into build/MinatoGraphics.xcframework with the configuration in
# build/MinatoGraphics.xcframework.configuration. It needs tools/frontgen's lists from tools/build-front-libs.sh and
# MoltenVK's headers from tools/fetch-moltenvk.sh. Build folders: build/native/<preset>-graphics-host.
# Usage:
#   tools/build-graphics-host.sh            Debug, which the app links
#   tools/build-graphics-host.sh --release  optimized, for the benchmark build (ADR-0037, decision 3); the next run
#                                           without --release packs the debug builds again
set -euo pipefail
cd "$(dirname "$0")/.."

configuration=Debug
suffix=""
for argument in "$@"; do
  case "$argument" in
    --release) configuration=Release suffix=-release ;;
    *)
      echo "usage: tools/build-graphics-host.sh [--release]" >&2
      exit 2
      ;;
  esac
done

for needed in build/graphics/gen/vulkan_names.inc build/moltenvk/include/vulkan/vulkan_metal.h; do
  if [[ ! -f "$needed" ]]; then
    echo "$needed is missing: run tools/build-front-libs.sh and tools/fetch-moltenvk.sh first." >&2
    exit 1
  fi
done

xcframework=build/MinatoGraphics.xcframework
# Keep the Mac's build paths out of the app's graphics host code (tools/check-no-build-paths.sh enforces it).
pfx="$(tools/prefix-map-flags.sh)"
arguments=()
libraries=()
for preset in "ios$suffix" "ios-simulator$suffix"; do
  build="build/native/$preset-graphics-host"
  # The preset's toolchain in a folder of its own. With OPTIMIZE_DEPENDENCIES the static library does not
  # wait for the runtime it names for the tests on the Mac: the app links the runtime from
  # APKRuntime.xcframework (tools/build-native.sh --app).
  cmake --preset "$preset" -B "$build" -D CMAKE_OPTIMIZE_DEPENDENCIES=ON \
    -DCMAKE_C_FLAGS="$pfx" -DCMAKE_CXX_FLAGS="$pfx" -DCMAKE_ASM_FLAGS="$pfx" >/dev/null
  cmake --build "$build" --target minato_graphics_host
  libraries+=("$build/graphics/host/libminato_graphics_host.a")
  arguments+=(-library "$build/graphics/host/libminato_graphics_host.a" -headers graphics/host/include)
done

# Packed again only when a library or header changed or the configuration differs, so Xcode does not relink the
# app for nothing and never links a mix.
if [[ ! -f "$xcframework/Info.plist" ]] \
  || [[ "$(cat "$xcframework.configuration" 2>/dev/null)" != "$configuration" ]] \
  || [[ -n "$(find "${libraries[@]}" graphics/host/include -newer "$xcframework/Info.plist" -print -quit)" ]]; then
  rm -rf "$xcframework"
  xcodebuild -create-xcframework "${arguments[@]}" -output "$xcframework" >/dev/null
  echo "$configuration" >"$xcframework.configuration"
  echo "Packed $xcframework ($configuration)"
fi
