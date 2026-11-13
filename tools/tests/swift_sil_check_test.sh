#!/bin/sh
# Tests tools/swift-sil-check.sh on two small Swift 6 packages of its own: one with a data race that only the SIL
# passes find (a value sent to the main actor and used again: "sending ... risks causing data races"; -typecheck
# passes it, which the test shows first), which must be red with that error named, and the same package without the
# race, which must be OK.
set -u
here=$(cd "$(dirname "$0")" && pwd)
check="$here/../swift-sil-check.sh"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/swift-sil-check.XXXXXX") || exit 1
trap 'rm -rf "$scratch"' EXIT
failed=0

package() { # <dir> <body of the use after the send>
    mkdir -p "$1/Sources/Racy"
    cat > "$1/Package.swift" <<'EOF'
// swift-tools-version: 6.2
import PackageDescription
let package = Package(name: "Racy", platforms: [.macOS("26.0")], targets: [.target(name: "Racy")])
EOF
    cat > "$1/Sources/Racy/Racy.swift" <<EOF
final class Box { var count = 0 }

@MainActor func keep(_ box: Box) { _ = box.count }

func send() async {
    let box = Box()
    await keep(box)
    $2
}
EOF
}

package "$scratch/racy" "box.count += 1"
package "$scratch/clean" "_ = 1"

# The race is one the type checker lets through: only the SIL passes see it.
if ! xcrun swiftc -typecheck -swift-version 6 -parse-as-library "$scratch/racy/Sources/Racy/Racy.swift" \
    > "$scratch/typecheck.out" 2>&1; then
    echo "FAIL the planted race is already a type check error, so it tests nothing:"
    sed 's/^/  | /' "$scratch/typecheck.out"
    failed=1
fi

"$check" --package "$scratch/racy" > "$scratch/racy.out" 2>&1
code=$?
if [ "$code" -ne 1 ] || ! grep -q '^RED  Racy: .*risks causing data races' "$scratch/racy.out"; then
    echo "FAIL the planted race: exit $code, expected 1 and a RED line that names it:"
    sed 's/^/  | /' "$scratch/racy.out"
    failed=1
fi

"$check" --package "$scratch/clean" > "$scratch/clean.out" 2>&1
code=$?
if [ "$code" -ne 0 ] || ! grep -q '^OK   Clean\|^OK   Racy' "$scratch/clean.out"; then
    echo "FAIL the package without the race: exit $code, expected 0 and an OK line:"
    sed 's/^/  | /' "$scratch/clean.out"
    failed=1
fi

[ "$failed" -eq 0 ] && echo "swift-sil-check test: fine"
exit "$failed"
