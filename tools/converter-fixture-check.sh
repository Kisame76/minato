#!/bin/sh
# Converts fixture libraries with the converter and compares the syscalls it rewrote with the count each library has
# to give (fixtures/expected/data-in-text-syscalls.txt: "<file name> <count>" per line). A syscall left in place
# runs on iOS as SIGSYS, which the converter's unit tests cannot see: a word of data or a load that hides a real svc
# shows only here, in a count that is one too low.
#
# Usage: tools/converter-fixture-check.sh <apk-convert> <expected-file> <elf>...
# Prints OK or WRONG per library, exits 1 on any WRONG, 77 (skipped) when the converter or a library is not built.
set -u
if [ "$#" -lt 3 ]; then
    echo "usage: $0 <apk-convert> <expected-file> <elf>..." >&2
    exit 2
fi
convert=$1
expected=$2
shift 2
if [ ! -x "$convert" ]; then
    echo "skipped: $convert is not built"
    exit 77
fi
for elf in "$@"; do
    if [ ! -f "$elf" ]; then
        echo "skipped: $elf is not built, see tools/build-fixtures.sh"
        exit 77
    fi
done
scratch=$(mktemp -d "${TMPDIR:-/tmp}/converter-fixture-check.XXXXXX") || exit 1
trap 'rm -rf "$scratch"' EXIT
failed=0
for elf in "$@"; do
    name=$(basename "$elf")
    want=$(awk -v n="$name" '$1 == n { print $2; exit }' "$expected")
    if [ -z "$want" ]; then
        printf 'WRONG  %s  no expected count in %s\n' "$name" "$expected"
        failed=1
        continue
    fi
    if ! report=$("$convert" convert "$elf" "$scratch/$name.dylib" --sign - 2>&1); then
        printf 'WRONG  %s  the converter failed\n' "$name"
        failed=1
        continue
    fi
    got=$(printf '%s\n' "$report" | sed -n 's/.*rewritten: .*, \([0-9][0-9]*\) syscalls.*/\1/p' | head -1)
    if [ "$got" = "$want" ]; then
        printf 'OK     %s  %s\n' "$name" "$got"
    else
        printf 'WRONG  %s  expected %s, converter reports %s\n' "$name" "$want" "${got:-none}"
        failed=1
    fi
done
exit $failed
