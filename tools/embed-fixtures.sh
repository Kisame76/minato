#!/usr/bin/env bash
# Xcode build phase (project.yml): puts the fixture tests of fixtures/tests.txt into the app bundle
# for tools/device-suite.sh (M2 step 6, M2a). In Fixtures/:
#   tests/<test>/   the program and its own libraries as container dylibs, as next to it on Android
#   elf/<test>/     the same as ELF files, which the app converts and signs itself with
#                   -signOnDevice (M2b, ADR-0022), as it will do with the libraries of an APK
#   system-lib64/   bionic's libraries, the guest's /system/lib64
#   system/         the guest's /system for files: bionic's time zone file
#   tests.txt       the test list, for the app's FixtureRunner
# Every container is signed with the app's identity and the bundle ID as identifier (ADR-0008).
# Only Debug builds with APK_EMBED_FIXTURES=YES get them; every other build removes them.
set -euo pipefail

OUT="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/Fixtures"
rm -rf "$OUT"
[[ "$CONFIGURATION" == "Debug" && "${APK_EMBED_FIXTURES:-NO}" == "YES" ]] || exit 0

CONVERT="$SRCROOT/build/native/macos/importer/converter/apk-convert"
FIXTURES="$SRCROOT/build/fixtures/bin"
BIONIC="$SRCROOT/build/bionic"
TESTS="$SRCROOT/fixtures/tests.txt"
for needed in "$CONVERT" "$FIXTURES" "$BIONIC/lib/libc.so" "$BIONIC/system/usr/share/zoneinfo/tzdata"; do
  if [[ ! -e "$needed" ]]; then
    echo "error: $needed is missing; tools/device-suite.sh builds it" >&2
    exit 1
  fi
done
case "$PLATFORM_NAME" in
  iphoneos) PLATFORM=ios ;;
  iphonesimulator) PLATFORM=ios-simulator ;;
  *) echo "error: no containers for $PLATFORM_NAME" >&2; exit 1 ;;
esac

# Converts the ELF file into <directory>/<file name>.dylib and signs it.
contain() {
  local container="$2/$(basename "$1").dylib"
  mkdir -p "$2"
  "$CONVERT" convert "$1" "$container" --platform "$PLATFORM" >/dev/null
  codesign --force --timestamp=none --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" \
    --identifier "$PRODUCT_BUNDLE_IDENTIFIER" "$container"
}

for library in "$BIONIC"/lib/*.so; do
  contain "$library" "$OUT/system-lib64"
done
mkdir -p "$OUT/system/usr/share/zoneinfo"
cp "$BIONIC/system/usr/share/zoneinfo/tzdata" "$OUT/system/usr/share/zoneinfo/"
cp "$TESTS" "$OUT/tests.txt"

while read -r name fields; do
  program=$name
  libraries=""
  for field in $fields; do
    case "$field" in
      program=*) program=${field#program=} ;;
      libraries=*) libraries=${field#libraries=} ;;
    esac
  done
  mkdir -p "$OUT/elf/$name"
  contain "$FIXTURES/$program" "$OUT/tests/$name"
  cp "$FIXTURES/$program" "$OUT/elf/$name/"
  for library in ${libraries//,/ }; do
    contain "$FIXTURES/$library" "$OUT/tests/$name"
    cp "$FIXTURES/$library" "$OUT/elf/$name/"
  done
done < <(grep -v -e '^#' -e '^$' "$TESTS")
echo "Embedded $(find "$OUT" -name '*.dylib' | wc -l | tr -d ' ') signed containers for $PLATFORM"
