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
# Four split sets follow (ADR-0048 b8), each APK packed and signature-checked; with --android each set is
# installed as one session through `adb install-multiple`, so that every split attribute has an effect
# Android checks:
#   split-required    a NativeActivity base with isSplitRequired and its library in config.arm64_v8a: alone
#                     Android refuses it (INSTALL_FAILED_MISSING_SPLIT), with the split it installs
#   split-types       a base with requiredSplitTypes="base__abi": with only config.de Android refuses it, with
#                     config.arm64_v8a (splitTypes="base__abi") it installs, and config.de joins it with -p
#   isolated-splits   a base with isolatedSplits, a feature split with isFeatureSplit and a config split for it
#                     (configForSplit) install; a config split for a missing feature is refused
#                     (INSTALL_PARSE_FAILED_BAD_MANIFEST)
#   verity            config.de signed again by tools/sign-verity.py, with the base's key but only over a
#                     verity digest: Android installs the set, and Minato verifies the base and not the split
# The library is build/fixtures/bin/libraw-dyn.so from tools/build-fixtures.sh; Android only installs it.
# The JDK and D8 come from tools/fetch-sources.py --set art, the boot jars' classes from tools/build-art.sh,
# aapt2 from tools/build-aapt2.sh and framework-res from tools/build-framework-res.sh.
# Usage: tools/make-apk-check.sh [--android]
# Results: build/make-apk-check/<sample>.apk and <set>/*.apk, and with --android <sample>.dumpsys,
# <sample>.resolve and <set>.dumpsys
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
    echo "No adb at $ADB: install platform-tools into $SDK." >&2
    exit 1
  fi
  if ! "$ADB" start-server >/dev/null 2>&1; then
    echo "adb start-server failed ($ADB)." >&2
    exit 1
  fi
  serial=$("$ADB" devices | awk '$1 ~ /^emulator-/ && $2 == "device" { print $1; exit }')
  if [[ -z "$serial" ]]; then
    echo "No running emulator. The emulator is shared: ask whoever uses it, then start it, for example with" >&2
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

# --- Split sets (ADR-0048 b8) ---

NATIVE_ACTIVITY='
    <activity android:name="android.app.NativeActivity" android:exported="true">
      <meta-data android:name="android.app.lib_name" android:value="raw-dyn"/>
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>'

# set_apk <set> <file> <manifest attributes> <application attributes> [<the application's children> [lib]]
# packs one APK of a set without code, with the library when the last argument is "lib", and checks its signature.
set_apk() {
  local set=$1 file=$2 attributes=$3 application=$4 children=${5:-} library=${6:-}
  local folder="$OUT/$set" package="io.github.kisame76.minato.check.${1//-/}"
  mkdir -p "$folder"
  cat >"$folder/${file%.apk}.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$package" android:versionCode="1" android:versionName="1.1"$attributes>
  <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
  <application android:hasCode="false"$application>$children
  </application>
</manifest>
EOF
  local arguments=(--manifest "$folder/${file%.apk}.xml" --out "$folder/$file")
  [[ "$library" != lib ]] || arguments+=(--lib "$LIBRARY")
  # Its stderr goes to a log, shown when it fails: SwiftPM's note that it waits for another build has no newline and
  # would run into a result line.
  if ! tools/make-apk.sh "${arguments[@]}" >/dev/null 2>"$folder/${file%.apk}.make-apk.log"; then
    cat "$folder/${file%.apk}.make-apk.log" >&2
    problems+=" make-apk refuses $file;"
    return 1
  fi
  local check
  check=$(python3 tools/apk-check.py "$folder/$file" | tail -1) || true
  [[ "$check" == "RESULT: signatures valid" ]] || { problems+=" tools/apk-check.py says of $file: $check;"; return 1; }
}

# Android's answer to one install session of these APKs and options: adb's last line that says "Success" or
# "Failure [...]", or all of adb's output when none does. adb writes "Performing Incremental Install" to stdout and a
# refusal to stderr, so the refusal need not come last.
session() {
  local output answer
  output=$(device install-multiple "$@" 2>&1 | tr -d '\r') || true
  answer=$(grep -E '^(Success|Failure)' <<<"$output" | tail -1) || true
  echo "${answer:-$output}"
}

# expect <what> <answer> <a part it must contain>
expect() { [[ "$2" == *"$3"* ]] || problems+=" $1: adb says \"$2\";"; }

# The installed package's dumpsys, kept as <set>.dumpsys, and its splits as Android lists them.
expect_splits() {
  local set=$1 package=$2 splits=$3 dump
  dump=$(device shell dumpsys package "$package" | tr -d '\r') || true
  printf '%s\n' "$dump" >"$OUT/$set.dumpsys"
  [[ "$dump" == *"splits=[$splits]"* ]] || problems+=" dumpsys package lacks splits=[$splits];"
}

uninstall() {
  local answer
  answer=$(device uninstall "$1" 2>&1 | tr -d '\r') || true
  [[ "$answer" == *Success* ]] || problems+=" adb uninstall says: $answer;"
}

# apk-inspect's signature check of an APK, as Minato's library reads it. Its build's output goes to a log, shown when
# the build fails, as make-apk's in set_apk.
signature_check() {
  local log="$OUT/apk-inspect.build.log"
  swift build -c release --package-path importer/APKKit --scratch-path build/make-apk --product apk-inspect -q \
    >"$log" 2>&1 || { cat "$log" >&2; return 1; }
  "$(swift build -c release --package-path importer/APKKit --scratch-path build/make-apk --show-bin-path 2>>"$log")/apk-inspect" \
    "$1" | sed -n 's/^signature check: //p'
}

