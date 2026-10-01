#!/usr/bin/env bash
# Checks the host aapt2 of tools/build-aapt2.sh (ADR-0039 b1): compiles tools/aapt2/check/res, with a PNG made here,
# links it with a manifest that needs no framework-res, and reads the table back with `aapt2 dump resources`. Each
# expected line is what aapt2's Debug.cpp prints for these values. Linking against framework-res, and Android's own
# reading of the result, come with b2.
# Usage: tools/aapt2-check.sh
# Results: build/aapt2-check/
set -euo pipefail
cd "$(dirname "$0")/.."

AAPT2=build/aapt2/aapt2
OUT=build/aapt2-check
CHECK=tools/aapt2/check
if [[ ! -x "$AAPT2" ]]; then
  echo "No $AAPT2: run tools/build-aapt2.sh first." >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/res"
cp -R "$CHECK/res/." "$OUT/res/"
# A 2x2 RGBA PNG, which aapt2 crunches.
mkdir -p "$OUT/res/drawable"
python3 - "$OUT/res/drawable/icon.png" <<'PY'
import struct, sys, zlib
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
rows = b"".join(b"\x00" + bytes([30, 90, 168, 255]) * 2 for _ in range(2))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY

failed=0
run() {
  local what=$1; shift
  if ! output=$("$@" 2>&1); then
    echo "FAILED   $what: $output"
    exit 1
  fi
}
run "aapt2 compile" "$AAPT2" compile --dir "$OUT/res" -o "$OUT/compiled.zip"
run "aapt2 link" "$AAPT2" link -o "$OUT/check.apk" --manifest "$CHECK/AndroidManifest.xml" "$OUT/compiled.zip"
echo "ok       compile and link: $OUT/check.apk"

dump=$("$AAPT2" dump resources "$OUT/check.apk" | sed 's/^ *//')
printf '%s\n' "$dump" >"$OUT/dump.txt"
expect() {
  local pattern=$1
  if ! grep -qE -- "$pattern" <<<"$dump"; then
    echo "FAILED   dump lacks: $pattern"
    failed=$((failed + 1))
  fi
}
expect '^Package name=io\.github\.kisame76\.minato\.check\.res id=7f$'
expect '^resource 0x7f[0-9a-f]{2}0000 string/app_name$'
expect '^\(\) "Minato check"$'
expect '^\(de\) "Minato-Prüfung"$'
expect '^\(\) "Hello, %1\$s!"$'
expect '^resource 0x7f[0-9a-f]{6} plurals/apps$'
expect '^\(\) #ff1e5aa8$'
expect '^\(\) 42$'
# ComplexToString in ResourceValues.cpp prints a dimension with %f.
expect '^\(\) 16\.000000dp$'
expect '^resource 0x7f[0-9a-f]{6} drawable/icon$'
expect '^resource 0x7f[0-9a-f]{6} layout/main$'
expect '^resource 0x7f[0-9a-f]{6} raw/data$'
if [[ $failed -gt 0 ]]; then
  echo "aapt2 check: $failed lines missing, see $OUT/dump.txt"
  exit 1
fi
echo "ok       dump: every resource with its values, see $OUT/dump.txt"
echo "aapt2 check: fine"
