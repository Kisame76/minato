#!/usr/bin/env bash
# Builds graphics/host for the app (ADR-0030): the providers of the front libs' host tables and the probe
# of V3's check, which only debug builds link (project.yml), for the iOS device and the Simulator, packed
# into build/MinatoGraphics.xcframework. It needs tools/frontgen's lists from tools/build-front-libs.sh and
# MoltenVK's headers from tools/fetch-moltenvk.sh. Build folders: build/native/<preset>-graphics-host.
set -euo pipefail
cd "$(dirname "$0")/.."

for needed in build/graphics/gen/vulkan_names.inc build/moltenvk/include/vulkan/vulkan_metal.h; do
  if [[ ! -f "$needed" ]]; then
    echo "$needed is missing: run tools/build-front-libs.sh and tools/fetch-moltenvk.sh first." >&2
    exit 1
  fi
done

xcframework=build/MinatoGraphics.xcframework
arguments=()
libraries=()
for preset in ios ios-simulator; do
  build="build/native/$preset-graphics-host"
  # The preset's toolchain in a folder of its own. With OPTIMIZE_DEPENDENCIES the static library does not
  # wait for the runtime it names for the tests on the Mac: the app links the runtime from
  # APKRuntime.xcframework (tools/build-native.sh --app).
  cmake --preset "$preset" -B "$build" -D CMAKE_OPTIMIZE_DEPENDENCIES=ON >/dev/null
  cmake --build "$build" --target minato_graphics_host
  libraries+=("$build/graphics/host/libminato_graphics_host.a")
  arguments+=(-library "$build/graphics/host/libminato_graphics_host.a" -headers graphics/host/include)
done

# Packed again only when a library or header changed, so Xcode does not relink the app for nothing.
if [[ ! -f "$xcframework/Info.plist" ]] \
  || [[ -n "$(find "${libraries[@]}" graphics/host/include -newer "$xcframework/Info.plist" -print -quit)" ]]; then
  rm -rf "$xcframework"
  xcodebuild -create-xcframework "${arguments[@]}" -output "$xcframework" >/dev/null
  echo "Packed $xcframework"
fi
