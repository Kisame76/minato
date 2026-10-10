#!/usr/bin/env bash
# Xcode build phase (project.yml): puts the guest system into every app build (ADR-0028, decision 1 and its
# consequences; ADR-0032, decision 10), as game mode mounts it for activity-host and, for a Java app, app_process64
# (ADR-0039 c4). tools/guest-check.py checks the result. In Guest/:
#   bin/<program>.dylib       the programs /system/bin/activity-host and /system/bin/app_process64
#   lib64/<library>.so.dylib  the guest's /system/lib64: bionic, liblog, libnetd_client, libandroid,
#                             libnativewindow, libaaudio (ADR-0036) and libz, and the graphics front libs
#                             libEGL, libGLESv1_CM (ADR-0041 a1), libGLESv2, libGLESv3 and libvulkan (ADR-0029,
#                             ADR-0030); for the Java start libandroid_runtime.so with libsqlite.so (the compatibility
#                             plan's gap 7), hwui's set (ADR-0043) and ART's copies of libbase, libziparchive, libexpat
#                             and libc++_shared that it links
#   system/                   the guest's /system for files: bionic's time zone file, public.libraries.txt
#                             (ADR-0040), and the fonts with their two lists (ADR-0043 test 2); framework.jar and
#                             the framework-res folder in framework/ (ADR-0039, decision 8), platform.xml in
#                             etc/permissions/, and etc/security/cacerts empty: the runtime links that name to
#                             Conscrypt's certificates (ADR-0046 decision 4)
#   apex/                     the guest's /apex for files (ADR-0033 d2): ART's, Conscrypt's, ICU's and the time
#                             zones' APEX folders as ART and ICU look for them, with the boot jars in javalib/, ICU's
#                             data, the tzdata module's files (the compatibility plan's gap 21), and Conscrypt's CA
#                             certificates in its cacerts/ (ADR-0046 decision 4)
#   apex-lib64/<APEX>/        the APEXes' lib64: ART's release flavor in com.android.art, libjavacrypto in
#                             com.android.conscrypt
#   resource-cache/           Monet's overlays (ADR-0043 f)), the fabricated overlays of
#                             framework/monet-overlays.txt and their idmaps, made from this build's framework-res:
#                             a Java app's /data/resource-cache (GuestStart.swift)
# Every container is converted at build time and signed with the app's identity and the bundle ID as
# identifier (route 1 of ADR-0008). tools/build-bionic.sh builds what it takes from build/bionic,
# tools/build-front-libs.sh the front libs in build/graphics/guest/bin, tools/build-art.sh ART in build/art,
# tools/build-framework-native.sh app_process64, libandroid_runtime.so and libsqlite.so, tools/build-framework.sh
# framework.jar, tools/build-framework-res.sh the framework-res folder and tools/build-hwui.sh hwui's set;
# platform.xml comes with tools/fetch-sources.py --set framework, the CA certificates with --set art.
set -euo pipefail

OUT="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/Guest"
CONVERT="$SRCROOT/build/native/macos/importer/converter/apk-convert"
BIONIC="$SRCROOT/build/bionic"
FRONT="$SRCROOT/build/graphics/guest/bin"
FRONT_LIBS=(libEGL.so libGLESv1_CM.so libGLESv2.so libGLESv3.so libvulkan.so)
# ART's build: build/art, or the build setting APK_ART_DIR, which tools/device-suite.sh --art-dir passes.
ART="${APK_ART_DIR:-$SRCROOT/build/art}"
ICU_DATA="$SRCROOT/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
CA_CERTIFICATES="$SRCROOT/build/sources/ca-certificates/files"
# ART's debug flavor, which only fixture builds carry (tools/embed-fixtures.sh), and Conscrypt's library.
ART_DEBUG_LIBS=(libartd.so libartbased.so libdexfiled.so libopenjdkd.so libopenjdkjvmd.so libprofiled.so)
CONSCRYPT_LIB=libjavacrypto.so
# The Java start's parts (ADR-0039 c4): the framework's, hwui's set, and the libraries of ART that hwui and
# libandroid_runtime.so link from /system/lib64 (graphics/tests/hwui_load_run.py's PLATFORM_COPIES).
FRAMEWORK_NATIVE="$SRCROOT/build/framework-native"
FRAMEWORK_JAR="$SRCROOT/build/framework/framework.jar"
FRAMEWORK_RES="$SRCROOT/build/framework-res/framework-res"
PLATFORM_XML="$SRCROOT/build/sources/frameworks_base_etc/data/etc/platform.xml"
HWUI="$SRCROOT/build/hwui/lib"
ART_COPIES=(libbase.so libziparchive.so libexpat.so libc++_shared.so)
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
if [[ ! -d "$CA_CERTIFICATES" ]]; then
  echo "error: $CA_CERTIFICATES is missing; tools/fetch-sources.py --set art fetches it" >&2
  exit 1
