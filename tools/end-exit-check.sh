#!/usr/bin/env bash
# Checks that game mode's code ends its process with _exit, never with exit (apkrt.h, the end handler): after a
# program's end its stopped threads may still finish host code, and exit's static destructors would destroy the
# runtime's mutexes under them, so that one of them aborts with "mutex lock failed" (a guest's kill of itself in a
# benchmark run, 2026-10-27). The files are app/Sources/Session and app/Benchmark; comment lines do not count.
# Usage: tools/end-exit-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

found=$(grep -rnE '(^|[^_[:alnum:].])exit\(' app/Sources/Session app/Benchmark | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' ||
  true)
if [[ -n "$found" ]]; then
  echo "FAIL: exit in game mode's code, where the process ends with _exit:"
  echo "$found"
  exit 1
fi
echo "end-exit-check: game mode's code ends with _exit only"
