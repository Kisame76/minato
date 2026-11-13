#!/usr/bin/env bash
# The Swift 6 data-race check of the tree's Swift code, which only a compile through SIL gives: the region checks
# ("sending 'x' risks causing data races") run in the SIL passes, so `swiftc -parse` and `-typecheck` pass code that
# a build refuses. Compiles, in this order, and stops at the first module with an error, naming it:
#   - importer/APKKit, every target and its tests (swift build --build-tests, the package's Swift 6 mode), for the Mac;
#   - app/, the Minato target, for the iOS Simulator (arm64) in Debug and in Release (xcodebuild with the project's own
#     settings, unsigned; it boots no Simulator). It needs Minato.xcodeproj (xcodegen generate) and the frameworks the
#     app links in build/ (tools/build-native.sh --app and the other build steps); without them the app is SKIP.
# One line per module: OK, RED with the first error (paths relative to the tree), or SKIP with the reason.
# Exit 0 when all are OK, 1 at the first RED, 2 when one was skipped. Builds go to build/swift-sil-check/.
# Usage: tools/swift-sil-check.sh            the tree's modules
#        tools/swift-sil-check.sh --package <dir>   one Swift package only (its own test)
# SIL_CHECK_JOBS sets the parallel jobs (default 4).
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd -P)
OUT=$ROOT/build/swift-sil-check
JOBS=${SIL_CHECK_JOBS:-4}
skipped=0

# The first error of a build log, its path made relative to the tree and its colors taken out: a compiler's
# "<file>:<line>:<column>: error: ..." first, else the build system's own first error line.
first_error() {
  local plain
  plain=$(sed -e $'s/\x1b\\[[0-9;]*m//g' "$1")
  { grep -m 1 -E '^[^ ]+:[0-9]+:[0-9]+: error: ' <<< "$plain" || grep -m 1 -E '(^|: )error: ' <<< "$plain"; } |
    sed -e "s#$ROOT/##g" -e "s#${ROOT#/private}/##g" -e 's/^[[:space:]]*//'
}

package() { # <package dir> <scratch dir>
  local dir=$1 scratch=$2 name log
  name=$(sed -n -E 's/.*name: *"([^"]+)".*/\1/p' "$dir/Package.swift" | head -n 1)
  [[ -n "$name" ]] || name=$(basename "$dir")
  log=$(mktemp "${TMPDIR:-/tmp}/swift-sil-check.XXXXXX")
  if swift build --build-tests --package-path "$dir" --scratch-path "$scratch" -j "$JOBS" \
      > "$log" 2>&1; then
    echo "OK   $name"
    rm -f "$log"
    return 0
  fi
  mkdir -p "$scratch"
  mv "$log" "$scratch/swift-sil-check.log"
  echo "RED  $name: $(first_error "$scratch/swift-sil-check.log")"
  echo "     the whole log: $scratch/swift-sil-check.log"
  exit 1
}

app() { # <configuration>
  local configuration=$1 log missing="" framework
  [[ -d Minato.xcodeproj ]] || missing="Minato.xcodeproj (run xcodegen generate)"
  for framework in APKRuntime APKConvert MinatoGraphics; do
    [[ -e "build/$framework.xcframework" ]] || missing="${missing:+$missing, }build/$framework.xcframework"
  done
  if [[ -n "$missing" ]]; then
    echo "SKIP app ($configuration): missing $missing"
    skipped=1
    return 0
  fi
  log=$(mktemp "${TMPDIR:-/tmp}/swift-sil-check.XXXXXX")
  if xcodebuild -project Minato.xcodeproj -scheme Minato -configuration "$configuration" \
      -destination 'generic/platform=iOS Simulator' -derivedDataPath "$OUT/app" -jobs "$JOBS" \
      ARCHS=arm64 ONLY_ACTIVE_ARCH=YES CODE_SIGNING_ALLOWED=NO -quiet build > "$log" 2>&1; then
    echo "OK   app ($configuration)"
    rm -f "$log"
    return 0
  fi
  mkdir -p "$OUT"
  mv "$log" "$OUT/app-$configuration.log"
  echo "RED  app ($configuration): $(first_error "$OUT/app-$configuration.log")"
  echo "     the whole log: ${OUT#"$ROOT"/}/app-$configuration.log"
  exit 1
}

if [[ "${1:-}" == --package ]]; then
  [[ -f "${2:-}/Package.swift" ]] || { echo "swift-sil-check: no Package.swift in ${2:-}" >&2; exit 2; }
  # A package of its own builds in its own .build, not in the tree's build/.
  package "$(cd "$2" && pwd)" "$(cd "$2" && pwd)/.build"
  exit 0
fi

package "$ROOT/importer/APKKit" "$OUT/package-APKKit"
app Debug
app Release
exit $((skipped ? 2 : 0))