fi
for needed in "$FRAMEWORK_NATIVE/app_process64:tools/build-framework-native.sh" \
              "$FRAMEWORK_NATIVE/libandroid_runtime.so:tools/build-framework-native.sh" \
              "$FRAMEWORK_NATIVE/libsqlite.so:tools/build-framework-native.sh" \
              "$FRAMEWORK_JAR:tools/build-framework.sh" "$FRAMEWORK_RES/resources.arsc:tools/build-framework-res.sh" \
              "$HWUI/libhwui.so:tools/build-hwui.sh" "$PLATFORM_XML:tools/fetch-sources.py --set framework"; do
  if [[ ! -f "${needed%%:*}" ]]; then
    echo "error: ${needed%%:*} is missing; ${needed#*:} makes it" >&2
    exit 1
  fi
done
for library in "${ART_COPIES[@]}"; do
  if [[ ! -f "$ART/lib/$library" ]]; then
    echo "error: $ART/lib/$library is missing; tools/build-art.sh builds it" >&2
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

# The Java start (ADR-0039 c4), as framework/tests/app_process_run.py lays it out on the Mac.
contain "$FRAMEWORK_NATIVE/app_process64" "$OUT/bin"
contain "$FRAMEWORK_NATIVE/libandroid_runtime.so" "$OUT/lib64"
contain "$FRAMEWORK_NATIVE/libsqlite.so" "$OUT/lib64"
for library in "$HWUI"/*.so; do
  contain "$library" "$OUT/lib64"
done
for library in "${ART_COPIES[@]}"; do
  contain "$ART/lib/$library" "$OUT/lib64"
done
mkdir -p "$OUT/system/framework" "$OUT/system/etc/permissions"
cp "$FRAMEWORK_JAR" "$OUT/system/framework/"
cp -R "$FRAMEWORK_RES" "$OUT/system/framework/"
cp "$PLATFORM_XML" "$OUT/system/etc/permissions/"
# Monet's overlays from the same framework-res (ADR-0043 f)), as SystemUI and idmap2 keep them on Android 15.
python3 "$SRCROOT/tools/monet-overlays.py" "$(dirname "$FRAMEWORK_RES")" "$OUT/resource-cache" >/dev/null

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
# The tzdata module as Android 15's com.android.tzdata (the compatibility plan's gap 21): bionic's tzdata and its
# tz_version at the top and in versioned/8, where libcore reads them; there too the tag's lookups, and ICU's time
# zone resources of the same release in icu/: one IANA release for bionic, libcore and ICU (ADR-0033 decision 9).
TZ_MODULE="$APEX/com.android.tzdata/etc/tz"
TZDATA="$BIONIC/system/usr/share/zoneinfo/tzdata"
TIMEZONE_DATA="$SRCROOT/build/sources/timezone/output_data/android"
release=$(head -c 12 "$TZDATA" | tail -c 6 | tr -d '\0')
ICU_TZDATA="$SRCROOT/build/sources/icu_tzdata/tzdata/icunew/$release/44/le"
if [[ ! -d "$ICU_TZDATA" || ! -f "$TIMEZONE_DATA/tzlookup.xml" ]]; then
  echo "error: $ICU_TZDATA or $TIMEZONE_DATA/tzlookup.xml is missing; tools/fetch-sources.py --set art fetches them." \
    "Bionic's tzdata is of $release: a newer release needs icu-data's files pinned in third_party/sources.json" >&2
  exit 1
fi
mkdir -p "$TZ_MODULE/versioned/8/icu"
for place in "$TZ_MODULE" "$TZ_MODULE/versioned/8"; do
  cp "$TZDATA" "$place/"
  printf '008.001|%s|001' "$release" > "$place/tz_version"
done
cp "$TIMEZONE_DATA/tzlookup.xml" "$TIMEZONE_DATA/telephonylookup.xml" "$TZ_MODULE/versioned/8/"
for name in zoneinfo64 metaZones timezoneTypes windowsZones; do
  cp "$ICU_TZDATA/$name.res" "$TZ_MODULE/versioned/8/icu/"
done
for library in "$ART"/lib/*.so; do
  name=$(basename "$library")
  if [[ "$name" == "$CONSCRYPT_LIB" ]]; then
    contain "$library" "$OUT/apex-lib64/com.android.conscrypt"
  elif [[ " ${ART_DEBUG_LIBS[*]} " != *" $name "* ]]; then
    contain "$library" "$OUT/apex-lib64/com.android.art"
  fi
done
# Conscrypt's CA certificates, once (ADR-0046 decision 4): /system/etc/security/cacerts is the runtime's link to
# them, and its empty folder keeps that name in /system/etc/security's listing.
mkdir -p "$APEX/com.android.conscrypt/cacerts" "$OUT/system/etc/security/cacerts"
cp "$CA_CERTIFICATES"/* "$APEX/com.android.conscrypt/cacerts/"
echo "Embedded the guest system: $(find "$OUT" -name '*.dylib' | wc -l | tr -d ' ') signed containers for $PLATFORM"
