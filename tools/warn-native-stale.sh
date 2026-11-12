#!/usr/bin/env bash
# Warns (never fails) when build/framework-native is older than its sources or missing, so a framework.jar built on
# its own is not mistaken for a whole guest framework: the jar calls natives (WindowSurface.isAlive, say) that an
# older libandroid_runtime.so does not have, and the first start dies with UnsatisfiedLinkError.
# tools/build-framework.sh runs it as its last step.
# Usage: tools/warn-native-stale.sh [<repo root>]
cd "$(dirname "$0")/.."
root="${1:-$PWD}"
log="${TMPDIR:-/tmp}/warn-native.$$"
if ! python3 tools/check-guest-fresh.py --only build/framework-native "$root" >/dev/null 2>"$log"; then
  echo "build-framework: warning: the native side of the framework is not that of this tree" \
    "(the jar just built may call natives it lacks); run tools/build-framework-native.sh. Details:" >&2
  sed 's/^/  /' "$log" >&2
fi
rm -f "$log"
exit 0
