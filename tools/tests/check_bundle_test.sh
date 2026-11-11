#!/usr/bin/env bash
# tools/check-bundle.sh, against bundles made in a temp folder: a whole one passes silently; one without ART's
# Guest/apex-lib64/com.android.art (a build that failed in embed-guest-system, as on 2026-11-11), one without
# Guest/lib64 and one whose main binary is missing each fail with a line that names what is missing.
set -uo pipefail
cd "$(dirname "$0")/../.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
bundle() {
  local app="$work/$1/Minato.app"
  mkdir -p "$app/Guest/lib64" "$app/Guest/apex-lib64/com.android.art"
  /usr/libexec/PlistBuddy -c "Add :CFBundleExecutable string Minato" "$app/Info.plist" >/dev/null
  : > "$app/Minato"
  echo "$app"
}
expect() {
  local name=$1 app=$2 want=$3 out status
  out=$(tools/check-bundle.sh "$app")
  status=$?
  if [[ "$want" == pass ]]; then
    [[ $status -eq 0 && -z "$out" ]] ||
      { echo "FAIL  $name: should pass silently, got $status: $out"; failures=$((failures + 1)); }
  else
    [[ $status -eq 1 && "$out" == *"$want"* ]] ||
      { echo "FAIL  $name: should fail naming $want, got $status: $out"; failures=$((failures + 1)); }
  fi
}
expect "a whole bundle" "$(bundle whole)" pass
app=$(bundle no-art); rm -rf "$app/Guest/apex-lib64"
expect "no ART" "$app" "no Guest/apex-lib64/com.android.art"
app=$(bundle no-lib64); rm -rf "$app/Guest/lib64"
expect "no lib64" "$app" "no Guest/lib64"
app=$(bundle no-binary); rm -f "$app/Minato"
expect "no main binary" "$app" "no main binary Minato"
if (( failures > 0 )); then echo "check_bundle_test: $failures FAILED"; exit 1; fi
echo "check_bundle_test: ok"
