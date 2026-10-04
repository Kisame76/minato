#!/usr/bin/env bash
# Xcode build phase (project.yml): puts the guest system into every app build (ADR-0028, decision 1 and its
# consequences; ADR-0032, decision 10), as game mode mounts it for activity-host. In Guest/:
#   bin/activity-host.dylib   the program /system/bin/activity-host
#   lib64/<library>.so.dylib  the guest's /system/lib64: bionic, liblog, libnetd_client, libandroid,
#                             libnativewindow, libaaudio (ADR-0036) and libz, and the graphics front libs
#                             libEGL, libGLESv2, libGLESv3 and libvulkan (ADR-0029, ADR-0030)
#   system/                   the guest's /system for files: bionic's time zone file, public.libraries.txt
#                             (ADR-0040), and the fonts with their two lists (ADR-0043 test 2)
#   apex/                     the guest's /apex for files (ADR-0033 d2): ART's, Conscrypt's, ICU's and the time
#                             zones' APEX folders as ART and ICU look for them, with the boot jars in javalib/, ICU's
#                             data and the time zone file
#   apex-lib64/<APEX>/        the APEXes' lib64: ART's release flavor in com.android.art, libjavacrypto in
#                             com.android.conscrypt
# Every container is converted at build time and signed with the app's identity and the bundle ID as
# identifier (route 1 of ADR-0008). tools/build-bionic.sh builds what it takes from build/bionic,
# tools/build-front-libs.sh the front libs in build/graphics/guest/bin, and tools/build-art.sh ART in build/art.
set -euo pipefail

OUT="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/Guest"
CONVERT="$SRCROOT/build/native/macos/importer/converter/apk-convert"
BIONIC="$SRCROOT/build/bionic"
FRONT="$SRCROOT/build/graphics/guest/bin"
FRONT_LIBS=(libEGL.so libGLESv2.so libGLESv3.so libvulkan.so)
# ART's build: build/art, or the build setting APK_ART_DIR, which tools/device-suite.sh --art-dir passes.
ART="${APK_ART_DIR:-$SRCROOT/build/art}"
ICU_DATA="$SRCROOT/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
# ART's debug flavor, which only fixture builds carry (tools/embed-fixtures.sh), and Conscrypt's library.
ART_DEBUG_LIBS=(libartd.so libartbased.so libdexfiled.so libopenjdkd.so libopenjdkjvmd.so libprofiled.so)
CONSCRYPT_LIB=libjavacrypto.so
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
for needed in "$ART/lib/libart.so" "$ART/lib/$CONSCRYPT_LIB" "$ART/javalib/core-oj.jar" "$ART/javalib/conscrypt.jar"; do
  if [[ ! -e "$needed" ]]; then
    echo "error: $needed is missing; tools/build-art.sh builds it" >&2
    exit 1
  fi
done
if [[ ! -f "$ICU_DATA" ]]; then
  echo "error: $ICU_DATA is missing; tools/fetch-sources.py --set art fetches it" >&2
  exit 1
fi
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

# ART's APEXes (ADR-0033 d2), as runtime/tests/art_run.py lays them out on the Mac: the folders ART and ICU look
# for, bin/ and lib64/ empty in the files, since their programs and libraries are containers.
APEX="$OUT/apex"
mkdir -p "$APEX/com.android.art/bin" "$APEX/com.android.art/lib64" "$APEX/com.android.art/javalib" \
  "$APEX/com.android.conscrypt/lib64" "$APEX/com.android.conscrypt/javalib" "$APEX/com.android.i18n/etc/icu" \
  "$APEX/com.android.tzdata/etc/tz"
for jar in "$ART"/javalib/*.jar; do
  if [[ "$(basename "$jar")" == conscrypt.jar ]]; then
    cp "$jar" "$APEX/com.android.conscrypt/javalib/"
  else
    cp "$jar" "$APEX/com.android.art/javalib/"
  fi
done
cp "$ICU_DATA" "$APEX/com.android.i18n/etc/icu/"
cp "$BIONIC/system/usr/share/zoneinfo/tzdata" "$APEX/com.android.tzdata/etc/tz/"
for library in "$ART"/lib/*.so; do
  name=$(basename "$library")
  if [[ "$name" == "$CONSCRYPT_LIB" ]]; then
    contain "$library" "$OUT/apex-lib64/com.android.conscrypt"
  elif [[ " ${ART_DEBUG_LIBS[*]} " != *" $name "* ]]; then
    contain "$library" "$OUT/apex-lib64/com.android.art"
  fi
done
echo "Embedded the guest system: $(find "$OUT" -name '*.dylib' | wc -l | tr -d ' ') signed containers for $PLATFORM"
