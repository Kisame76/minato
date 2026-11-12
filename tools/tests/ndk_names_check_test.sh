#!/usr/bin/env bash
# tools/ndk-names-check.sh against a fake NDK (a llvm-nm that reads the names from the file) and fake built libraries:
# equal names pass (exit 0); a library that has more names than its stub passes; a stub name that the library lacks is
# WRONG (exit 1, the name is printed); a library that is not built is SKIP (exit 77, never OK); libc++.so is left out
# whether built or not; a WRONG wins over a SKIP; no NDK at all is SKIP too.
# Usage: tools/tests/ndk_names_check_test.sh
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/ndk-names-test.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
failures=0
TOOLCHAIN="$scratch/ndk/toolchains/llvm/prebuilt/darwin-x86_64"
STUBS="$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/35"
LIBS="$scratch/lib"
fresh() {
  rm -rf "$scratch/ndk" "$LIBS"; mkdir -p "$TOOLCHAIN/bin" "$STUBS" "$LIBS"
  cat > "$TOOLCHAIN/bin/llvm-nm" <<'NM'
#!/usr/bin/env bash
# the fake: one name per line in the file, optionally with @version; prints them as llvm-nm -D --defined-only does
file="${@: -1}"
while read -r name; do [[ -n "$name" ]] && echo "0000000000001000 T $name"; done < "$file"
NM
  chmod +x "$TOOLCHAIN/bin/llvm-nm"
  printf 'alpha\nbeta@@V1\n' > "$STUBS/libone.so"; printf 'alpha\nbeta@@V1\nextra\n' > "$LIBS/libone.so"
  printf 'gamma\n' > "$STUBS/libtwo.so"; printf 'gamma\n' > "$LIBS/libtwo.so"
  printf 'cxx\n' > "$STUBS/libc++.so"
}
run() { NDK_NAMES_LIBDIRS="$LIBS" ANDROID_NDK="$scratch/ndk" "$here/ndk-names-check.sh" 2>&1; }
expect() {  # expect <name> <exit status> <pattern in the output> [<pattern that must not be there>]
  local name="$1" want="$2" pattern="$3" out status
  out="$(run)"; status=$?
  local absent="${4:-}"
  if [[ $status -eq $want ]] && grep -q -E "$pattern" <<< "$out" &&
     { [[ -z "$absent" ]] || ! grep -q -E "$absent" <<< "$out"; }; then
    echo "ok   $name"
  else echo "FAIL $name (exit $status, wanted $want, pattern: $pattern)"; failures=$((failures + 1)); fi
}
fresh; expect "equal names pass, and libc++.so is left out although it is not built" 0 "0 WRONG, 0 SKIP" "libc\+\+"
fresh; printf 'alpha\n' > "$LIBS/libone.so"
expect "a stub name the library lacks is WRONG and named" 1 "WRONG +libone.so.*beta"
fresh; printf 'alpha\nbeta@@V1\nextra\ninvented\n' > "$STUBS/libone.so"
expect "a name added to the stub is WRONG and named" 1 "WRONG +libone.so.*invented"
fresh; rm "$LIBS/libtwo.so"
expect "a library that is not built is SKIP, never OK" 77 "SKIP +libtwo.so" "OK +libtwo.so"
fresh; rm "$LIBS/libtwo.so"; printf 'alpha\n' > "$LIBS/libone.so"
expect "a WRONG wins over a SKIP" 1 "WRONG +libone.so"
fresh; printf 'cxx\n' > "$LIBS/libc++.so"; printf 'cxx\nnot_in_the_stub\n' > "$STUBS/libc++.so"
expect "libc++.so is left out even when it is built and differs" 0 "0 WRONG, 0 SKIP" "libc\+\+"
fresh; rm -rf "$scratch/ndk"
expect "no NDK is SKIP" 77 "SKIP.*no NDK"
echo "$failures failed"; exit $((failures != 0))
