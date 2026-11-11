#!/usr/bin/env bash
# Whether the Minato.app bundle $1 is whole, so that it may be installed: the main binary (Info.plist's
# CFBundleExecutable), the guest's Guest/lib64 and ART's Guest/apex-lib64/com.android.art (tools/embed-guest-system.sh,
# a build phase late in the build). A build that failed in embed-guest-system leaves a bundle that installs and then
# fails every start with `could not start host path ".../Guest/apex-lib64/com.android.art"` (iPhone, 2026-11-11).
# Prints one line naming what is missing and exits 1, else prints nothing and exits 0.
# Usage: tools/check-bundle.sh <Minato.app>
set -uo pipefail
app=${1:?usage: tools/check-bundle.sh <Minato.app>}
missing=()
executable=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$app/Info.plist" 2>/dev/null) || executable=""
if [[ -z "$executable" ]]; then missing+=("Info.plist's CFBundleExecutable"); elif [[ ! -f "$app/$executable" ]]; then
  missing+=("main binary $executable"); fi
[[ -d "$app/Guest/lib64" ]] || missing+=("Guest/lib64")
[[ -d "$app/Guest/apex-lib64/com.android.art" ]] || missing+=("Guest/apex-lib64/com.android.art")
if (( ${#missing[@]} > 0 )); then
  echo "the app bundle $app is not whole (no $(IFS=,; echo "${missing[*]}" | sed 's/,/, no /g')): its build failed" \
    "or stopped early; build it again before installing it"
  exit 1
fi
