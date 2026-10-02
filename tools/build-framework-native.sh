#!/usr/bin/env bash
# Builds the framework's guest code (framework/CMakeLists.txt) with the NDK into build/framework-native, with
# Ninja and 6 jobs; Ninja rebuilds only what changed:
#   - app_process64 (ADR-0039 decision 1), which links the ART libraries of tools/build-art.sh;
#   - libandroidfw and the libraries it links as static archives (ADR-0039 b3.2), and androidfw-test, which
#     tools/androidfw-check.sh runs. libandroidfw's AssetsProvider.cpp gets Minato's patch
#     (framework/native/patches) in a copy, build/framework-native/patched, so build/sources keeps its digests.
# Needs the sources of tools/fetch-sources.py --set art and --set framework.
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
PATCHED=build/framework-native/patched/libs/androidfw
mkdir -p "$PATCHED"
patch --quiet -o "$PATCHED/AssetsProvider.cpp.new" build/sources/frameworks_base/libs/androidfw/AssetsProvider.cpp \
  framework/native/patches/androidfw-directory-listing.patch
# Replaced only when it changes, so Ninja does not rebuild it each time.
if cmp -s "$PATCHED/AssetsProvider.cpp.new" "$PATCHED/AssetsProvider.cpp"; then
  rm "$PATCHED/AssetsProvider.cpp.new"
else
  mv "$PATCHED/AssetsProvider.cpp.new" "$PATCHED/AssetsProvider.cpp"
fi
cmake -S framework -B build/framework-native -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/framework-native -j 6
echo "build-framework-native: build/framework-native"
