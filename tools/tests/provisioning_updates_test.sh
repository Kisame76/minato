#!/usr/bin/env bash
# tools/provisioning-updates.sh against profiles made on the fly in a temp PROFILE_DIR (a plain plist between the bytes
# of a fake CMS envelope, as the real files hold it): a fresh profile prints nothing, one expiring in 10 h prints the
# flag with its note, a bundle ID without a profile prints the flag, the newest of several profiles counts, a wildcard
# profile covers its bundle IDs, and none of the fixtures' own team or account values appears in the output.
set -euo pipefail
cd "$(dirname "$0")/../.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export PROFILE_DIR="$work/profiles"
mkdir -p "$PROFILE_DIR"
make_profile() { # file, application-identifier, hours until it expires
  python3 - "$PROFILE_DIR/$1.mobileprovision" "$2" "$3" <<'PY'
import datetime, plistlib, sys
path, identifier, hours = sys.argv[1], sys.argv[2], float(sys.argv[3])
expires = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None) + datetime.timedelta(hours=hours)
body = plistlib.dumps({"Entitlements": {"application-identifier": identifier},
                       "ExpirationDate": expires, "TeamName": "Example Team", "TeamIdentifier": ["ABCDE12345"],
                       "Name": "appleseed@example.com"})
open(path, "wb").write(b"\x30\x82\x01\x00binary envelope" + body + b"signature bytes\x00\x01")
PY
}
failed=0
check() { # label, expected stdout, expected stderr pattern ('' for none), then the command
  local label=$1 want=$2 note=$3 out err
  shift 3
  out=$("$@" 2> "$work/err") || { echo "FAIL  $label: exit status $?"; failed=1; return; }
  err=$(cat "$work/err")
  if [[ "$out" != "$want" ]]; then echo "FAIL  $label: printed \"$out\", not \"$want\""; failed=1; return; fi
  if [[ -z "$note" && -n "$err" ]] || [[ -n "$note" && ! "$err" =~ $note ]]; then
    echo "FAIL  $label: stderr \"$err\" does not fit \"$note\""; failed=1; return
  fi
  if [[ "$out$err" == *ABCDE12345* || "$out$err" == *"Example Team"* || "$out$err" == *appleseed* ]]; then
    echo "FAIL  $label: a team or account value of the profile shows"; failed=1; return; fi
  echo "ok    $label"
}
tool=tools/provisioning-updates.sh
make_profile fresh ABCDE12345.io.example.fresh 150
make_profile soon ABCDE12345.io.example.soon 10
make_profile old-a ABCDE12345.io.example.renewed 5
make_profile new-a ABCDE12345.io.example.renewed 160
make_profile wild "ABCDE12345.io.wild.*" 100
check "a fresh profile asks for nothing" "" "" "$tool" io.example.fresh
soon_note="^note  the profile for io.example.soon expires [0-9-]+.*asking Apple for a new one"
soon_note="$soon_note \\(needs the Xcode account\\)$"
check "a profile expiring in 10 h asks Apple" "-allowProvisioningUpdates" "$soon_note" "$tool" io.example.soon
check "a bundle ID without a profile asks Apple" "-allowProvisioningUpdates" "io.example.missing" \
  "$tool" io.example.missing
check "the newest of two profiles counts" "" "" "$tool" io.example.renewed
check "a wildcard profile covers its IDs" "" "" "$tool" io.wild.app
check "one of several IDs needing a profile is enough" "-allowProvisioningUpdates" "io.example.soon" \
  "$tool" io.example.fresh io.example.soon
check "an empty profile folder asks Apple" "-allowProvisioningUpdates" "io.example.fresh" \
  env PROFILE_DIR="$work/none" "$tool" io.example.fresh
exit $failed
