#!/usr/bin/env bash
# Builds the framework's guest code (framework/CMakeLists.txt) with the NDK into build/framework-native, with
# Ninja and 6 jobs; Ninja rebuilds only what changed:
#   - app_process64 (ADR-0039 decision 1), which links the ART libraries of tools/build-art.sh;
#   - libandroidfw and the libraries it links as static archives (ADR-0039 b3.2), and androidfw-test, which
#     tools/androidfw-check.sh runs. libandroidfw's AssetsProvider.cpp gets Minato's patch
#     (framework/native/patches) in a copy, build/framework-native/patched, so build/sources keeps its digests;
#     so do the five core/jni files of the GLES bindings (ADR-0041 a), opengl-without-libgui.patch).
# Needs the sources of tools/fetch-sources.py --set art, --set framework and --set graphics (hwui's jni_runtime.h), and
# tools/build-hwui.sh's libhwui.so, which libandroid_runtime links (ADR-0043 test 2).
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
# The GLES bindings' five files (ADR-0041 a)): patched together in a scratch folder, each copy replaced only when it
# changes.
GLES_PATCHED=build/framework-native/patched/core/jni
GLES_FILES=(android_opengl_EGL14.cpp android_opengl_EGL15.cpp android_opengl_EGLExt.cpp
  com_google_android_gles_jni_EGLImpl.cpp android/opengl/util.cpp)
GLES_WORK=$(mktemp -d build/framework-native/patch.XXXXXX)
for file in "${GLES_FILES[@]}"; do
  mkdir -p "$GLES_WORK/$(dirname "$file")"
  cp "build/sources/frameworks_base/core/jni/$file" "$GLES_WORK/$file"
done
patch --quiet -d "$GLES_WORK" -p1 < framework/native/patches/opengl-without-libgui.patch
for file in "${GLES_FILES[@]}"; do
  mkdir -p "$GLES_PATCHED/$(dirname "$file")"
  cmp -s "$GLES_WORK/$file" "$GLES_PATCHED/$file" || cp "$GLES_WORK/$file" "$GLES_PATCHED/$file"
done
rm -rf "${GLES_WORK:?}"
# Keep the Mac's build paths out of the guest libraries (tools/check-no-build-paths.sh enforces it).
pfx="$(tools/prefix-map-flags.sh "$NDK")"
cmake -S framework -B build/framework-native -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=c++_static \
  -DCMAKE_C_FLAGS="$pfx" -DCMAKE_CXX_FLAGS="$pfx" -DCMAKE_ASM_FLAGS="$pfx" \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/framework-native -j 6
echo "build-framework-native: build/framework-native"
