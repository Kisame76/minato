#!/usr/bin/env bash
# Compares Minato's set checks with Android 15's (ADR-0048 b8c). Each case is one install session of APKs that
# make-apk packs: on the Mac, apk-inspect --set (or --join for splits added to an installed app) prints what
# SetChecks answers; with --android the same session goes to the running emulator through `adb install-multiple`
# (`-p <package>` for a join, after the installed APKs as a session of their own), and Android's answer is checked
# too. Each case names the answers it expects from both; three known differences name their decisions.
# Unsigned APKs are make-apk's with their signing block removed; tools/sign-verity.py signs a split over a verity
# digest only; another signer's key is made in the output folder and goes with it.
# Usage: tools/formats-reference.sh [--android]
# Optional environment:
#   ANDROID_HOME  SDK with platform-tools, default: ~/Library/Android/sdk
# Results: build/formats-reference/<case>/, and with --android <case>.answer, Android's answer
set -euo pipefail
cd "$(dirname "$0")/.."

android=""
case "${1:-}" in
  "") ;;
  --android) android=yes ;;
  *) echo "Usage: tools/formats-reference.sh [--android]" >&2; exit 64 ;;
esac

SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ADB="$SDK/platform-tools/adb"
OUT=build/formats-reference
PKG=io.github.kisame76.minato.check.reference
serial=""
if [[ -n "$android" ]]; then
  if [[ ! -x "$ADB" ]] || ! "$ADB" start-server >/dev/null 2>&1; then
    echo "No working adb at $ADB: install platform-tools into $SDK." >&2
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
# apk-inspect builds once, as make-apk's scratch path does; its output goes to a log, shown when it fails.
if ! swift build -c release --package-path importer/APKKit --scratch-path build/make-apk --product apk-inspect -q \
    >"$OUT/apk-inspect.build.log" 2>&1; then
  cat "$OUT/apk-inspect.build.log" >&2
  exit 1
fi
INSPECT="$(swift build -c release --package-path importer/APKKit --scratch-path build/make-apk --show-bin-path 2>/dev/null)/apk-inspect"

