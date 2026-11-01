#!/usr/bin/env bash
# Builds the guest side of Minato's graphics (ADR-0029, ADR-0030) for arm64-v8a with the Android NDK:
# tools/frontgen's tables, stubs and export lists into build/graphics/gen/, and libvulkan.so, libEGL.so,
# libGLESv1_CM.so, libGLESv2.so and libGLESv3.so with the test programs of graphics/guest/ into
# build/graphics/guest/bin. Needs vk.xml and MoltenVK's headers from tools/fetch-moltenvk.sh and ANGLE's
# registries from tools/build-angle.sh.
# Usage: tools/build-front-libs.sh
# Optional environment:
#   ANDROID_NDK  NDK to build with, default: r30 in ~/Library/Android/sdk/ndk
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi

# API level 29, as the fixtures (tools/build-fixtures.sh).
# Keep the Mac's build paths out of the guest libraries (tools/check-no-build-paths.sh enforces it).
pfx="$(tools/prefix-map-flags.sh)"
cmake -S graphics/guest -B build/graphics/guest -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_C_FLAGS="$pfx" -DCMAKE_CXX_FLAGS="$pfx" -DCMAKE_ASM_FLAGS="$pfx" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build/graphics/guest -j 6
