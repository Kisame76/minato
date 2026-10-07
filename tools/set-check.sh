#!/usr/bin/env bash
# Checks what split sets show in the Simulator (ADR-0048 b7c): a set with its splits becomes one entry; a split joins
# the installed set; a lone split and a set without an arm64-v8a split say why on their tiles; one unit with two
# asking sets asks once, and "Replace" answers both; a set with a split Minato cannot verify asks, as a fresh install
# and over the installed version; one unit's rejections share one alert; and a question over an installed and a fresh
# set says what it does to each. Each unit goes into the library's incoming/ folder as a folder of APKs, as "+" leaves a
# pick there, and a launch imports it. Then containers (ADR-0049 a3): a set in SAI's .apks, in bundletool's .apks with a
# toc.json, which skips the set's x86_64 split, in a plain .zip and in APKMirror's plain .apkm (e) each install or
# update its app, and a .apks without an APK and an encrypted .apkm say why on their tiles. An XAPK (c2) installs its
# set with its OBB in the app's OBB folder, which backups leave out, and names on the entry what it leaves out, an OBB
# of another app among it; its update's OBB replaces the first, and other bytes under that OBB's name ask first: "Keep
# Installed" keeps the file, "Replace" puts them in. Single OBBs (d): one picked by hand for X by its name replaces X's
# main, older as it is; one without an OBB name asks which app it belongs to and is named for X; "Don’t Import" lets
# one go. The app's Info.plist offers it for .apks, .xapk, .apkm and .obb in "Open with". The Debug build answers
# through app/Sources/Library/UpdateCheck.swift. The test apps are removed afterwards.
# Usage: tools/set-check.sh [simulator name or UDID]
# Optional environment:
#   SKIP_BUILD  when set, installs the app built last instead of building it first
#   APP         a built Minato.app (Debug) to install in place of tools/sim.sh's build
#   SHOTS       a folder for screenshots of each question, alert, note and tile
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
PREFIX="io.github.kisame76.minato.check.set"
LONE="file:split_config.de.apk"
read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${1:-}") || true
if [[ -z "${SIM_UDID:-}" ]]; then
  echo "No matching iPhone simulator found." >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
[[ -f build/fixtures/bin/libraw-dyn.so ]] || tools/build-fixtures.sh >/dev/null
LIBRARY=build/fixtures/bin/libraw-dyn.so
NATIVE_ACTIVITY='
    <activity android:name="android.app.NativeActivity" android:exported="true">
      <meta-data android:name="android.app.lib_name" android:value="raw-dyn"/>
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>'

