#!/bin/sh
# Tests tools/converter-fixture-check.sh with a stand-in converter that prints the report line the real one prints.
# Usage: converter_fixture_check_test.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
check="$here/../converter-fixture-check.sh"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/cfc.XXXXXX") || exit 1
trap 'rm -rf "$scratch"' EXIT
failed=0

# A converter that reports $SYSCALLS rewritten syscalls for every library.
cat > "$scratch/convert" <<'S'
#!/bin/sh
[ "$1" = convert ] || exit 2
: > "$3"
echo "$2 -> $3 (1 bytes)"
echo "  dynamic, 8 bytes of code scanned; data words left as they are: 0 by loaded literals, 0 by indexed tables"
echo "  rewritten: 0 thread pointer reads, 0 writes, ${SYSCALLS:-3} syscalls, 0 cache type reads (0 bytes of stubs)"
S
chmod +x "$scratch/convert"
: > "$scratch/libone.so"; : > "$scratch/libtwo.so"
printf 'libone.so 3\nlibtwo.so 3\n' > "$scratch/expected.txt"

expect() { # name status pattern command...
    name=$1; want=$2; pattern=$3; shift 3
    out=$("$@" 2>&1); status=$?
    if [ "$status" -eq "$want" ] && printf '%s\n' "$out" | grep -Eq "$pattern"; then echo "ok   $name"
    else
        echo "FAIL $name (status $status, wanted $want, pattern $pattern)"
        printf '%s\n' "$out" | sed 's/^/     /'
        failed=1
    fi
}

SYSCALLS=3 expect "every library with the expected count is OK" 0 "^OK +libone.so +3" \
    "$check" "$scratch/convert" "$scratch/expected.txt" "$scratch/libone.so" "$scratch/libtwo.so"
SYSCALLS=2 expect "one syscall left unrewritten is WRONG" 1 "^WRONG +libone.so +expected 3, converter reports 2" \
    "$check" "$scratch/convert" "$scratch/expected.txt" "$scratch/libone.so"
SYSCALLS=4 expect "one more than expected is WRONG too" 1 "^WRONG +libone.so +expected 3, converter reports 4" \
    "$check" "$scratch/convert" "$scratch/expected.txt" "$scratch/libone.so"
: > "$scratch/libthree.so"
expect "a library with no expected count is WRONG" 1 "^WRONG +libthree.so +no expected count" \
    "$check" "$scratch/convert" "$scratch/expected.txt" "$scratch/libthree.so"
expect "a library that is not built is skipped (77)" 77 "^skipped:" \
    "$check" "$scratch/convert" "$scratch/expected.txt" "$scratch/nothere.so"
expect "a converter that is not built is skipped (77)" 77 "^skipped:" \
    "$check" "$scratch/nothere" "$scratch/expected.txt" "$scratch/libone.so"
cat > "$scratch/refuses" <<'S'
#!/bin/sh
echo "apk-convert: refused" >&2; exit 1
S
chmod +x "$scratch/refuses"
expect "a converter that refuses a library is WRONG" 1 "^WRONG +libone.so +the converter failed" \
    "$check" "$scratch/refuses" "$scratch/expected.txt" "$scratch/libone.so"
exit $failed
