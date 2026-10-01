#!/usr/bin/env bash
# Checks make-apk from end to end (ADR-0031 c): packs five sample apps with tools/make-apk.sh and
# checks each signature with tools/apk-check.py. With --android it also installs them on the running
# Android emulator, compares what `dumpsys package` read with their manifests, and uninstalls them.
#   manifest-only     a manifest without code and without a library
#   stored-library    a NativeActivity with extractNativeLibs="false": a stored library aligned to
#                     16 KiB, which Android refuses when it is compressed or not page-aligned, assets,
#                     and screenOrientation="sensorLandscape"
#   deflated-library  a NativeActivity with extractNativeLibs="true": a deflated library, assets, and
#                     screenOrientation="reversePortrait"
#   java-code         the stored library's app with Java code (ADR-0039, decision 11): hasCode left
#                     out, so true, and a classes.dex that javac and D8 make from one class against the
#                     boot jars, never android.jar; Android's install verifies the DEX file
#   resources         the stored library's app with resources (ADR-0039, decision 11): aapt2 compiles
#                     tools/aapt2/check/res and links it against our framework-res, and make-apk packs
#                     its output with --resources; the label is @string/app_name, which Android resolves
#                     through the table
# The library is build/fixtures/bin/libraw-dyn.so from tools/build-fixtures.sh; Android only installs it.
# The JDK and D8 come from tools/fetch-sources.py --set art, the boot jars' classes from tools/build-art.sh,
# aapt2 from tools/build-aapt2.sh and framework-res from tools/build-framework-res.sh.
# Usage: tools/make-apk-check.sh [--android]
# Results: build/make-apk-check/<sample>.apk, and with --android <sample>.dumpsys and <sample>.resolve
# Optional environment:
#   ANDROID_HOME  SDK with platform-tools, default: ~/Library/Android/sdk
set -euo pipefail
cd "$(dirname "$0")/.."

android=""
case "${1:-}" in
  "") ;;
  --android) android=yes ;;
  *) echo "Usage: tools/make-apk-check.sh [--android]" >&2; exit 64 ;;
esac

SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ADB="$SDK/platform-tools/adb"
LIBRARY=build/fixtures/bin/libraw-dyn.so
OUT=build/make-apk-check
JDK=$(echo build/tools/jdk-17/*/Contents/Home)
R8=build/sources/r8/r8.jar
BOOT_CLASSES=build/art/java/all-classes.jar
AAPT2=build/aapt2/aapt2
FRAMEWORK_RES=build/framework-res/framework-res.apk

if [[ ! -f "$LIBRARY" ]]; then
  echo "No $LIBRARY: run tools/build-fixtures.sh first." >&2
  exit 1
fi
if [[ ! -x "$JDK/bin/javac" || ! -f "$R8" ]]; then
  echo "No JDK 17 in build/tools/jdk-17 or no D8 in $R8: run tools/fetch-sources.py --set art." >&2
  exit 1
fi
if [[ ! -f "$BOOT_CLASSES" ]]; then
  echo "No $BOOT_CLASSES: run tools/build-art.sh first." >&2
  exit 1
fi
if [[ ! -x "$AAPT2" || ! -f "$FRAMEWORK_RES" ]]; then
  echo "No $AAPT2 or $FRAMEWORK_RES: run tools/build-aapt2.sh and tools/build-framework-res.sh first." >&2
  exit 1
fi
serial=""
if [[ -n "$android" ]]; then
  if [[ ! -x "$ADB" ]]; then
    echo "No adb at $ADB: install platform-tools into $SDK (docs/STATUS.md)." >&2
    exit 1
  fi
  if ! "$ADB" start-server >/dev/null 2>&1; then
    echo "adb start-server failed ($ADB)." >&2
    exit 1
  fi
  serial=$("$ADB" devices | awk '$1 ~ /^emulator-/ && $2 == "device" { print $1; exit }')
  if [[ -z "$serial" ]]; then
    echo "No running emulator. The emulator is shared: ask the lead, then start it, for example with" >&2
    echo "  $SDK/emulator/emulator -avd apk-runner-35 -port 5554 -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect &" >&2
    exit 1
  fi
fi
# Never with the terminal as stdin.
device() { "$ADB" -s "$serial" "$@" </dev/null; }

rm -rf "$OUT"
mkdir -p "$OUT"
failed=0

# The classes.dex of the java-code sample in <folder>/dex: javac against the boot jars' classes, then D8.
dex() {
  local folder=$1
  mkdir -p "$folder/src" "$folder/classes" "$folder/dex"
  cat >"$folder/src/Hello.java" <<'JAVA'
package io.github.kisame76.minato.check.code;

public final class Hello {
    public static String greeting() {
        return "hello from classes.dex";
    }
}
JAVA
  "$JDK/bin/javac" -source 8 -target 8 -bootclasspath "$BOOT_CLASSES" -nowarn -d "$folder/classes" \
    "$folder/src/Hello.java" 2>/dev/null \
    && "$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
      --output "$folder/dex" "$folder/classes/io/github/kisame76/minato/check/code/Hello.class"
}

# sample <name> <package> <versionCode> <extractNativeLibs, or empty> <library: yes or no> <code: yes or no>
#        <resources: yes or no> [<screenOrientation> <its value in ActivityInfo>]
sample() {
  local name=$1 package=$2 version=$3 extract=$4 library=$5 code=$6 resources=$7 orientation=${8:-}
  local orientation_value=${9:-}
  local folder="$OUT/$name" apk="$OUT/$name.apk" attribute="" activity="" has_code=' android:hasCode="false"'
  local label="make-apk check $name" label_id=""
  [[ "$resources" == no ]] || label="@string/app_name"
  mkdir -p "$folder/assets/sub"
  [[ -z "$extract" ]] || attribute=" android:extractNativeLibs=\"$extract\""
  # Left out, hasCode is true.
  [[ "$code" == no ]] || has_code=""
  if [[ "$library" == yes ]]; then
    activity="    <activity android:name=\"android.app.NativeActivity\" android:exported=\"true\" android:screenOrientation=\"$orientation\">"
    activity+='
      <meta-data android:name="android.app.lib_name" android:value="raw-dyn"/>
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>'
  fi
  cat >"$folder/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$package" android:versionCode="$version" android:versionName="1.$version">
  <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
  <application android:label="$label"$has_code$attribute>
$activity
  </application>
</manifest>
EOF
  printf 'hello\n' >"$folder/assets/hello.txt"
  printf 'stored as aapt2 stores a .png\n' >"$folder/assets/sub/image.png"

  local arguments=(--manifest "$folder/AndroidManifest.xml" --out "$apk")
  if [[ "$resources" == yes ]]; then
    # aapt2's output in place of the manifest, linked against our framework-res.
    if ! "$AAPT2" compile --dir tools/aapt2/check/res -o "$folder/compiled.zip" \
        || ! "$AAPT2" link -o "$folder/linked.apk" -I "$FRAMEWORK_RES" --manifest "$folder/AndroidManifest.xml" \
          "$folder/compiled.zip"; then
      echo "FAILED   $name: aapt2"
      failed=$((failed + 1))
      return
    fi
    label_id=$("$AAPT2" dump resources "$folder/linked.apk" | awk '$3 == "string/app_name" { print $2 }')
    arguments=(--resources "$folder/linked.apk" --out "$apk")
  fi
  [[ "$library" == yes ]] && arguments+=(--lib "$LIBRARY" --assets "$folder/assets")
  if [[ "$code" == yes ]]; then
    if ! dex "$folder"; then
      echo "FAILED   $name: javac or D8"
      failed=$((failed + 1))
      return
    fi
    arguments+=(--dex "$folder/dex/classes.dex")
  fi
  if ! tools/make-apk.sh "${arguments[@]}"; then
    echo "FAILED   $name: make-apk"
    failed=$((failed + 1))
    return
  fi
  local check
  check=$(python3 tools/apk-check.py "$apk" | tail -1) || true
  if [[ "$check" != "RESULT: signatures valid" ]]; then
    echo "FAILED   $name: tools/apk-check.py says: $check"
    failed=$((failed + 1))
    return
  fi
  [[ -n "$android" ]] || { echo "ok       $name: signatures valid"; return; }

  # A key made anew since the last run would make Android refuse the update.
  device uninstall "$package" >/dev/null 2>&1 || true
  local installed
  installed=$(device install "$apk" 2>&1 | tr -d '\r') || true
  if [[ "$installed" != *Success* ]]; then
    echo "FAILED   $name: adb install says: $installed"
    failed=$((failed + 1))
    return
  fi
  local dump problems=""
  dump=$(device shell dumpsys package "$package" | tr -d '\r') || true
  printf '%s\n' "$dump" >"$OUT/$name.dumpsys"
  # extractNativeLibs is true when the manifest leaves it out; apkSigningVersion=2 says Android checked v2.
  for expected in "versionCode=$version " "minSdk=29" "targetSdk=35" "versionName=1.$version" \
      "extractNativeLibs=${extract:-true}" "apkSigningVersion=2"; do
    [[ "$dump" == *"$expected"* ]] || problems+=" dumpsys package lacks $expected;"
  done
  if [[ "$library" == yes && "$dump" != *"primaryCpuAbi=arm64-v8a"* ]]; then
    problems+=" dumpsys package lacks primaryCpuAbi=arm64-v8a;"
  fi
  local flags
  flags=$(grep -E "^ +pkgFlags=" <<<"$dump") || true
  if [[ -z "$flags" ]]; then
    problems+=" dumpsys package has no pkgFlags line;"
  elif [[ "$code" == no && "$flags" == *HAS_CODE* ]]; then
    problems+=" dumpsys package shows HAS_CODE, although hasCode is false;"
  elif [[ "$code" == yes && "$flags" != *HAS_CODE* ]]; then
    problems+=" dumpsys package lacks HAS_CODE, although the app has classes.dex;"
  fi
  # ActivityInfo.dump prints screenOrientation when it is not unspecified.
  if [[ "$library" == yes ]]; then
    local resolved
    resolved=$(device shell cmd package resolve-activity -a android.intent.action.MAIN \
      -c android.intent.category.LAUNCHER "$package" | tr -d '\r') || true
    printf '%s\n' "$resolved" >"$OUT/$name.resolve"
    [[ "$resolved" == *"screenOrientation=$orientation_value "* ]] \
      || problems+=" the launcher activity lacks screenOrientation=$orientation_value;"
    # ApplicationInfo dumps a resource label as labelRes, and a plain one as nonLocalizedLabel.
    if [[ -n "$label_id" && "$resolved" != *"labelRes=$label_id "* ]]; then
      problems+=" the application lacks labelRes=$label_id, string/app_name;"
    fi
  fi
  local uninstalled
  uninstalled=$(device uninstall "$package" 2>&1 | tr -d '\r') || true
  [[ "$uninstalled" == *Success* ]] || problems+=" adb uninstall says: $uninstalled;"
  if [[ -n "$problems" ]]; then
    echo "FAILED   $name:$problems"
    failed=$((failed + 1))
  else
    echo "ok       $name: signatures valid, installed on $serial, dumpsys as in the manifest, uninstalled"
  fi
}

sample manifest-only io.github.kisame76.minato.check.manifest 1 "" no no no
sample stored-library io.github.kisame76.minato.check.stored 2 false yes no no sensorLandscape 6
sample deflated-library io.github.kisame76.minato.check.deflated 3 true yes no no reversePortrait 9
sample java-code io.github.kisame76.minato.check.code 4 false yes yes no sensorLandscape 6
sample resources io.github.kisame76.minato.check.resources 5 false yes no yes sensorLandscape 6

if [[ $failed -gt 0 ]]; then
  echo "make-apk check: $failed of 5 samples failed"
  exit 1
fi
echo "make-apk check: 5 of 5 samples fine${android:+ on Android}"
