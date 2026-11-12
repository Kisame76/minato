#!/usr/bin/env bash
# Checks that a Release build of Minato carries none of the Debug-only check paths: the session check (SessionCheck and
# its tmp/session-check folder), its requests at launch (MINATO_CHECK_REQUESTS) and as minato://check URLs
# (CheckRequests), its console (MINATO_CHECK_CONSOLE, CheckConsole's line prefixes) and the Debug guest environment
# (tmp/guest-environment, DebugGuestEnvironment). It reads the app's executable and every framework binary in it with
# strings and nm. With a Debug build as a second argument it checks the other direction too: each marker must be in the
# Debug build, so a marker that no build carries (renamed code, or a short literal the compiler keeps inside the
# instructions) shows up instead of passing unseen.
# Usage: tools/release-debug-strings-check.sh <Release Minato.app> [<Debug Minato.app>]
set -uo pipefail

# Literals of 16 bytes and more (Swift keeps shorter ones inside the code, where strings cannot see them) and the type
# names of the Debug-only code, as nm and the Swift metadata spell them.
MARKERS=(MINATO_CHECK_REQUESTS MINATO_CHECK_CONSOLE "minato-check-event " "minato-guest-log " "minato-guest-out "
  guest-environment SessionCheck CheckRequests CheckConsole CheckEventPrivacy DebugGuestEnvironment)

# Every Mach-O file of app $1: its executable and the frameworks' and dylibs' binaries.
binaries() {
  find "$1" -type f \( -perm -u+x -o -name '*.dylib' \) -print0 2>/dev/null |
    while IFS= read -r -d '' file; do
      file -b "$file" | grep -q 'Mach-O' && printf '%s\n' "$file"
    done
}

# The markers of $2 found in app $1, one "<marker> <file in the app>" per hit.
hits() {
  local app=$1 file text marker
  while IFS= read -r file; do
    text=$( { strings -a "$file"; nm -U "$file" 2>/dev/null; } )
    for marker in "${MARKERS[@]}"; do
      if grep -q -F -- "$marker" <<< "$text"; then printf '%s\t%s\n' "$marker" "${file#"$app"/}"; fi
    done
  done < <(binaries "$app")
}

[[ $# -ge 1 && -d "$1" ]] || { echo "usage: $0 <Release Minato.app> [<Debug Minato.app>]" >&2; exit 2; }
failed=0
release=$(hits "$1")
if [[ -n "$release" ]]; then
  echo "FAIL  Debug-only code in the Release build:"
  sed 's/^/      /' <<< "$release"
  failed=1
else
  echo "ok    the Release build carries none of the ${#MARKERS[@]} Debug-only markers ($(binaries "$1" | wc -l |
    tr -d ' ') binaries read)"
fi
if [[ $# -ge 2 ]]; then
  [[ -d "$2" ]] || { echo "no Debug app at $2" >&2; exit 2; }
  debug=$(hits "$2" | cut -f 1 | sort -u)
  missing=0
  for marker in "${MARKERS[@]}"; do
    grep -q -x -F -- "$marker" <<< "$debug" && continue
    echo "FAIL  the Debug build lacks the marker \"$marker\": the check would not see it in a Release build either"
    missing=1
  done
  [[ $missing == 1 ]] && failed=1 || echo "ok    the Debug build carries all ${#MARKERS[@]} markers"
fi
exit $failed
