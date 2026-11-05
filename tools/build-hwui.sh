#!/usr/bin/env bash
# Builds hwui's host flavor as guest code (ADR-0043 a1): fetches the pinned sources of the list "graphics" and those of
# the list "framework" that the plan reads (frameworks/base's androidfw and text flags, libpng, libcutils and libutils,
# incremental_delivery, build/release's flag values) if needed, then builds libhwui, what it links and libjnigraphics
# (ADR-0041 a5) with the NDK into build/hwui/lib (graphics/hwui). hwui links the guest system's libraries of bionic
# and ART, so tools/build-bionic.sh and tools/build-art.sh come first; they fetch the lists "sources" and "art",
# which the plan reads too.
# Usage: tools/build-hwui.sh [ninja arguments]
# Optional environment:
#   ANDROID_NDK  NDK to build with, default: r30 in ~/Library/Android/sdk/ndk
#   HWUI_BUILD   the build directory, default: build/hwui
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi

python3 tools/fetch-sources.py --set graphics
python3 tools/fetch-sources.py --set framework \
  frameworks_base libpng system_core_framework incremental_delivery build_release
build="${HWUI_BUILD:-build/hwui}"
# Keep the Mac's build paths out of the guest libraries (tools/check-no-build-paths.sh enforces it).
pfx="$(tools/prefix-map-flags.sh "$NDK")"
cmake -S graphics/hwui -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=none \
  -DCMAKE_C_FLAGS="$pfx" -DCMAKE_CXX_FLAGS="$pfx" -DCMAKE_ASM_FLAGS="$pfx" -DAPK_PREFIX_MAP="$pfx" \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" "$@"
