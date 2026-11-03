#!/usr/bin/env bash
# Xcode build phase (project.yml): builds the test dylib for spikes S1 and S1b and signs
# it with the app's identity, once more with the app's bundle ID as signing identifier (S1b), plus an
# ad-hoc signed copy as negative control. Debug builds only.
set -euo pipefail

[[ "$CONFIGURATION" == "Debug" ]] || exit 0

OUT="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/Spikes"
mkdir -p "$OUT"
xcrun clang -target "arm64-apple-${LLVM_TARGET_TRIPLE_OS_VERSION}${LLVM_TARGET_TRIPLE_SUFFIX:-}" \
  -isysroot "$SDKROOT" -dynamiclib -O2 -install_name @rpath/SpikeS1.dylib \
  -o "$OUT/SpikeS1.dylib" "$SRCROOT/app/Spikes/TestLibrary/spike_s1.c"
cp "$OUT/SpikeS1.dylib" "$OUT/SpikeS1-adhoc.dylib"
cp "$OUT/SpikeS1.dylib" "$OUT/SpikeS1-bundleid.dylib"

# The identity Xcode signs the app with (Apple Development, Personal Team); "-" is ad hoc,
# e.g. in the Simulator. Without --identifier, codesign uses the file name ("SpikeS1").
codesign --force --timestamp=none --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" "$OUT/SpikeS1.dylib"
# S1b: same code, identified by the bundle ID like LiveContainer signs every guest file.
codesign --force --timestamp=none --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" \
  --identifier "$PRODUCT_BUNDLE_IDENTIFIER" "$OUT/SpikeS1-bundleid.dylib"
codesign --force --timestamp=none --sign - "$OUT/SpikeS1-adhoc.dylib"
