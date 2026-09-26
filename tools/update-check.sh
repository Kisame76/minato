#!/usr/bin/env bash
# Checks what an update shows in the Simulator (ADR-0034, test 4): the notes for an update, the same bytes
# and the same version again; the alert for a file that needs splits; the question before an older version
# and before another signer, with both buttons; both warnings in one question; and a question that comes
# again after Minato was quit without an answer. Each file goes into the library's incoming/ folder, and a
# launch imports it. The Debug build answers through app/Sources/Library/UpdateCheck.swift. The test app
# is removed afterwards.
# Usage: tools/update-check.sh [simulator name or UDID]
# Optional environment:
#   SKIP_BUILD  when set, installs the app built last instead of building it first
#   SHOTS       a folder for screenshots of a note and of a question
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
PKG="io.github.kisame76.minato.check.update"
NAME="Update Check"
read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${1:-}") || true
if [[ -z "${SIM_UDID:-}" ]]; then
  echo "No matching iPhone simulator found." >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
[[ -f build/fixtures/bin/libraw-dyn.so ]] || tools/build-fixtures.sh >/dev/null
mkdir -p "$WORK/assets"
echo "rebuilt" > "$WORK/assets/rebuilt.txt"
# make_apk <file> <versionCode> <versionName> [needs-splits] [more make-apk arguments]
make_apk() {
  local out=$1 code=$2 name=$3 splits=""
  shift 3
  if [[ "${1:-}" == needs-splits ]]; then
    splits='<meta-data android:name="com.android.vending.splits.required" android:value="true"/>'
    shift
  fi
  cat > "$WORK/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$PKG" android:versionCode="$code" android:versionName="$name">
  <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
  <application android:label="$NAME" android:hasCode="false">
    $splits
    <activity android:name="android.app.NativeActivity" android:exported="true">
      <meta-data android:name="android.app.lib_name" android:value="raw-dyn"/>
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>
  </application>
</manifest>
EOF
  tools/make-apk.sh --manifest "$WORK/AndroidManifest.xml" --lib build/fixtures/bin/libraw-dyn.so \
    --out "$WORK/$out" "$@" >/dev/null
}
make_apk v1.apk 1 1.0
make_apk v2.apk 2 2.0
make_apk v2-rebuilt.apk 2 2.0 --assets "$WORK/assets"
make_apk v4-splits.apk 4 4.0 needs-splits
make_apk other-v3.apk 3 3.0 --key "$WORK/other-key"

if [[ -n "${SKIP_BUILD:-}" ]]; then
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
# Prints the first event that starts with $1, without time and pid, waiting up to 20 s; "none" if none comes.
event() {
  for _ in $(seq 1 100); do
    local line
    line=$(cut -d ' ' -f 3- "$EVENTS" 2>/dev/null | grep -m 1 -F -- "$1" || true)
    if [[ -n "$line" ]]; then
      echo "$line"
      return 0
    fi
    sleep 0.2
  done
  echo "none"
}
# Puts file $1 into incoming/ and launches Minato, which imports it; $2 answers the question, if any.
offer() {
  mkdir -p "$INCOMING"
  cp "$WORK/$1" "$INCOMING/$1"
  if [[ -n "${2:-}" ]]; then echo "$2" > "$CHECK/answer"; else rm -f "$CHECK/answer"; fi
  relaunch
}
installed_version() {
  python3 - "$SUPPORT/library.json" "$PKG" <<'EOF'
import json, sys
entries = json.load(open(sys.argv[1]))["entries"]
print(next((e["report"]["versionName"] for e in entries if e["id"] == sys.argv[2]), "none"))
EOF
}
# Seconds between the first events that start with $1 and $2.
seconds_between() {
  python3 - "$EVENTS" "$1" "$2" <<'EOF'
import sys
lines = [line.split(" ", 2) for line in open(sys.argv[1]).read().splitlines()]
first = lambda prefix: next(float(t) for t, _, event in lines if event.startswith(prefix))
print(f"{first(sys.argv[3]) - first(sys.argv[2]):.1f}")
EOF
}
shot() {
  [[ -n "${SHOTS:-}" ]] || return 0
  mkdir -p "$SHOTS"
  xcrun simctl io "$SIM_UDID" screenshot "$SHOTS/$1.png" >/dev/null 2>&1
}
waiting() { [[ -e "$INCOMING/$1" ]] && echo yes || echo no; }

