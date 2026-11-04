#!/usr/bin/env bash
# M6 c's E6 in one command: what compiling an app costs and brings on a device, through Minato's own compile mode
# (ADR-0053, E5). For compute-app-test (compute-bench's loops in its Application) and godot-bench-gles (the larger
# dex), tools/java-start-check.sh's COMPILE round imports the app, starts it plain, leaves it by Quit so that the
# library asks for compile mode, lets compile mode run dex2oat in a process of its own (GuestStart(compile:)'s
# program), lets the library sign and keep the code at its next start, and starts the app again with its compiled code.
# The Debug build's events carry dex2oat's peak footprint and the sizes and the time of the library's taking
# (app/Sources/Session/CompileMode.swift, LibraryModel.swift).
# The CSV, build/compile-measure/<UTC date and time>.csv, holds per app: dex_mb (its classes*.dex), plain_s, compiled_s
# (the tap to the app's last line, the fetches' delay included), their ratio, dex2oat_s and per MB of dex, peak_mb (the
# peak physical footprint of the compile process) and per MB of dex, odex_bytes, vdex_bytes, container_bytes (the signed
# container the library keeps) and take_s (the library's converting, signing and keeping). No device name or identifier
# goes into it. A start that failed leaves its cells empty and the script's exit status 1.
# On the iPhone (--device) the app must stay unlocked and the sitting needs the user's OK; without it, the Simulator.
# Needs what tools/java-start-check.sh needs, and build/godot/godot-bench-gles.apk (tools/build-godot.sh).
# Usage: tools/compile-measure.sh [--device] [<device or simulator name>]
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=build/compile-measure
mkdir -p "$OUT"
CSV=$OUT/$(date -u +%Y%m%dT%H%M%SZ).csv
LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT
status=0
COMPILE=1 tools/java-start-check.sh "$@" 2>&1 | tee "$LOG" || status=$?
grep '^csv,' "$LOG" | sed 's/^csv,//' > "$CSV"
echo "wrote $CSV:"
cat "$CSV"
exit "$status"
