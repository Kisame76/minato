#!/usr/bin/env bash
# Imports one file of every format through the library's own entry, on the Mac. It builds the test apps the files are
# made of (java-test, obb-test, splits-test as a bundletool set, packs-test with its asset packs), has
# tools/make-format-files.py make a plain APK, bundletool's .apks, a zip of APKs, an .xapk with OBBs, an .apkm, a .apks
# with asset packs, a loose set with its folder and an APK with OBBs to pick apart, into build/format-files-check/files,
# and runs only FormatFilesTests of importer/APKKit with MINATO_FORMAT_FILES set, in a SwiftPM scratch folder of its own
# (build/swiftpm-format-files). The suite is skipped in a plain `swift test`, which has no such variable, so this check
# is what runs it; it fails when the suite was skipped or ran fewer tests than it has.
# Needs tools/build-art.sh, tools/build-framework-res.sh and tools/build-aapt2.sh first, as tools/splits-check.sh does.
# Usage: tools/format-files-check.sh
# Prints "format-files-check ok" or "format-files-check FAIL" with the reason; the swift test log is
# build/format-files-check/swift-test.log.
set -euo pipefail
cd "$(dirname "$0")/.."

fail() {
  echo "format-files-check FAIL: $1"
  exit 1
}

OUT=build/format-files-check
mkdir -p "$OUT"
for step in "tools/build-framework-native.sh" "tools/build-framework.sh" "tools/build-splits-test.sh" \
    "tools/build-packs-test.sh" "tools/build-java-test-app.sh java-test" "tools/build-java-test-app.sh obb-test"; do
  # shellcheck disable=SC2086
  $step >"$OUT/build.log" 2>&1 || { tail -n 20 "$OUT/build.log"; fail "$step failed"; }
done
python3 tools/make-format-files.py "$OUT/files" >"$OUT/make.log" 2>&1 || {
  cat "$OUT/make.log"
  fail "make-format-files.py failed"
}
if grep -q '^missing' "$OUT/make.log"; then cat "$OUT/make.log"; fail "a source file is missing"; fi

MINATO_FORMAT_FILES="$PWD/$OUT/files" swift test --package-path importer/APKKit \
    --scratch-path build/swiftpm-format-files --filter FormatFilesTests >"$OUT/swift-test.log" 2>&1 || {
  grep -E "✘|error:|Expectation failed" "$OUT/swift-test.log" | head -n 20
  fail "FormatFilesTests failed"
}
grep -q "skipped" "$OUT/swift-test.log" && fail "FormatFilesTests was skipped"
ran='Test run with ([7-9]|[1-9][0-9]+) tests? '
grep -Eq "$ran" "$OUT/swift-test.log" || fail "FormatFilesTests ran fewer than 7 tests"
echo "format-files-check ok"
