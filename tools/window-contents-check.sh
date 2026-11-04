#!/usr/bin/env bash
# ADR-0043 b5's step f: whether every window that an app draws by the CPU shows what it posted, in the Simulator or,
# with --device, on the iPhone. It runs tools/java-start-check.sh's pixel round (PIXELS) on a Debug build:
# views-test (RGBX_8888, text and a Button drawn in software), dialog-test (a dimmed window over the activity's),
# second-windows-test (sub-windows, toasts, alpha), activity-flow-test (translucent windows over Main's) and
# window-formats-test (RGBA_8888, RGBX_8888 and RGB_565 in turn), each through its start, Home and the return. At each
# checkpoint the screenshot must show each window's last buffer where nothing covers it (tools/window_pixels.py,
# within 3 per channel after the sRGB to Display P3 conversion), and window-formats-test's log must be Android 15's.
# With --device a second run of views-test draws its windows by drawables (MINATO_WINDOW_BY_DRAWABLES's Debug switch,
# SessionCheck's window-by-drawables), and the check prints both runs' physical footprint, the changelog's number.
# Results: build/window-contents-check/<contents|drawables>/<app>/<checkpoint>/ (screen.png, dump/, compare.txt),
# never in the repo; they are deleted with the next run.
# Usage: tools/window-contents-check.sh [--device] [--dry-run] [simulator or iPhone name or identifier]
set -euo pipefail
cd "$(dirname "$0")/.."

device=()
dry_run=no
while [[ $# -gt 0 ]]; do
  case "$1" in
    --device) device=(--device) ;;
    --dry-run) dry_run=yes ;;
    -*) echo "usage: tools/window-contents-check.sh [--device] [--dry-run] [simulator or iPhone]" >&2; exit 2 ;;
    *) break ;;
  esac
  shift
done
apps="views-test dialog-test second-windows-test activity-flow-test window-formats-test"
out=build/window-contents-check
if [[ "$dry_run" == yes ]]; then
  echo "window contents check: ${device[*]:-the Simulator}"
  for app in $apps; do
    if [[ "$app" == window-formats-test ]]; then
      echo "  $app: checkpoints RGBA_8888, RGBX_8888, RGB_565, then Home, Continue and return; its log vs Android 15"
    else
      echo "  $app: checkpoints start, then Home, Continue and return"
    fi
  done
  [[ ${#device[@]} -eq 0 ]] || echo "  views-test again by drawables, for the footprint"
  echo "  about $(( ${#device[@]} > 0 ? 18 : 12 )) minutes after the Debug build; results in $out"
  exit 0
fi

rm -rf "${out:?}"
mkdir -p "$out"
failed=0
PIXELS="$out/contents" tools/java-start-check.sh ${device[@]+"${device[@]}"} "$@" || failed=1
if [[ ${#device[@]} -gt 0 ]]; then
  PIXELS="$out/drawables" PIXELS_DRAWABLES=1 ONLY=views-test \
    tools/java-start-check.sh ${device[@]+"${device[@]}"} "$@" || failed=1
  # The footprint once the app has settled: the largest sample of each run's views-test.
  python3 - "$out/contents/views-test/footprint-log" "$out/drawables/views-test/footprint-log" <<'PY' || failed=1
import sys


def peak(path):
    try:
        values = [int(line.split()[2]) for line in open(path) if line.split()[1:2] == ["footprint"]]
    except OSError:
        return None
    return max(values) if values else None


contents, drawables = peak(sys.argv[1]), peak(sys.argv[2])
if contents is None or drawables is None:
    print("FAIL  views-test: no footprint samples in one of the runs")
    sys.exit(1)
print(f"note  views-test's footprint at its peak: {contents / 1e6:.1f} MB by contents, {drawables / 1e6:.1f} MB by "
      f"drawables, {(drawables - contents) / 1e6:.1f} MB less by contents")
PY
fi
echo "window contents check: $([[ $failed == 0 ]] && echo ok || echo FAILED)"
exit "$failed"
