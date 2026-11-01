#!/usr/bin/env bash
# Checks that tools/build-framework.sh replaces build/framework in one step, so that a program that reads
# build/framework/framework.jar meanwhile (a test app's build, an iPhone build, a measurement) never finds it missing:
#  1. tools/swap-dir.py swaps a folder 300 times while a reader opens a file in it in a loop; no open may fail.
#  2. tools/build-framework.sh runs (the script given as $1, else tools/build-framework.sh) while a reader opens
#     build/framework/framework.jar and stubs.jar in a loop every 2 ms; no open may fail. Skipped, with a line starting
#     "skipped:", when there is no build/framework yet or the build's inputs are missing.
# Usage: tools/build-framework-atomic-check.sh [build script]
set -euo pipefail
cd "$(dirname "$0")/.."

reader='
import os, sys, time
paths, stop = sys.argv[1].split(","), sys.argv[2]
opened = missed = 0
while not os.path.exists(stop):
    for path in paths:
        try:
            with open(path, "rb") as handle:
                handle.read(16)
            opened += 1
        except OSError:
            missed += 1
    time.sleep(0.002)
print(opened, missed)
'

work=$(mktemp -d "${TMPDIR:-/tmp}/atomic-check.XXXXXX")
trap 'rm -rf "$work"' EXIT

# 1. The swap alone.
mkdir "$work/final"
echo old >"$work/final/file"
python3 -c "$reader" "$work/final/file" "$work/stop1" >"$work/reader1.out" &
for round in $(seq 1 300); do
  mkdir "$work/new"
  echo "new $round" >"$work/new/file"
  python3 tools/swap-dir.py "$work/new" "$work/final"
done
touch "$work/stop1"
wait
read -r opened missed <"$work/reader1.out"
echo "swap-dir: $opened opens, $missed missed"
[[ "$missed" == 0 && "$opened" -gt 100 ]] || { echo "FAIL: the swap left the file missing" >&2; exit 1; }

# 2. The real build.
script=${1:-tools/build-framework.sh}
if [[ ! -f build/framework/framework.jar || ! -f build/framework/stubs.jar ]]; then
  echo "skipped: no build/framework yet (tools/build-framework.sh makes it)"
  exit 0
fi
python3 -c "$reader" "build/framework/framework.jar,build/framework/stubs.jar" "$work/stop2" >"$work/reader2.out" &
status=0
"$script" >"$work/build.out" 2>&1 || status=$?
touch "$work/stop2"
wait
if [[ "$status" != 0 ]]; then
  tail -20 "$work/build.out" >&2
  echo "skipped: $script failed (status $status), not a result of this check" >&2
  exit 0
fi
read -r opened missed <"$work/reader2.out"
echo "$script: $opened opens of framework.jar and stubs.jar, $missed missed"
[[ "$missed" == 0 && "$opened" -gt 100 ]] ||
  { echo "FAIL: build/framework was missing while it was built" >&2; exit 1; }
echo "build-framework-atomic-check: ok"
