#!/usr/bin/env bash
# Xcode build phase (project.yml): puts the guest system into every app build (ADR-0028, decision 1 and its
# consequences; ADR-0032, decision 10), as game mode mounts it for activity-host. In Guest/:
#   bin/activity-host.dylib   the program /system/bin/activity-host
#   lib64/<library>.so.dylib  the guest's /system/lib64: bionic, liblog, libnetd_client, libandroid,
#                             libnativewindow, libaaudio (ADR-0036) and libz, and the graphics front libs
#                             libEGL, libGLESv2, libGLESv3 and libvulkan (ADR-0029, ADR-0030)
#   system/                   the guest's /system for files: bionic's time zone file
# Every container is converted at build time and signed with the app's identity and the bundle ID as
# identifier (route 1 of ADR-0008). tools/build-bionic.sh builds what it takes from build/bionic,
# tools/build-front-libs.sh the front libs in build/graphics/guest/bin.
set -euo pipefail

OUT="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/Guest"
CONVERT="$SRCROOT/build/native/macos/importer/converter/apk-convert"
BIONIC="$SRCROOT/build/bionic"
FRONT="$SRCROOT/build/graphics/guest/bin"
FRONT_LIBS=(libEGL.so libGLESv2.so libGLESv3.so libvulkan.so)
for needed in "$CONVERT" "$BIONIC/bin/activity-host" "$BIONIC/lib/libc.so" "$BIONIC/lib/libandroid.so" \
              "$BIONIC/system/usr/share/zoneinfo/tzdata"; do
  if [[ ! -e "$needed" ]]; then
    echo "error: $needed is missing; tools/build-bionic.sh builds it" >&2
    exit 1
  fi
done
for library in "${FRONT_LIBS[@]}"; do
  if [[ ! -f "$FRONT/$library" ]]; then
    echo "error: $FRONT/$library is missing; tools/build-front-libs.sh builds it" >&2
    exit 1
  fi
done
case "$PLATFORM_NAME" in
  iphoneos) PLATFORM=ios ;;
  iphonesimulator) PLATFORM=ios-simulator ;;
  *) echo "error: no containers for $PLATFORM_NAME" >&2; exit 1 ;;
esac
rm -rf "$OUT"

# Converts the ELF file into <directory>/<file name>.dylib and signs it.
contain() {
  local container="$2/$(basename "$1").dylib"
  mkdir -p "$2"
  "$CONVERT" convert "$1" "$container" --platform "$PLATFORM" >/dev/null
  codesign --force --timestamp=none --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" \
    --identifier "$PRODUCT_BUNDLE_IDENTIFIER" "$container"
}

contain "$BIONIC/bin/activity-host" "$OUT/bin"
for library in "$BIONIC"/lib/*.so; do
  contain "$library" "$OUT/lib64"
done
for library in "${FRONT_LIBS[@]}"; do
  contain "$FRONT/$library" "$OUT/lib64"
done
mkdir -p "$OUT/system"
cp -R "$BIONIC/system/." "$OUT/system/"
echo "Embedded the guest system: $(find "$OUT" -name '*.dylib' | wc -l | tr -d ' ') signed containers for $PLATFORM"
