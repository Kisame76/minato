#!/usr/bin/env bash
# Builds the NDK test programs in fixtures/ for arm64-v8a into build/fixtures/bin (ADR-0016), and packs
# each test app of fixtures/apps/<app>/ (its AndroidManifest.xml, lib<app>.so and assets/, if any) into
# build/fixtures/apks/<app>.apk with tools/make-apk.sh (ADR-0028, decision 15). An app without native code,
# for which the build makes no lib<app>.so, has a script of its own (tools/build-java-test-app.sh).
# Usage: tools/build-fixtures.sh
# Optional environment:
#   ANDROID_NDK     NDK to build with, default: r30 in ~/Library/Android/sdk/ndk
#   FIXTURES_BUILD  the build directory, default: build/fixtures
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi

# API level 29 (Android 10): the first with native ELF TLS, and below 30 like the minSdk of most apps,
# so the NDK links with --no-rosegment and read-only data shares the executable segment (risk R1).
build="${FIXTURES_BUILD:-build/fixtures}"
cmake -S fixtures -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$build"

mkdir -p "$build/apks"
for manifest in fixtures/apps/*/AndroidManifest.xml; do
  [[ -e "$manifest" ]] || continue
  app=$(basename "$(dirname "$manifest")")
  [[ -f "$build/bin/lib$app.so" ]] || continue
  assets=()
  [[ ! -d "fixtures/apps/$app/assets" ]] || assets=(--assets "fixtures/apps/$app/assets")
  tools/make-apk.sh --manifest "$manifest" --lib "$build/bin/lib$app.so" ${assets[@]+"${assets[@]}"} \
    --out "$build/apks/$app.apk"
done
