#!/usr/bin/env bash
# Builds the framework's guest code (framework/CMakeLists.txt) with the NDK into build/framework-native; Ninja
# rebuilds only what changed:
#   - app_process64 (ADR-0039 decision 1), which links the ART libraries of tools/build-art.sh.
# Needs the sources of tools/fetch-sources.py --set art.
# Usage: tools/build-framework-native.sh
# Optional environment:
#   ANDROID_NDK  NDK to build with, default: r30 in ~/Library/Android/sdk/ndk
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi
cmake -S framework -B build/framework-native -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/framework-native
echo "build-framework-native: build/framework-native"