# apk <case>/<file> <manifest attributes> [<children of <manifest>> [<versionCode> [<package> [make-apk arguments]]]]
# packs one APK without code.
apk() {
  local path=$1 attributes=$2 children=${3:-} code=${4:-1} package=${5:-$PKG}
  shift $(($# < 5 ? $# : 5))
  mkdir -p "$OUT/$(dirname "$path")"
  cat >"$OUT/manifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$package" android:versionCode="$code" android:versionName="$code.0"$attributes>
  <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>$children
  <application android:hasCode="false"/>
</manifest>
EOF
  if ! tools/make-apk.sh --manifest "$OUT/manifest.xml" --out "$OUT/$path" "$@" >/dev/null 2>"$OUT/make-apk.log"; then
    cat "$OUT/make-apk.log" >&2
    echo "make-apk refuses $path" >&2
    exit 1
  fi
}

# Removes an APK's signing block, as an unsigned build leaves it: its entries, central directory and end record.
unsign() {
  python3 - "$OUT/$1" <<'EOF'
import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()
end = data.rindex(b"PK\x05\x06")
central = struct.unpack_from("<I", data, end + 16)[0]
if data[central - 16:central] != b"APK Sig Block 42":
    sys.exit("unsign: no signing block in " + path)
start = central - (struct.unpack_from("<Q", data, central - 24)[0] + 8)
entries = data[:start]
data = entries + data[central:end + 16] + struct.pack("<I", len(entries)) + data[end + 20:]
open(path, "wb").write(data)
EOF
}

# Signs a split again over a verity digest only (b8b): Android verifies it, Minato cannot.
verity() {
  python3 tools/sign-verity.py "$OUT/$1" "$OUT/$1.verity" >/dev/null
  mv "$OUT/$1.verity" "$OUT/$1"
}

# Android's answer to one install session: its last "Success" or "Failure [...]" line, or all of adb's output.
session() {
  local output answer
  output=$(device install-multiple "$@" 2>&1 | tr -d '\r') || true
  answer=$(grep -E '^(Success|Failure)' <<<"$output" | tail -1) || true
  echo "${answer:-$output}"
}

passed=0
failed=0
# reference <case> <a part of Android's answer> <Minato's answer> [<known difference>]
# A case is a folder of APKs for one session, or for a join installed/ (one session) and new/ (`-p`).
reference() {
  local name=$1 android_wants=$2 minato_wants=$3 known=${4:-} folder="$OUT/$1" minato android_says=""
  if [[ -d "$folder/installed" ]]; then
    minato=$("$INSPECT" --join "$folder"/installed/*.apk -- "$folder"/new/*.apk 2>&1) || true
  else
    minato=$("$INSPECT" --set "$folder"/*.apk 2>&1) || true
  fi
  local problems=""
  [[ "$minato" == "$minato_wants" ]] || problems+=" Minato says \"$minato\", not \"$minato_wants\";"
  if [[ -n "$android" ]]; then
    device uninstall "$PKG" >/dev/null 2>&1 || true
    if [[ -d "$folder/installed" ]]; then
      local first
      first=$(session "$folder"/installed/*.apk)
      [[ "$first" == Success ]] || problems+=" the installed APKs: Android says \"$first\";"
      android_says=$(session -p "$PKG" "$folder"/new/*.apk)
    else
      android_says=$(session "$folder"/*.apk)
    fi
    printf '%s\n' "$android_says" >"$OUT/$name.answer"
    [[ "$android_says" == *"$android_wants"* ]] || problems+=" Android says \"$android_says\", not \"…$android_wants…\";"
    device uninstall "$PKG" >/dev/null 2>&1 || true
  fi
  if [[ -n "$problems" ]]; then
    echo "FAILED   $name:$problems"
    failed=$((failed + 1))
    return
  fi
  passed=$((passed + 1))
  local said=${android:+"Android \"$android_says\", "}
  echo "ok       $name: ${said}Minato \"$minato\"${known:+ (known: $known)}"
}

BASE_ABI=' split="config.arm64_v8a" android:splitTypes="base__abi"'

# Sessions Android installs, and Minato too.
apk set/base.apk ''
apk set/split_config.arm64_v8a.apk "$BASE_ABI"
apk own-types-free/base.apk ''
apk own-types-free/split_config.de.apk ' split="config.de" android:requiredSplitTypes="base__density"'
apk isolated-fine/base.apk ' android:isolatedSplits="true"'
apk isolated-fine/split_feature_extra.apk ' split="feature_extra" android:isFeatureSplit="true"'
apk isolated-fine/split_feature_extra.config.de.apk ' split="feature_extra.config.de" configForSplit="feature_extra"'
# Without isolatedSplits Android builds no dependency tree, so a config split may name a missing feature.
apk not-isolated/base.apk ''
apk not-isolated/split_feature_missing.config.de.apk \
  ' split="feature_missing.config.de" configForSplit="feature_missing"'

# Missing splits: INSTALL_FAILED_MISSING_SPLIT, "Needs split APKs" in Minato.
apk required-alone/base.apk ' android:isSplitRequired="true"'
apk types-unmet/base.apk ' android:requiredSplitTypes="base__abi"'
apk types-unmet/split_config.de.apk ' split="config.de"'
apk own-types-required/base.apk ' android:isSplitRequired="true"'
apk own-types-required/split_config.de.apk ' split="config.de" android:requiredSplitTypes="base__density"'

# Sessions both refuse.
apk twice/base.apk ''
apk twice/split_config.de.apk ' split="config.de"'
apk twice/split_config.de-2.apk ' split="config.de"'
apk other-package/base.apk ''
apk other-package/split_config.de.apk ' split="config.de"' '' 1 "$PKG.other"
apk other-version/base.apk ''
apk other-version/split_config.de.apk ' split="config.de"' '' 2
apk other-signer/base.apk ''
apk other-signer/split_config.de.apk ' split="config.de"' '' 1 "$PKG" --key "$OUT/other-key"
apk no-base/split_config.de.apk ' split="config.de"'
apk unsigned-split/base.apk ''
apk unsigned-split/split_config.de.apk ' split="config.de"'
unsign unsigned-split/split_config.de.apk
# Isolated splits: SplitDependencyLoader's three refusals and its cycle, INSTALL_PARSE_FAILED_BAD_MANIFEST.
apk isolated-config-missing/base.apk ' android:isolatedSplits="true"'
apk isolated-config-missing/split_feature_missing.config.de.apk \
  ' split="feature_missing.config.de" configForSplit="feature_missing"'
apk isolated-uses-missing/base.apk ' android:isolatedSplits="true"'
apk isolated-uses-missing/split_feature_extra.apk ' split="feature_extra" android:isFeatureSplit="true"' \
  $'\n  <uses-split android:name="feature_missing"/>'
apk isolated-config-of-config/base.apk ' android:isolatedSplits="true"'
apk isolated-config-of-config/split_config.fr.apk ' split="config.fr"'
apk isolated-config-of-config/split_extra.config.de.apk ' split="extra.config.de" configForSplit="config.fr"'
apk isolated-cycle/base.apk ' android:isolatedSplits="true"'
apk isolated-cycle/split_feature_a.apk ' split="feature_a" android:isFeatureSplit="true"' \
  $'\n  <uses-split android:name="feature_b"/>'
apk isolated-cycle/split_feature_b.apk ' split="feature_b" android:isFeatureSplit="true"' \
  $'\n  <uses-split android:name="feature_a"/>'

# Known differences.
apk verity-split/base.apk ''
apk verity-split/split_config.de.apk ' split="config.de"'
verity verity-split/split_config.de.apk
apk unsigned/base.apk ''
apk unsigned/split_config.de.apk ' split="config.de"'
unsign unsigned/base.apk
unsign unsigned/split_config.de.apk
apk case-names/base.apk ''
apk case-names/split_config.de.apk ' split="config.de"'
# APFS would take split_config.DE.apk for the same file.
apk case-names/split_upper_config.DE.apk ' split="config.DE"'

# Joins: splits added to the installed app with -p.
apk join-new/installed/base.apk ''
apk join-new/installed/split_config.arm64_v8a.apk "$BASE_ABI"
apk join-new/new/split_config.de.apk ' split="config.de"'
mkdir -p "$OUT/assets-de"
echo "replaced" >"$OUT/assets-de/text.txt"
apk join-replace/installed/base.apk ''
apk join-replace/installed/split_config.de.apk ' split="config.de"'
apk join-replace/new/split_config.de.apk ' split="config.de"' '' 1 "$PKG" --assets "$OUT/assets-de"
apk join-other-version/installed/base.apk ''
apk join-other-version/new/split_config.de.apk ' split="config.de"' '' 2
apk join-other-signer/installed/base.apk ''
apk join-other-signer/new/split_config.de.apk ' split="config.de"' '' 1 "$PKG" --key "$OUT/other-key"

reference set Success accept
reference own-types-free Success accept
reference isolated-fine Success accept
reference not-isolated Success accept
reference required-alone INSTALL_FAILED_MISSING_SPLIT "missing split"
reference types-unmet INSTALL_FAILED_MISSING_SPLIT "missing split"
reference own-types-required INSTALL_FAILED_MISSING_SPLIT "missing split"
reference twice "Split config.de was defined multiple times" "refuse: Split config.de was defined multiple times"
reference other-package "inconsistent with $PKG" \
  "refuse: split_config.de.apk package $PKG.other inconsistent with $PKG"
reference other-version "inconsistent with" "refuse: split_config.de.apk version code 2 inconsistent with 1"
reference other-signer "signatures are inconsistent" "refuse: split_config.de.apk signatures are inconsistent"
reference no-base "Full install must include a base package" \
  "refuse: no base APK: splits without their base only join an installed version"
reference unsigned-split Failure "refuse: split_config.de.apk signatures are inconsistent"
# Android refuses these with INSTALL_PARSE_FAILED_BAD_MANIFEST and SplitDependencyLoader's words.
reference isolated-config-missing "Split 'feature_missing.config.de' targets split 'feature_missing', which is missing." \
  "refuse: Split 'feature_missing.config.de' targets split 'feature_missing', which is missing."
reference isolated-uses-missing "Split 'feature_extra' requires split 'feature_missing', which is missing." \
  "refuse: Split 'feature_extra' requires split 'feature_missing', which is missing."
reference isolated-config-of-config \
  "Split 'extra.config.de' declares itself as configuration split for a non-feature split 'config.fr'" \
  "refuse: Split 'extra.config.de' declares itself as configuration split for a non-feature split 'config.fr'"
reference isolated-cycle "Cycle detected in split dependencies." "refuse: Cycle detected in split dependencies."
reference verity-split Success "ask: cannot verify split_config.de.apk" \
  "Minato does not check verity digests, b8b"
reference unsigned INSTALL_PARSE_FAILED_NO_CERTIFICATES "ask: unsigned" "Minato asks about unsigned APKs, ADR-0034"
reference case-names Success "refuse: Splits config.DE and config.de differ only in case" \
  "APFS does not tell the two file names apart, the decision"
reference join-new Success accept
reference join-replace Success accept
JOIN_REFUSED="refuse: the splits cannot join the installed version: no base, or another package, version code or signer"
reference join-other-version "inconsistent with" "$JOIN_REFUSED"
reference join-other-signer "signatures are inconsistent" "$JOIN_REFUSED"

total=$((passed + failed))
if [[ $failed -gt 0 ]]; then
  echo "formats reference: $failed of $total cases failed"
  exit 1
fi
echo "formats reference: $total of $total cases fine${android:+ on Android}"