# A test app and files left from an earlier run go first.
rm -f "$INCOMING"/v*.apk "$INCOMING"/other-*.apk
rm -rf "$CHECK"
mkdir -p "$CHECK"
echo "$PKG" > "$CHECK/end"
relaunch
for _ in $(seq 1 50); do [[ -d "$CHECK" ]] || break; sleep 0.2; done
mkdir -p "$CHECK"

offer v1.apk
expect "install: no note" "import install" "$(event "import ")"
mkdir -p "$SUPPORT/data/$PKG"
echo "progress" > "$SUPPORT/data/$PKG/save.txt"

offer v2.apk
expect "update" "note Updated “$NAME” to 2.0." "$(event "note ")"
sleep 0.5
shot note
expect "  the note goes by itself" "note hidden" "$(event "note hidden")"
shown=$(seconds_between "note Updated" "note hidden")
expect "  after about 3 s ($shown s)" "yes" "$(python3 -c "print('yes' if 2.5 <= $shown <= 4.5 else 'no')")"

offer v2.apk
expect "the same bytes" "note “$NAME” 2.0 is already installed." "$(event "note ")"

offer v2-rebuilt.apk
expect "the same version, other bytes" "note Installed “$NAME” 2.0 again." "$(event "note ")"

offer v4-splits.apk
expect "a file that needs splits" \
  "alert Can’t Update “$NAME” | 4.0 needs its split APKs. Version 2.0 stays installed. | OK" "$(event "alert ")"
expect "  the installed version stays" "2.0" "$(installed_version)"
expect "  the file is discarded" "no" "$(waiting v4-splits.apk)"

OLDER="1.0 is older than the installed 2.0. The data stays, but the older version may not be able to read it."
offer v1.apk
expect "an older version asks" \
  "alert Replace “$NAME” with an Older Version? | $OLDER | Replace / Keep Installed" "$(event "alert ")"
expect "  the file waits" "yes" "$(waiting v1.apk)"
shot question-older
relaunch
expect "  the question comes again after quitting" \
  "alert Replace “$NAME” with an Older Version? | $OLDER | Replace / Keep Installed" "$(event "alert ")"
echo keep > "$CHECK/answer"
relaunch
expect "  Keep Installed" "answered keep" "$(event "answered ")"
sleep 1
expect "  keeps the installed version" "2.0" "$(installed_version)"
expect "  and discards the file" "no" "$(waiting v1.apk)"

offer v1.apk replace
expect "  Replace" "answered replace" "$(event "answered ")"
expect "  says what it did" "note Replaced “$NAME” with 1.0." "$(event "note ")"
expect "  installs the older version" "1.0" "$(installed_version)"

SIGNER="The new file is signed by someone else, for example as a modified version. Replacing it gives it the data of the installed app. To start without that data, delete the app first."
offer other-v3.apk keep
expect "another signer asks" "alert Replace “$NAME”? | $SIGNER | Replace / Keep Installed" "$(event "alert ")"
expect "  Keep Installed" "answered keep" "$(event "answered ")"
sleep 1
expect "  keeps the installed version" "1.0" "$(installed_version)"

offer other-v3.apk replace
expect "  Replace" "note Replaced “$NAME” with 3.0." "$(event "note ")"
expect "  installs the other signer's version" "3.0" "$(installed_version)"

offer v1.apk keep
OLDER_THAN_3="1.0 is older than the installed 3.0. The data stays, but the older version may not be able to read it."
expect "an older version of another signer: both warnings" \
  "alert Replace “$NAME” with an Older Version? | $OLDER_THAN_3 $SIGNER | Replace / Keep Installed" "$(event "alert ")"
expect "  Keep Installed" "answered keep" "$(event "answered ")"

expect "the data stayed through every update" "progress" "$(cat "$SUPPORT/data/$PKG/save.txt" 2>/dev/null || echo none)"

echo "$PKG" > "$CHECK/end"
relaunch
for _ in $(seq 1 50); do [[ -d "$CHECK" ]] || break; sleep 0.2; done
expect "the test app is removed" "none" "$(installed_version)"
xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true

if [[ "$failures" == 0 ]]; then
  echo "update check: all passed"
else
  echo "update check: $failures failed"
  exit 1
fi