split_required() {
  local set=split-required package=io.github.kisame76.minato.check.splitrequired folder="$OUT/split-required"
  set_apk $set base.apk ' android:isSplitRequired="true"' \
      ' android:label="make-apk check split-required" android:extractNativeLibs="false"' "$NATIVE_ACTIVITY" \
    && set_apk $set split_config.arm64_v8a.apk ' split="config.arm64_v8a"' ' android:extractNativeLibs="false"' "" lib \
    || return 0
  [[ -n "$android" ]] || return 0
  device uninstall "$package" >/dev/null 2>&1 || true
  expect "the base alone" "$(session "$folder/base.apk")" "INSTALL_FAILED_MISSING_SPLIT: Missing split for $package"
  expect "the base with its ABI split" "$(session "$folder/base.apk" "$folder/split_config.arm64_v8a.apk")" Success
  expect_splits $set $package "base, config.arm64_v8a"
  [[ "$(cat "$OUT/$set.dumpsys")" == *"primaryCpuAbi=arm64-v8a"* ]] || problems+=" dumpsys package lacks primaryCpuAbi=arm64-v8a;"
  local resolved
  resolved=$(device shell cmd package resolve-activity -a android.intent.action.MAIN \
    -c android.intent.category.LAUNCHER "$package" | tr -d '\r') || true
  [[ "$resolved" == *"name=android.app.NativeActivity"* ]] || problems+=" the launcher does not resolve to NativeActivity;"
  uninstall $package
}

split_types() {
  local set=split-types package=io.github.kisame76.minato.check.splittypes folder="$OUT/split-types"
  set_apk $set base.apk ' android:requiredSplitTypes="base__abi"' ' android:label="make-apk check split-types"' \
    && set_apk $set split_config.arm64_v8a.apk ' split="config.arm64_v8a" android:splitTypes="base__abi"' "" "" lib \
    && set_apk $set split_config.de.apk ' split="config.de"' "" \
    || return 0
  [[ -n "$android" ]] || return 0
  device uninstall "$package" >/dev/null 2>&1 || true
  expect "the base with config.de only" "$(session "$folder/base.apk" "$folder/split_config.de.apk")" \
    "INSTALL_FAILED_MISSING_SPLIT: Missing split for $package"
  expect "the base with its ABI split" "$(session "$folder/base.apk" "$folder/split_config.arm64_v8a.apk")" Success
  expect "config.de joining" "$(session -p "$package" "$folder/split_config.de.apk")" Success
  expect_splits $set $package "base, config.arm64_v8a, config.de"
  uninstall $package
}

isolated_splits() {
  local set=isolated-splits package=io.github.kisame76.minato.check.isolatedsplits folder="$OUT/isolated-splits"
  set_apk $set base.apk ' android:isolatedSplits="true"' ' android:label="make-apk check isolated-splits"' \
    && set_apk $set split_feature_extra.apk ' split="feature_extra" android:isFeatureSplit="true"' "" \
    && set_apk $set split_feature_extra.config.de.apk ' split="feature_extra.config.de" configForSplit="feature_extra"' "" \
    && set_apk $set split_feature_missing.config.de.apk \
      ' split="feature_missing.config.de" configForSplit="feature_missing"' "" \
    || return 0
  [[ -n "$android" ]] || return 0
  device uninstall "$package" >/dev/null 2>&1 || true
  local refused
  refused=$(session "$folder/base.apk" "$folder/split_feature_missing.config.de.apk")
  expect "a config split for a missing feature" "$refused" "INSTALL_PARSE_FAILED_BAD_MANIFEST"
  expect "a config split for a missing feature" "$refused" "targets split 'feature_missing', which is missing."
  expect "the feature split and its config split" \
    "$(session "$folder/base.apk" "$folder/split_feature_extra.apk" "$folder/split_feature_extra.config.de.apk")" Success
  expect_splits $set $package "base, feature_extra, feature_extra.config.de"
  uninstall $package
}

verity() {
  local set=verity package=io.github.kisame76.minato.check.verity folder="$OUT/verity"
  set_apk $set base.apk "" ' android:label="make-apk check verity"' \
    && set_apk $set split_config.de.apk ' split="config.de"' "" \
    || return 0
  if ! python3 tools/sign-verity.py "$folder/split_config.de.apk" "$folder/split_config.de.verity.apk" >/dev/null; then
    problems+=" tools/sign-verity.py fails;"
    return 0
  fi
  local base split
  base=$(signature_check "$folder/base.apk") || true
  split=$(signature_check "$folder/split_config.de.verity.apk") || true
  [[ "$base" == verified ]] || problems+=" Minato reads the base as $base;"
  [[ "$split" == 'unverified("signed with RSASSA-PKCS1-v1_5 over a verity digest")' ]] \
    || problems+=" Minato reads the verity split as $split;"
  [[ -n "$android" ]] || return 0
  device uninstall "$package" >/dev/null 2>&1 || true
  expect "the base with its verity split" "$(session "$folder/base.apk" "$folder/split_config.de.verity.apk")" Success
  expect_splits $set $package "base, config.de"
  uninstall $package
}

sets_failed=0
for set in split-required split-types isolated-splits verity; do
  problems=""
  "${set//-/_}"
  if [[ -n "$problems" ]]; then
    echo "FAILED   $set:$problems"
    sets_failed=$((sets_failed + 1))
  elif [[ -n "$android" ]]; then
    echo "ok       $set: signatures valid, installed on $serial as Android checks a set, uninstalled"
  else
    echo "ok       $set: signatures valid"
  fi
done

if [[ $failed -gt 0 || $sets_failed -gt 0 ]]; then
  echo "make-apk check: $failed of 5 samples and $sets_failed of 4 sets failed"
  exit 1
fi
echo "make-apk check: 5 of 5 samples and 4 of 4 sets fine${android:+ on Android}"
