#!/usr/bin/env bash
# Checks that a converter refusal longer than a kilobyte reaches the app whole (app/Sources/Signing/ConverterError.swift;
# a Unity test APK's x18 refusal was cut at the 1,024-byte buffer): builds APKConvert's library for macOS into
# build/native/macos-app, writes an ELF whose first of 40 LOAD segments does not fit 4 KiB pages, so that the refusal
# lists all 40, and runs app/HostChecks/ConverterMessage/main.swift with the app's buffer against a 64 KiB one.
# Usage: tools/converter-message-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-app
OUT=build/converter-message-check
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apkconv >/dev/null
rm -rf "$OUT"
mkdir -p "$OUT"
python3 - "$OUT/long-refusal.so" <<'PY'
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
  app/Sources/Signing/ConverterError.swift app/HostChecks/ConverterMessage/main.swift -o "$OUT/converter-message-check"
"$OUT/converter-message-check" "$OUT/long-refusal.so" "$OUT/container.dylib"
echo "converter message check: fine"
