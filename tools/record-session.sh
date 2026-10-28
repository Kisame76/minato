#!/usr/bin/env bash
# A recorded session (ADR-0041 d), the App's part): builds the benchmark build (Release,
# APK_EMBED_BENCHMARK=YES) as tools/benchmark.sh does, installs it, starts it with -recordSession <package>
# without a debugger and fetches the app's tmp/benchmark/session-<package>/ to build/record-session/<package>/
# (build/record-session-simulator/ for the Simulator):
#   drawables.csv  frame,next_drawable_ns,presented_ns: each drawable the layer gave, in order, so row k is
#                  the app's frame k; presented_ns 0 for one that never reached the screen; CLOCK_UPTIME_RAW ns
#   session.csv    the conditions, one row: thermal state, Low Power Mode, battery, debugger, the layer's
#                  values, lost drawables (past 7200), unanswered presented handlers and the exit status
#   error          only when something is off: the reasons, "; " apart
# The app must be in the library already, imported as any app. Minato waits for a nominal thermal state, starts
# it with its recording on and ends once the app has ended by itself, at most after 600 s. The Simulator's layer
# records nothing: there drawables.csv has only its header.
# Each --guest-env KEY=VALUE adds an entry to the app's guest environment, as -benchmarkGuestEnvironment, which the
# benchmark build takes (BenchmarkMode.guestEnvironment), such as MINATO_PRESENTS_WITHOUT_TIME=1. Keys follow the
# Debug guest environment's rule (DebugGuestEnvironment): MINATO_ and capitals, digits or "_", so an entry cannot
# change what the runtime sets. --dry-run prints the app's launch arguments and ends before building.
# Usage: tools/record-session.sh [--simulator] [--device <name or identifier>] [--no-build]
#                                [--guest-env MINATO_KEY=VALUE]... [--dry-run] <package>
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
SIMULATOR=0
WANTED_DEVICE=""
BUILD=1
DRY_RUN=0
PACKAGE=""
LAUNCH=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --simulator) SIMULATOR=1; shift ;;
    --device) WANTED_DEVICE="$2"; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --guest-env)
      if [[ ! "${2:-}" =~ ^MINATO_[A-Z0-9_]+= ]]; then
        echo "--guest-env takes MINATO_KEY=VALUE, the key of capitals, digits and \"_\": ${2:-nothing}" >&2
        exit 2
      fi
      LAUNCH+=(-benchmarkGuestEnvironment "$2"); shift 2 ;;
    -*) echo "Unknown argument $1" >&2; exit 2 ;;
    *) PACKAGE="$1"; shift ;;
  esac
done
# The package names a folder that is emptied below.
if [[ ! "$PACKAGE" =~ ^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$ ]]; then
  echo "Usage: tools/record-session.sh [--simulator] [--device <name or identifier>] [--no-build]" \
    "[--guest-env MINATO_KEY=VALUE]... [--dry-run] <package>" >&2
  exit 2
fi
LAUNCH=(-recordSession "$PACKAGE" ${LAUNCH[@]+"${LAUNCH[@]}"})
if [[ "$DRY_RUN" == 1 ]]; then
  echo "launch arguments: ${LAUNCH[*]}"
  exit 0
fi
# The iPhone paired and reachable, by cable or over the local network, checked before the builds.
[[ "$SIMULATOR" == 1 ]] || python3 tools/require-device.py "$WANTED_DEVICE" || exit 1
RESULTS=build/record-session
[[ "$SIMULATOR" == 0 ]] || RESULTS=build/record-session-simulator
mkdir -p "$RESULTS"

if [[ "$BUILD" == 1 ]]; then
  # The benchmark build embeds MinatoBench.framework and the APKs (tools/embed-benchmark.sh).
  tools/build-bench.sh
  tools/build-bench.sh --ios
  COMMIT=$(git rev-parse --short HEAD)
  [[ -z "$(git status --porcelain)" ]] || COMMIT+=-dirty
  SETTINGS="APK_EMBED_BENCHMARK=YES MINATO_COMMIT=$COMMIT"
fi
if [[ "$SIMULATOR" == 1 ]]; then
  if [[ "$BUILD" == 1 ]]; then
    CONFIGURATION=Release BUILD_SETTINGS="$SETTINGS ARCHS=arm64 ONLY_ACTIVE_ARCH=YES" INSTALL_ONLY=1 \
      tools/sim.sh --headless ${WANTED_DEVICE:+"$WANTED_DEVICE"}
  fi
  read -r DEVICE_ID _ < <(python3 tools/find-simulator.py "$WANTED_DEVICE") || true
else
  if [[ "$BUILD" == 1 ]]; then
    CONFIGURATION=Release BUILD_SETTINGS="$SETTINGS" INSTALL_ONLY=1 tools/deploy.sh ${WANTED_DEVICE:+"$WANTED_DEVICE"}
  fi
  read -r DEVICE_ID _ < <(python3 tools/find-device.py "$WANTED_DEVICE") || true
fi
if [[ -z "${DEVICE_ID:-}" ]]; then
  echo "No device found." >&2
  exit 1
fi

OUT="$RESULTS/$PACKAGE"
rm -rf "${OUT:?}" "$OUT.console"
started=$(date +%s)
# Waits until the app has ended: the thermal wait, the session and the files.
if [[ "$SIMULATOR" == 1 ]]; then
  xcrun simctl launch --console --terminate-running-process "$DEVICE_ID" "$BUNDLE_ID" \
    "${LAUNCH[@]}" >"$OUT.console" 2>&1 || true
  container=$(xcrun simctl get_app_container "$DEVICE_ID" "$BUNDLE_ID" data)
  cp -R "$container/tmp/benchmark/session-$PACKAGE" "$OUT" 2>/dev/null || true
else
  # "--" ends devicectl's options. Without --start-stopped no debugger attaches.
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --console --timeout 900 \
    "$BUNDLE_ID" -- "${LAUNCH[@]}" >"$OUT.console" 2>&1 || true
  # Only out of the app's own data container, and never with --remove-existing-content.
  xcrun devicectl device copy from --device "$DEVICE_ID" --domain-type appDataContainer \
    --domain-identifier "$BUNDLE_ID" --source "tmp/benchmark/session-$PACKAGE" --destination "$OUT" \
    >>"$OUT.console" 2>&1 || true
fi
if [[ ! -f "$OUT/session.csv" ]]; then
  echo "FAILED   no session.csv, see $OUT.console" >&2
  exit 1
fi
python3 - "$OUT" "$(($(date +%s) - started))" <<'PY'
import csv, pathlib, sys
folder = pathlib.Path(sys.argv[1])
row = next(csv.DictReader(open(folder / "session.csv")))
drawables = sum(1 for _ in open(folder / "drawables.csv")) - 1
error = folder / "error"
note = f"  ({error.read_text().strip()})" if error.exists() else ""
status = row["exit_status"] or "none"
print(f"{row['package']}: {drawables} drawables, exit status {status}, in {sys.argv[2]} s; files in {folder}{note}")
PY
