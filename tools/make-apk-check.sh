#!/usr/bin/env bash
# Checks make-apk from end to end (ADR-0031 c): packs three sample apps with tools/make-apk.sh and
# checks each signature with tools/apk-check.py. With --android it also installs them on the running
# Android emulator, compares what `dumpsys package` read with their manifests, and uninstalls them.
#   manifest-only     a manifest without code and without a library
#   stored-library    a NativeActivity with extractNativeLibs="false": a stored library aligned to
#                     16 KiB, which Android refuses when it is compressed or not page-aligned, assets,
#                     and screenOrientation="sensorLandscape"
#   deflated-library  a NativeActivity with extractNativeLibs="true": a deflated library, assets, and
#                     screenOrientation="reversePortrait"
# The library is build/fixtures/bin/libraw-dyn.so from tools/build-fixtures.sh; Android only installs it.
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

if [[ ! -f "$LIBRARY" ]]; then
  echo "No $LIBRARY: run tools/build-fixtures.sh first." >&2
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

# sample <name> <package> <versionCode> <extractNativeLibs, or empty> <library: yes or no>
#        [<screenOrientation> <its value in ActivityInfo>]
sample() {
  local name=$1 package=$2 version=$3 extract=$4 library=$5 orientation=${6:-} orientation_value=${7:-}
  local folder="$OUT/$name" apk="$OUT/$name.apk" attribute="" activity=""
  mkdir -p "$folder/assets/sub"
  [[ -z "$extract" ]] || attribute=" android:extractNativeLibs=\"$extract\""
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
  <application android:label="make-apk check $name" android:hasCode="false"$attribute>
$activity
  </application>
</manifest>
EOF
  printf 'hello\n' >"$folder/assets/hello.txt"
  printf 'stored as aapt2 stores a .png\n' >"$folder/assets/sub/image.png"

  local arguments=(--manifest "$folder/AndroidManifest.xml" --out "$apk")
  [[ "$library" == yes ]] && arguments+=(--lib "$LIBRARY" --assets "$folder/assets")
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
  elif [[ "$flags" == *HAS_CODE* ]]; then
    problems+=" dumpsys package shows HAS_CODE, although hasCode is false;"
  fi
  # ActivityInfo.dump prints screenOrientation when it is not unspecified.
  if [[ "$library" == yes ]]; then
    local resolved
    resolved=$(device shell cmd package resolve-activity -a android.intent.action.MAIN \
      -c android.intent.category.LAUNCHER "$package" | tr -d '\r') || true
    printf '%s\n' "$resolved" >"$OUT/$name.resolve"
    [[ "$resolved" == *"screenOrientation=$orientation_value "* ]] \
      || problems+=" the launcher activity lacks screenOrientation=$orientation_value;"
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

sample manifest-only io.github.kisame76.minato.check.manifest 1 "" no
sample stored-library io.github.kisame76.minato.check.stored 2 false yes sensorLandscape 6
sample deflated-library io.github.kisame76.minato.check.deflated 3 true yes reversePortrait 9

if [[ $failed -gt 0 ]]; then
  echo "make-apk check: $failed of 3 samples failed"
  exit 1
fi
echo "make-apk check: 3 of 3 samples fine${android:+ on Android}"
