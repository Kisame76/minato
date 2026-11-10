#!/usr/bin/env bash
# Runs independent Mac checks side by side in lanes, on a tree that tools/check-prepare.sh prepares first (it builds
# only what changed). Each check runs with MINATO_PREPARED=1 and tools/lane-bin first in PATH, so its builds return
# at once, and writes only its own folders (build/<name>-check, the test app's build/<app>) plus a log of its own
# here. The shared build folders are read-only while the lanes run; the sources must not change meanwhile. Not for
# checks that use a shared slot (the AVD, a Simulator, the iPhone), and not for two checks that build the same test
# app.
# Usage: tools/check-lanes.sh [-j LANES] [-o LOGDIR] check.sh...      (default: 3 lanes, build/check-lanes)
# Prints one line per check, "<name> ok|FAILED <seconds> s", then the total; exits 1 if any failed.
set -euo pipefail
cd "$(dirname "$0")/.."

lanes=3 logs=build/check-lanes
while getopts "j:o:" opt; do
  case $opt in j) lanes=$OPTARG ;; o) logs=$OPTARG ;; *) exit 64 ;; esac
done
shift $((OPTIND - 1))
[[ $# -gt 0 ]] || { echo "usage: tools/check-lanes.sh [-j LANES] [-o LOGDIR] check.sh..." >&2; exit 64; }

export MINATO_PREPARED=1
# Builds only what changed since the last preparation (nothing, when the tree is as it was), before any lane starts.
tools/check-prepare.sh >/dev/null

rm -rf "$logs"; mkdir -p "$logs"
export PATH="$PWD/tools/lane-bin:$PATH" LOGS="$logs"
start=$(date +%s)
printf '%s\n' "$@" | xargs -P "$lanes" -I{} bash -c '
  name=$(basename "{}" .sh); s=$(date +%s)
  if bash "{}" >"$LOGS/$name.out" 2>&1; then rc=0; else rc=$?; fi
  echo "$rc $(( $(date +%s) - s ))" >"$LOGS/$name.rc"'
failed=0
for check in "$@"; do
  name=$(basename "$check" .sh); read -r rc secs <"$logs/$name.rc"
  if [[ $rc == 0 ]]; then echo "$name ok $secs s"; else echo "$name FAILED (rc $rc) $secs s"; failed=1; fi
done
echo "total $(( $(date +%s) - start )) s in $lanes lanes"
exit $failed
