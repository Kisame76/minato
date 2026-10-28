#!/usr/bin/env bash
# Checks when tools/deploy.sh hands the signing certificate over, without a device or a build: a copy of
# the script runs in a scratch tree whose build scripts, find-device.py, require-device.py, xcodebuild, xcrun and
# hand-over-certificate.sh are fakes. Only the Debug build answers the hand-over
# (app/Sources/Signing/CertificateHandover.swift), so a Release install must not start it, which would
# wait out devicectl's 120 s for an answer that never comes; a Debug install still starts it.
# Usage: tools/deploy-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
failed=0

make_tree() {
  local tree=$1 script
  mkdir -p "$tree/tools" "$tree/fake"
  cp tools/deploy.sh "$tree/tools/"
  for script in gen.sh build-native.sh build-graphics-host.sh; do
    printf '#!/bin/sh\nexit 0\n' >"$tree/tools/$script"
  done
  printf '#!/bin/sh\necho "$*" >>"%s/hand-over.log"\n' "$tree" >"$tree/tools/hand-over-certificate.sh"
  printf 'print("FAKE-DEVICE FAKE-UDID enabled wired")\n' >"$tree/tools/find-device.py"
  : >"$tree/tools/require-device.py" # reachable, as tools/require-device-check.sh checks it
  printf '#!/bin/sh\nexit 0\n' >"$tree/fake/xcodebuild"
  printf '#!/bin/sh\necho "$*" >>"%s/xcrun.log"\n' "$tree" >"$tree/fake/xcrun"
  chmod +x "$tree/tools/"*.sh "$tree/fake/"*
}

# Deploys with the configuration ("" for the default) and INSTALL_ONLY; prints the exit status.
run_tree() {
  local tree=$1 configuration=$2 status=0
  if [[ -n "$configuration" ]]; then
    CONFIGURATION=$configuration INSTALL_ONLY=1 PATH="$tree/fake:$PATH" "$tree/tools/deploy.sh" \
      >"$tree/out" 2>&1 || status=$?
  else
    INSTALL_ONLY=1 PATH="$tree/fake:$PATH" "$tree/tools/deploy.sh" >"$tree/out" 2>&1 || status=$?
  fi
  echo "$status"
}

check() {
  local name=$1 configuration=$2 hands_over=$3 tree="$WORK/$1" status problems=""
  make_tree "$tree"
  status=$(run_tree "$tree" "$configuration")
  [[ "$status" == 0 ]] || problems+=" exit $status"
  grep -q "device install app" "$tree/xcrun.log" 2>/dev/null || problems+=" not installed"
  if [[ "$hands_over" == yes ]]; then
    [[ -s "$tree/hand-over.log" ]] || problems+=" no hand-over"
  else
    [[ ! -e "$tree/hand-over.log" ]] || problems+=" handed over"
  fi
  if [[ -z "$problems" ]]; then
    echo "same     $name"
  else
    echo "DIFFERS  $name:$problems"
    sed 's/^/  /' "$tree/out"
    failed=1
  fi
}

check "a Debug install hands the certificate over" Debug yes
check "the default install is Debug and hands it over" "" yes
check "a Release install does not start the hand-over" Release no

exit "$failed"
