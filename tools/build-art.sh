#!/usr/bin/env bash
# Builds ART as guest code (ADR-0033 a): fetches the pinned sources of the list "art" if needed, then builds ART and
# its libraries in the release and the debug flavor with the NDK into build/art/lib and dalvikvm into build/art/bin
# (runtime/art). ART links the guest system's libraries, so tools/build-bionic.sh comes first.
# Usage: tools/build-art.sh [ninja arguments]
# Optional environment:
#   ANDROID_NDK  NDK to build with, default: r30 in ~/Library/Android/sdk/ndk
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi

python3 tools/fetch-sources.py --set art
cmake -S runtime/art -B build/art -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=none \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/art "$@"
