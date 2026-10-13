#!/usr/bin/env bash
# Checks the import's checker over the converter's check mode (app/Sources/Signing/ELFChecker.swift, ADR-0055 a2):
# builds APKConvert's library for macOS into build/native/macos-app, writes an ELF whose first of 40 LOAD segments does
# not fit 4 KiB pages, whose reasons take more than a small first buffer, and runs app/HostChecks/ELFChecker/main.swift
# with the real converter and with converters that answer sizes the checker must not trust.
# Usage: tools/elf-checker-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-app
OUT=build/elf-checker-check
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apkconv >/dev/null
rm -rf "$OUT"
mkdir -p "$OUT"
python3 - "$OUT/refused.so" <<'PY'
import struct, sys
count = 40
headers = b"".join(
    struct.pack("<IIQQQQQQ", 1, 5, 0x4000 * i, 0x4000 * i, 0x4000 * i, 0x100, 0x100, 0x100 if i == 0 else 0x4000)
    for i in range(count))
header = bytearray(64)
header[0:7] = bytes([0x7F, ord("E"), ord("L"), ord("F"), 2, 1, 1])
# ET_DYN, EM_AARCH64, version 1, no entry, program headers at 64, no section headers.
struct.pack_into("<HHIQQQIHHHHHH", header, 16, 3, 183, 1, 0, 64, 0, 0, 64, 56, count, 64, 0, 0)
data = bytes(header) + headers
open(sys.argv[1], "wb").write(data + bytes(0x4000 * count + 0x100 - len(data)))
PY
# The macos preset builds the library with AddressSanitizer, as the program must be.
swiftc -sanitize=address -module-cache-path "$OUT/modules" -I importer/converter/capi/APKConvert -L "$NATIVE/importer/converter" -lapkconv \
  app/Sources/Signing/ConverterError.swift app/Sources/Signing/ELFChecker.swift app/HostChecks/ELFChecker/main.swift \
  -o "$OUT/elf-checker-check"
"$OUT/elf-checker-check" "$OUT/refused.so"
echo "ELF checker check: fine"
