#!/usr/bin/env bash
# Test 2 with Godot (ADR-0042 a): exports fixtures/apps/godot-bench as build/godot/godot-bench-vulkan.apk (the
# Mobile renderer on Vulkan) and build/godot/godot-bench-gles.apk (the Compatibility renderer on GLES 3.0).
# The editor, its templates and the SDK folder come from tools/fetch-godot.py --fetch, the JDK from ART's fetch.
# The editor runs headless on a copy of the project in build/godot/project/, where Godot keeps its import cache
# and tools/godot-icons.py writes the launcher icons, so the repo's project stays as it is. The release export
# needs a keystore: the JDK's keytool makes a debug one once, in build/godot/keystore/, and Godot's environment
# variables name it. tools/godot-check.py export checks the APKs.
# With --twin it builds the twin instead (ADR-0042 decision 3): ios.zip's release libgodot.a with our wrapper
# fixtures/apps/godot-bench/twin/minato_godot.mm, linked against ADR-0030's MoltenVK 1.4.2 into
# build/MinatoGodot.xcframework, for iOS devices only: the template's Simulator library is x86_64 alone.
# tools/godot-check.py twin checks it.
# Usage: tools/build-godot.sh                    both APKs
#        tools/build-godot.sh --run <bench.conf>  the project headless on the Mac with that bench.conf, writing
#                                                 scene.csv and done beside it (tools/godot-check.py script)
#        tools/build-godot.sh --twin              the twin's framework
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GODOT="$ROOT/build/godot"
EDITOR="$GODOT/editor/Godot.app/Contents/MacOS/Godot"
PROJECT="$GODOT/project"
KEYSTORE="$GODOT/keystore/debug.keystore"
export JAVA_HOME="$ROOT/build/tools/jdk-17/jdk-17.0.20.1+1/Contents/Home"
export ANDROID_HOME="$GODOT/sdk"

# -dead_strip, Xcode's default for iOS, drops dead functions of the template's SDL that name symbols defined
# nowhere. -ObjC and -all_load stay off: they would pull in app.swift's @main and SwiftUI. strip -x drops the
# local symbols, about a quarter of the binary.
twin() {
  local templates="$GODOT/editor/editor_data/export_templates/4.7.2.stable" work="$GODOT/twin"
  local moltenvk="$ROOT/build/MoltenVK.xcframework/ios-arm64" source="$ROOT/fixtures/apps/godot-bench/twin"
  local framework="$GODOT/twin/MinatoGodot.framework" library="libgodot.ios.release.xcframework/ios-arm64/libgodot.a"
  for needed in "$templates/ios.zip" "$moltenvk/MoltenVK.framework/MoltenVK"; do
    [[ -f "$needed" ]] || { echo "build-godot: missing $needed: run tools/fetch-godot.py --fetch and tools/fetch-moltenvk.sh --fetch" >&2; exit 1; }
  done
  rm -rf "$work"
  mkdir -p "$framework"
  unzip -q "$templates/ios.zip" "$library" -d "$work"
  xcrun --sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=26.0 -std=c++17 -fobjc-arc -fvisibility=hidden \
    -O2 -Wall -Wextra -Werror -c "$source/minato_godot.mm" -o "$work/minato_godot.o"
  xcrun --sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=26.0 -dynamiclib \
    -install_name @rpath/MinatoGodot.framework/MinatoGodot -o "$framework/MinatoGodot" "$work/minato_godot.o" \
    "$work/$library" -F"$moltenvk" -framework MoltenVK -Wl,-exported_symbols_list,"$source/exports.txt" -Wl,-dead_strip
  xcrun strip -x "$framework/MinatoGodot"
  plutil -convert binary1 -o "$framework/Info.plist" "$source/Info.plist"
  rm -rf "$work/libgodot.ios.release.xcframework" "$ROOT/build/MinatoGodot.xcframework"
  xcodebuild -create-xcframework -framework "$framework" -output "$ROOT/build/MinatoGodot.xcframework" >/dev/null
  echo "build-godot: $ROOT/build/MinatoGodot.xcframework ($(stat -f %z "$framework/MinatoGodot") bytes)"
}

if [[ $# -eq 1 && "$1" == --twin ]]; then
  twin
  exit 0
fi

for needed in "$EDITOR" "$ANDROID_HOME/build-tools/35.0.1/apksigner" "$ANDROID_HOME/platform-tools/adb"; do
  [[ -x "$needed" ]] || { echo "build-godot: missing $needed: run tools/fetch-godot.py --fetch" >&2; exit 1; }
done
[[ -x "$JAVA_HOME/bin/keytool" ]] || { echo "build-godot: missing the JDK in $JAVA_HOME: run tools/fetch-sources.py --set art" >&2; exit 1; }

# The copy: Godot's .godot/ cache stays between runs; everything else follows the repo's project.
mkdir -p "$PROJECT"
rsync -a --delete --exclude .godot --exclude icons --exclude twin "$ROOT/fixtures/apps/godot-bench/" "$PROJECT/"
/usr/bin/python3 "$ROOT/tools/godot-icons.py" "$PROJECT/icons"
"$EDITOR" --headless --path "$PROJECT" --import >"$GODOT/import.log" 2>&1 || {
  tail -20 "$GODOT/import.log" >&2
  echo "build-godot: the import failed, see $GODOT/import.log" >&2
  exit 1
}

if [[ $# -eq 2 && "$1" == --run ]]; then
  exec "$EDITOR" --headless --path "$PROJECT" -- "--bench-config=$2"
elif [[ $# -ne 0 ]]; then
  echo "usage: tools/build-godot.sh [--run <bench.conf> | --twin]" >&2
  exit 2
fi

if [[ ! -f "$KEYSTORE" ]]; then
  mkdir -p "$(dirname "$KEYSTORE")"
  "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" -noprompt
fi
export GODOT_ANDROID_KEYSTORE_RELEASE_PATH="$KEYSTORE"
export GODOT_ANDROID_KEYSTORE_RELEASE_USER=androiddebugkey
export GODOT_ANDROID_KEYSTORE_RELEASE_PASSWORD=android

for api in vulkan gles; do
  case $api in
    vulkan) preset="Android Vulkan" ;;
    gles) preset="Android GLES" ;;
  esac
  apk="$GODOT/godot-bench-$api.apk"
  rm -f "$apk"
  if ! "$EDITOR" --headless --path "$PROJECT" --export-release "$preset" "$apk" >"$GODOT/export-$api.log" 2>&1 ||
     [[ ! -f "$apk" ]]; then
    tail -20 "$GODOT/export-$api.log" >&2
    echo "build-godot: the export of $preset failed, see $GODOT/export-$api.log" >&2
    exit 1
  fi
  echo "build-godot: $apk ($(stat -f %z "$apk") bytes)"
done