# apk <unit>/<file> <app> <versionCode> <manifest attributes> [lib] [more make-apk arguments]
# packs one APK of app <app> ("Set Check <app>", package $PREFIX<app in lower case>) at version <versionCode>.0 into the
# folder of unit <unit>. A base gets the NativeActivity launcher and the label, a split (split= among its attributes)
# neither; "lib" adds the library.
apk() {
  local path=$1 app=$2 code=$3 attributes=$4 library="" children="" label=""
  shift 4
  if [[ "${1:-}" == lib ]]; then
    library=yes
    shift
  fi
  if [[ "$attributes" != *'split="'* ]]; then
    children=$NATIVE_ACTIVITY
    label=" android:label=\"Set Check $app\""
  fi
  mkdir -p "$WORK/units/$(dirname "$path")"
  cat >"$WORK/manifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$PREFIX$(tr '[:upper:]' '[:lower:]' <<<"$app")" android:versionCode="$code" android:versionName="$code.0"$attributes>
  <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
  <application android:hasCode="false" android:extractNativeLibs="false"$label>$children
  </application>
</manifest>
EOF
  local arguments=(--manifest "$WORK/manifest.xml" --out "$WORK/units/$path")
  [[ -z "$library" ]] || arguments+=(--lib "$LIBRARY")
  if ! tools/make-apk.sh "${arguments[@]}" "$@" >/dev/null 2>"$WORK/make-apk.log"; then
    cat "$WORK/make-apk.log" >&2
    echo "make-apk refuses $path" >&2
    exit 1
  fi
}
# set_of <unit> <app> <versionCode> [prefix] [more make-apk arguments]: a base that needs splits and its ABI split,
# which holds the library, with file names starting with <prefix>.
set_of() {
  local unit=$1 app=$2 code=$3 prefix=${4:-}
  shift 3
  [[ $# -eq 0 ]] || shift
  apk "$unit/${prefix}base.apk" "$app" "$code" ' android:isSplitRequired="true"' "$@"
  apk "$unit/${prefix}split_config.arm64_v8a.apk" "$app" "$code" ' split="config.arm64_v8a"' lib "$@"
}

# A with its ABI split, a density and a language split it takes, and an ABI and a density split it skips.
set_of a1 A 1
apk a1/split_config.x86_64.apk A 1 ' split="config.x86_64"'
apk a1/split_config.xxhdpi.apk A 1 ' split="config.xxhdpi"'
apk a1/split_config.mdpi.apk A 1 ' split="config.mdpi"'
apk a1/split_config.en.apk A 1 ' split="config.en"'
apk a1-fr/split_config.fr.apk A 1 ' split="config.fr"'
set_of a2 A 2
set_of b2 B 2
set_of ab1 A 1 a-
set_of ab1 B 1 b-
apk ab4/a-base.apk A 4 ' android:isSplitRequired="true"'
apk ab4/b-base.apk B 4 ' android:isSplitRequired="true"'
# C's language split is signed again over a verity digest only, which Android checks and Minato cannot (b8b).
for code in 1 2; do
  apk "c$code/base.apk" C "$code" '' lib
  apk "c$code/split.apk" C "$code" ' split="config.de"'
  python3 tools/sign-verity.py "$WORK/units/c$code/split.apk" "$WORK/units/c$code/split_config.de.apk" >/dev/null
  rm "$WORK/units/c$code/split.apk"
done
apk d1/base.apk D 1 ' android:isSplitRequired="true"'
apk d1/split_config.x86_64.apk D 1 ' split="config.x86_64"'
apk e-lone/split_config.de.apk E 1 ' split="config.de"'
# A 2.0 by another signer beside G, which is new and has a split Minato cannot verify; then a set of A 4.0 whose only
# split does not fit, beside the same G.
set_of ag A 2 a- --key "$WORK/other-key"
apk ag/g-base.apk G 1 '' lib
apk ag/split.apk G 1 ' split="config.de"'
python3 tools/sign-verity.py "$WORK/units/ag/split.apk" "$WORK/units/ag/g-split_config.de.apk" >/dev/null
rm "$WORK/units/ag/split.apk"
apk ar/a-base.apk A 4 ' android:isSplitRequired="true"'
apk ar/a-split_config.x86_64.apk A 4 ' split="config.x86_64"'
cp "$WORK/units/ag/g-base.apk" "$WORK/units/ag/g-split_config.de.apk" "$WORK/units/ar/"

# Containers (ADR-0049 a3): K 1.0 to 4.0, each a set whose x86_64 split the choice skips, packed as SAI, bundletool, a
# zipped folder and APKMirror's plain .apkm (e) do; then a .apks without an APK and an encrypted .apkm.
for code in 1 2 3 4; do
  set_of "k$code-files" K "$code"
  apk "k$code-files/split_config.x86_64.apk" K "$code" ' split="config.x86_64"'
done
# container <unit>/<name> <entry>=<file> ...: a ZIP whose entries are stored, as bundletool and SAI store them; an
# entry without a file holds its name.
container() {
  mkdir -p "$WORK/units/$(dirname "$1")"
  python3 - "$WORK/units/$1" "${@:2}" <<'EOF'
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_STORED) as out:
    for pair in sys.argv[2:]:
        name, _, path = pair.partition("=")
        if path:
            out.write(path, name)
        else:
            out.writestr(name, name)
EOF
}
K1=$WORK/units/k1-files K2=$WORK/units/k2-files K3=$WORK/units/k3-files
container k1/k.apks base.apk="$K1/base.apk" split_config.arm64_v8a.apk="$K1/split_config.arm64_v8a.apk" \
  split_config.x86_64.apk="$K1/split_config.x86_64.apk" meta.sai_v2.json=
python3 - "$WORK/toc.json" "${PREFIX}k" <<'EOF'
import json
import sys

def abi(alias, other):
    return {"value": [{"alias": alias}], "alternatives": [{"alias": other}]}

toc = {"packageName": sys.argv[2], "bundletool": {"version": "1.18.3"}, "variant": [{
    "targeting": {"sdkVersionTargeting": {"value": [{"min": 21}]}},
    "apkSet": [{"moduleMetadata": {"name": "base", "deliveryType": "INSTALL_TIME"}, "apkDescription": [
        {"path": "splits/base-master.apk", "splitApkMetadata": {"isMasterSplit": True}},
        {"path": "splits/base-arm64_v8a.apk", "splitApkMetadata": {"splitId": "config.arm64_v8a"},
         "targeting": {"abiTargeting": abi("ARM64_V8A", "X86_64")}},
        {"path": "splits/base-x86_64.apk", "splitApkMetadata": {"splitId": "config.x86_64"},
         "targeting": {"abiTargeting": abi("X86_64", "ARM64_V8A")}}]}]}]}
json.dump(toc, open(sys.argv[1], "w"))
EOF
container k2/k.apks toc.json="$WORK/toc.json" splits/base-master.apk="$K2/base.apk" \
  splits/base-arm64_v8a.apk="$K2/split_config.arm64_v8a.apk" splits/base-x86_64.apk="$K2/split_config.x86_64.apk"
container k3/k.zip "K/base.apk=$K3/base.apk" "K/arm64.apk=$K3/split_config.arm64_v8a.apk" \
  "K/x86_64.apk=$K3/split_config.x86_64.apk" "__MACOSX/K/._base.apk="
container x1/empty.apks readme.txt=
# A plain .apkm: K 4.0's set beside info.json with APKMirror's keys as public descriptions name them [unverified], which
# decides nothing; an encrypted one is no ZIP.
K4=$WORK/units/k4-files
printf '{"apkm_version": 5, "app_name": "Set Check K", "pname": "%s", "versioncode": "4"}' "${PREFIX}k" \
  > "$WORK/info.json"
container k4/k.apkm info.json="$WORK/info.json" base.apk="$K4/base.apk" \
  split_config.arm64_v8a.apk="$K4/split_config.arm64_v8a.apk" \
  split_config.x86_64.apk="$K4/split_config.x86_64.apk" icon.png=
mkdir -p "$WORK/units/k5"
printf 'not a ZIP, as an encrypted APKM is not' > "$WORK/units/k5/locked.apkm"
# XAPKs (ADR-0049 c2), packed as APKPure packs them: X 1.0's set with its main OBB, a file in its OBB folder that is no
# OBB and an OBB of an app it holds no APK of; then X 2.0 with main 2, which replaces main 1, and X 2.0 again with other
# bytes under main 2's name.
X=${PREFIX}x
for code in 1 2; do
  set_of "x$code-files" X "$code"
  printf 'main %s' "$code" > "$WORK/main.$code.obb"
  python3 - "$WORK/x$code-manifest.json" "$X" "$code" <<'EOF'
import json
import sys

path, package, code = sys.argv[1:]
obb = f"Android/obb/{package}/main.{code}.{package}.obb"
json.dump({"xapk_version": 2, "package_name": package, "name": "Set Check X", "version_code": code,
           "version_name": f"{code}.0",
           "split_apks": [{"file": f"{package}.apk", "id": "base"},
                          {"file": "config.arm64_v8a.apk", "id": "config.arm64_v8a"}],
           "expansions": [{"file": obb, "install_location": "EXTERNAL_STORAGE", "install_path": obb}]},
          open(path, "w"))
EOF
done
X1=$WORK/units/x1-files X2=$WORK/units/x2-files
container x2/game.xapk manifest.json="$WORK/x1-manifest.json" "$X.apk=$X1/base.apk" \
  config.arm64_v8a.apk="$X1/split_config.arm64_v8a.apk" "Android/obb/$X/main.1.$X.obb=$WORK/main.1.obb" \
  "Android/obb/$X/readme.txt=" "Android/obb/com.example.other/main.1.com.example.other.obb=$WORK/main.1.obb" icon.png=
container x3/game.xapk manifest.json="$WORK/x2-manifest.json" "$X.apk=$X2/base.apk" \
  config.arm64_v8a.apk="$X2/split_config.arm64_v8a.apk" "Android/obb/$X/main.2.$X.obb=$WORK/main.2.obb"
printf 'main 2, rebuilt' > "$WORK/main.2.rebuilt.obb"
container x4/game.xapk manifest.json="$WORK/x2-manifest.json" "$X.apk=$X2/base.apk" \
  config.arm64_v8a.apk="$X2/split_config.arm64_v8a.apk" "Android/obb/$X/main.2.$X.obb=$WORK/main.2.rebuilt.obb"
# Single OBBs (ADR-0049 d): main 1 for X by its name; a file without an OBB name, twice.
mkdir -p "$WORK/units/o1" "$WORK/units/o2" "$WORK/units/o3"
printf 'main 1, picked' > "$WORK/units/o1/main.1.$X.obb"
printf 'level' > "$WORK/units/o2/level.obb"
printf 'level, again' > "$WORK/units/o3/level.obb"

if [[ -n "${APP:-}" ]]; then
  xcrun simctl install "$SIM_UDID" "$APP"
elif [[ -n "${SKIP_BUILD:-}" ]]; then
  xcrun simctl install "$SIM_UDID" build/DerivedData/Build/Products/Debug-iphonesimulator/Minato.app
else
  INSTALL_ONLY=1 tools/sim.sh --headless "$SIM_UDID" >/dev/null
fi
CONTAINER=$(xcrun simctl get_app_container "$SIM_UDID" "$BUNDLE_ID" data)
SUPPORT="$CONTAINER/Library/Application Support"
INCOMING="$SUPPORT/incoming"
CHECK="$CONTAINER/tmp/update-check"
EVENTS="$CHECK/events"
failures=0

expect() {
  local what=$1 wanted=$2 got=$3
  if [[ "$got" == "$wanted" ]]; then
    echo "ok    $what"
  else
    echo "FAIL  $what: got \"$got\", wanted \"$wanted\""
    failures=$((failures + 1))
  fi
}
relaunch() {
  xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  : > "$EVENTS"
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
}
# The events since the last launch, without time and pid, one per line; a line break inside one reads "\n".
events() {
  python3 - "$EVENTS" <<'EOF'
import re
import sys

events = []
for line in open(sys.argv[1], encoding="utf-8").read().splitlines():
    match = re.match(r"\d+\.\d+ \d+ (.*)", line)
    if match:
        events.append(match.group(1))
    elif events:
        events[-1] += "\\n" + line
print("\n".join(events))
EOF
}
# Prints the first event that contains $1, waiting up to 20 s; "none" if none comes.
event() {
  for _ in $(seq 1 100); do
    local line
    line=$(events 2>/dev/null | grep -m 1 -F -- "$1" || true)
    if [[ -n "$line" ]]; then
      echo "$line"
      return 0
    fi
    sleep 0.2
  done
  echo "none"
}
# How many events contain $1.
count() { events 2>/dev/null | grep -c -F -- "$1" || true; }
# Terminates Minato, puts unit $1 into incoming/ as "+" does, a folder that appears whole, and launches Minato,
# which imports it; $2 answers the question, if any.
offer() {
  xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  mkdir -p "$INCOMING"
  rm -rf "$INCOMING/$1" "$INCOMING/$1.partial"
  cp -R "$WORK/units/$1" "$INCOMING/$1.partial"
  mv "$INCOMING/$1.partial" "$INCOMING/$1"
  if [[ -n "${2:-}" ]]; then echo "$2" > "$CHECK/answer"; else rm -f "$CHECK/answer"; fi
  relaunch
}
# Answers the question that is up with $1: it comes again at the next launch.
answer() {
  echo "$1" > "$CHECK/answer"
  relaunch
}
# What the library holds of $1: "<versionName> splits <names> skipped <names>", or "none".
entry() {
  python3 - "$SUPPORT/library.json" "$1" <<'EOF'
import json
import sys

try:
    entries = json.load(open(sys.argv[1], encoding="utf-8"))["entries"]
except FileNotFoundError:
    entries = []
entry = next((e for e in entries if e["id"] == sys.argv[2]), None)
if entry is None:
    print("none")
else:
    version = (entry.get("report") or {}).get("versionName") or "-"
    splits = ",".join(sorted(s.get("splitName") or "?" for s in entry.get("splits") or [])) or "-"
    skipped = ",".join(sorted(s["splitName"] for s in entry.get("skippedSplits") or [])) or "-"
    print(version, "splits", splits, "skipped", skipped)
EOF
}
entries() {
  python3 - "$SUPPORT/library.json" <<'EOF'
import json
import sys

try:
    print(len(json.load(open(sys.argv[1], encoding="utf-8"))["entries"]))
except FileNotFoundError:
    print(0)
EOF
}
shot() {
  [[ -n "${SHOTS:-}" ]] || return 0
  mkdir -p "$SHOTS"
  xcrun simctl io "$SIM_UDID" screenshot "$SHOTS/$1.png" >/dev/null 2>&1
}
waiting() { [[ -e "$INCOMING/$1" ]] && echo yes || echo no; }
# Removes app $1 through UpdateCheck's "end", which also removes the check's folder, and makes the folder again.
remove() {
  echo "$1" > "$CHECK/end"
  relaunch
  for _ in $(seq 1 50); do [[ -d "$CHECK" ]] || break; sleep 0.2; done
  mkdir -p "$CHECK"
}
APPS=("${PREFIX}a" "${PREFIX}b" "${PREFIX}c" "${PREFIX}d" "${PREFIX}g" "$LONE" "${PREFIX}k" "file:empty.apks"
      "file:locked.apkm" "$X")
# Whether the installed app claims type $1 with extension $2 for "Open with": its document type and the type it
# imports (project.yml).
open_with() {
  local app
  app=$(xcrun simctl get_app_container "$SIM_UDID" "$BUNDLE_ID" app)
  if plutil -extract CFBundleDocumentTypes json -o - "$app/Info.plist" | grep -q "\"$1\"" \
      && plutil -extract UTImportedTypeDeclarations json -o - "$app/Info.plist" | grep -q "\"$2\""; then
    echo yes
  else
    echo no
  fi
}
# The files in app $1's OBB folder with what they hold, "<name>: <content>" one after the other, or "none".
obbs() {
  local folder="$SUPPORT/data/$1/obb" out="" file
  for file in "$folder"/*; do
    [[ -f "$file" ]] && out+="${out:+ | }$(basename "$file"): $(cat "$file")"
  done
  echo "${out:-none}"
}
# Whether backups leave out folder $1: the attribute URLResourceValues.isExcludedFromBackup sets.
excluded() { xattr "$1" 2>/dev/null | grep -q com_apple_backup_excludeItem && echo yes || echo no; }
# What app $1's entry names as left out, "<path>: <reason>" one after the other, or "-".
ignored() {
  python3 - "$SUPPORT/library.json" "$1" <<'EOF'
import json
import sys

entries = json.load(open(sys.argv[1], encoding="utf-8"))["entries"]
entry = next((e for e in entries if e["id"] == sys.argv[2]), {})
print(" | ".join(f"{f['path']}: {f['reason']}" for f in entry.get("ignoredFiles") or []) or "-")
EOF
}

# Test apps and units left from an earlier run go first.
for unit in "$WORK"/units/*; do rm -rf "$INCOMING/$(basename "$unit")"; done
rm -rf "$CHECK"
mkdir -p "$CHECK"
for app in "${APPS[@]}"; do remove "$app"; done

before=$(entries)
offer a1
expect "a set" "import install" "$(event "import ")"
expect "  imports once" "1" "$(count "import ")"
expect "  as one entry" "$((before + 1))" "$(entries)"
expect "  with the splits it takes, and those it skips" \
  "1.0 splits config.arm64_v8a,config.en,config.xxhdpi skipped config.mdpi,config.x86_64" "$(entry "${PREFIX}a")"
expect "  its tile" "tile Set Check A | Ready" "$(event "tile Set Check A")"
shot set-tile

offer a1-fr
expect "a split joins the installed set" "import reinstall" "$(event "import ")"
expect "  its note" "note Installed “Set Check A” 1.0 again." "$(event "note ")"
shot join-note
expect "  the set takes it" "1.0 splits config.arm64_v8a,config.en,config.fr,config.xxhdpi skipped -" "$(entry "${PREFIX}a")"

offer e-lone
LONE_TEXT="Not installed: Split APK “split_config.de.apk” needs its base APK. Import it together with its base APK."
expect "a lone split" "tile split_config.de.apk | $LONE_TEXT" "$(event "tile split_config.de.apk")"
shot lone-split-tile

offer d1
expect "a set without an arm64-v8a split" \
  "tile Set Check D | Incompatible: Its split APKs do not fit this iPhone: No arm64-v8a split" "$(event "tile Set Check D")"
shot no-arm64-tile

offer b2
expect "another app's set" "tile Set Check B | Ready" "$(event "tile Set Check B")"
offer a2
expect "an update of a set" "note Updated “Set Check A” to 2.0." "$(event "note ")"

OLDER="1.0 is older than the installed 2.0. The data stays, but the older version may not be able to read it."
QUESTION="alert Replace 2 Apps? | “Set Check A”: $OLDER\n\n“Set Check B”: $OLDER | Replace / Keep Installed"
offer ab1
expect "one unit with two asking sets: one question" "$QUESTION" "$(event "alert ")"
shot two-sets-question
answer replace
expect "  comes again, and Replace" "answered replace" "$(event "answered ")"
expect "  answers both" "note Replaced “Set Check A” with 1.0. Replaced “Set Check B” with 1.0." "$(event "note ")"
expect "  A is 1.0" "1.0" "$(entry "${PREFIX}a" | cut -d ' ' -f 1)"
expect "  B is 1.0" "1.0" "$(entry "${PREFIX}b" | cut -d ' ' -f 1)"
expect "  the unit is gone" "no" "$(waiting ab1)"

UNCHECKED="Minato cannot verify how split_config.de.apk is signed, so it cannot tell whether its content was changed."
offer c1 keep
expect "a split Minato cannot verify asks before a fresh install" \
  "alert Install “Set Check C”? | $UNCHECKED | Install / Don’t Install" "$(event "alert ")"
expect "  Don’t Install" "answered keep" "$(event "answered ")"
sleep 1
expect "  installs nothing" "none" "$(entry "${PREFIX}c")"
expect "  and discards the unit" "no" "$(waiting c1)"
offer c1
expect "  asks again" "alert Install “Set Check C”? | $UNCHECKED | Install / Don’t Install" "$(event "alert ")"
shot fresh-unchecked-question
answer replace
expect "  Install" "answered replace" "$(event "answered ")"
expect "  installs the set" "tile Set Check C | Ready" "$(event "tile Set Check C")"
expect "  with its split" "1.0 splits config.de skipped -" "$(entry "${PREFIX}c")"
offer c2
expect "  and asks before it replaces the installed version" \
  "alert Replace “Set Check C”? | $UNCHECKED | Replace / Keep Installed" "$(event "alert ")"
shot update-unchecked-question
answer replace
expect "  Replace" "note Replaced “Set Check C” with 2.0." "$(event "note ")"

NEEDS="4.0 needs its split APKs. Version 1.0 stays installed."
offer ab4
expect "one unit's rejections share one alert" \
  "alert Can’t Update 2 Apps | “Set Check A”: $NEEDS\n\n“Set Check B”: $NEEDS | OK" "$(event "alert ")"
expect "  recorded once" "1" "$(count "alert ")"
shot two-rejections-alert
expect "  A stays 1.0" "1.0" "$(entry "${PREFIX}a" | cut -d ' ' -f 1)"
expect "  B stays 1.0" "1.0" "$(entry "${PREFIX}b" | cut -d ' ' -f 1)"
expect "  the unit is discarded" "no" "$(waiting ab4)"

SIGNER="The new file is signed by someone else, for example as a modified version. Replacing it gives it the data of the installed app. To start without that data, delete the app first."
G_UNCHECKED="Minato cannot verify how g-split_config.de.apk is signed, so it cannot tell whether its content was changed."
MIXED="alert Replace 1 App and Install 1? | “Set Check A”: $SIGNER\n\n“Set Check G”: $G_UNCHECKED | Replace and Install / Don’t Install"
offer ag
expect "a question over an installed and a fresh set says both" "$MIXED" "$(event "alert ")"
shot mixed-question
answer keep
expect "  Don’t Install" "answered keep" "$(event "answered ")"
sleep 1
expect "  A stays 1.0" "1.0" "$(entry "${PREFIX}a" | cut -d ' ' -f 1)"
expect "  G is not installed" "none" "$(entry "${PREFIX}g")"
expect "  the unit is discarded" "no" "$(waiting ag)"

# The answer imports the unit again, its rejected set too, whose files stay with the unit while G asks. SwiftUI shows
# the rejection and the question one after the other, in either order, so the check counts the rejections Minato sets.
offer ar replace
REJECTED="Can’t Update “Set Check A” | 4.0 cannot run: Its split APKs do not fit this iPhone: No arm64-v8a split. Version 1.0 stays installed."
expect "a rejected set beside a question" "rejection $REJECTED" "$(event "rejection ")"
expect "  Install" "answered replace" "$(event "answered ")"
expect "  installs G" "tile Set Check G | Ready" "$(event "tile Set Check G")"
sleep 1
expect "  alerts the rejection once" "1" "$(count "rejection ")"
expect "  A stays 1.0" "1.0" "$(entry "${PREFIX}a" | cut -d ' ' -f 1)"
expect "  the unit is gone" "no" "$(waiting ar)"

offer k1
expect "a set in SAI's .apks" "tile Set Check K | Ready" "$(event "tile Set Check K")"
expect "  takes the splits it takes as loose files" "1.0 splits config.arm64_v8a skipped config.x86_64" "$(entry "${PREFIX}k")"
expect "  the unit is gone" "no" "$(waiting k1)"
offer k2
expect "a set in bundletool's .apks" "note Updated “Set Check K” to 2.0." "$(event "note ")"
expect "  with the toc's choice" "2.0 splits config.arm64_v8a skipped -" "$(entry "${PREFIX}k")"
offer k3
expect "a set in a plain .zip" "note Updated “Set Check K” to 3.0." "$(event "note ")"
expect "  its splits" "3.0 splits config.arm64_v8a skipped config.x86_64" "$(entry "${PREFIX}k")"
offer k4
expect "a set in a plain .apkm" "note Updated “Set Check K” to 4.0." "$(event "note ")"
expect "  its splits, whatever info.json says" "4.0 splits config.arm64_v8a skipped config.x86_64" \
  "$(entry "${PREFIX}k")"
offer k5
expect "an encrypted .apkm" "tile locked.apkm | Not installed: Encrypted APKM files aren’t supported" \
  "$(event "tile locked.apkm")"
expect "  the unit is gone" "no" "$(waiting k5)"
offer x1
expect "a .apks without an APK" "tile empty.apks | Not installed: It holds no APK" "$(event "tile empty.apks")"
shot container-refusal-tile
expect "  the unit is gone" "no" "$(waiting x1)"
offer x2
expect "an XAPK" "tile Set Check X | Ready" "$(event "tile Set Check X")"
expect "  its set" "1.0 splits config.arm64_v8a skipped -" "$(entry "$X")"
expect "  its OBB in the app's OBB folder" "main.1.$X.obb: main 1" "$(obbs "$X")"
expect "  which backups leave out" "yes" "$(excluded "$SUPPORT/data/$X/obb")"
LEFT_OUT="Android/obb/$X/readme.txt: its name is not main.<version>.<package>.obb or patch.<version>.<package>.obb"
LEFT_OUT+=" | Android/obb/com.example.other/main.1.com.example.other.obb: it is for com.example.other, which has no APK here"
expect "  what it leaves out, named on the entry" "$LEFT_OUT" "$(ignored "$X")"
expect "  the unit is gone" "no" "$(waiting x2)"
offer x3
expect "an XAPK's update, with its OBB in the note (ADR-0049 d)" \
  "note Updated “Set Check X” to 2.0. “main.2.$X.obb” was added to “Set Check X”." "$(event "note ")"
expect "  its OBB replaces the first" "main.2.$X.obb: main 2" "$(obbs "$X")"
expect "  and it leaves nothing out" "-" "$(ignored "$X")"
expect "  the unit is gone" "no" "$(waiting x3)"
OTHER_OBB="The expansion file “main.2.$X.obb” has different contents from the installed one, for example from another"
OTHER_OBB+=" source. Replacing it changes the app’s content."
offer x4 keep
expect "other bytes under an installed OBB's name ask first" \
  "alert Replace “Set Check X”? | $OTHER_OBB | Replace / Keep Installed" "$(event "alert ")"
shot other-obb-question
expect "  Keep Installed" "answered keep" "$(event "answered ")"
sleep 1
expect "  keeps the installed file" "main.2.$X.obb: main 2" "$(obbs "$X")"
expect "  the unit is discarded" "no" "$(waiting x4)"
offer x4 replace
expect "  Replace" "answered replace" "$(event "answered ")"
sleep 1
expect "  puts the new bytes in" "main.2.$X.obb: main 2, rebuilt" "$(obbs "$X")"
expect "  the unit is gone" "no" "$(waiting x4)"
offer o1
expect "an OBB picked by hand goes to the app its name gives" \
  "note “main.1.$X.obb” was added to “Set Check X”. It replaces “main.2.$X.obb”." "$(event "note ")"
expect "  and replaces the main, older as it is" "main.1.$X.obb: main 1, picked" "$(obbs "$X")"
expect "  the unit is gone" "no" "$(waiting o1)"
OBB_QUESTION="alert Add “level.obb” to an App? | An expansion file holds extra content for an app. Choose the app it belongs to."
offer o2 "app $X"
alert=$(event "alert Add")
expect "an OBB without an OBB name asks which app" "yes" \
  "$([[ "$alert" == "$OBB_QUESTION | "*"Set Check X"*" / Don’t Import" ]] && echo yes || echo no)"
shot obb-question
expect "  the answer names its app" "answered app $X" "$(event "answered ")"
expect "  which takes it under a name of its own" \
  "note “level.obb” was added to “Set Check X”. It replaces “main.1.$X.obb”." "$(event "note ")"
expect "  as its main for 2.0" "main.2.$X.obb: level" "$(obbs "$X")"
expect "  the unit is gone" "no" "$(waiting o2)"
offer o3 keep
expect "Don’t Import" "answered keep" "$(event "answered ")"
sleep 1
expect "  lets the OBB go with its unit" "no" "$(waiting o3)"
expect "  and leaves the app's OBB as it is" "main.2.$X.obb: level" "$(obbs "$X")"
expect "\"Open with\" offers Minato for .apks" "yes" "$(open_with io.github.kisame76.minato.apks apks)"
expect "\"Open with\" offers Minato for .xapk" "yes" "$(open_with io.github.kisame76.minato.xapk xapk)"
expect "\"Open with\" offers Minato for .obb" "yes" "$(open_with io.github.kisame76.minato.obb obb)"
expect "\"Open with\" offers Minato for .apkm" "yes" "$(open_with io.github.kisame76.minato.apkm apkm)"

for app in "${APPS[@]}"; do remove "$app"; done
rm -rf "$CHECK"
for app in "${APPS[@]}"; do expect "the test app $app is removed" "none" "$(entry "$app")"; done
xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true

if [[ "$failures" == 0 ]]; then
  echo "set check: all passed"
else
  echo "set check: $failures failed"
  exit 1
fi
