#!/usr/bin/env bash
# tools/warn-native-stale.sh: a stale libandroid_runtime.so (its stamp older than a native source) is named on stderr,
# a fresh one and a tree without any native build are silent for the stale case's wording, and the exit code is 0 in
# every case (a warning, not an error).
set -u
cd "$(dirname "$0")/../.."
tool=tools/warn-native-stale.sh
fail=0
check() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
mkdir -p "$root/build/framework-native" "$root/framework/native"
touch -t 202601010000 "$root/build/framework-native/libandroid_runtime.so" "$root/build/framework-native/.built"
touch -t 202601020000 "$root/framework/native/Newer.cpp"
err=$("$tool" "$root" 2>&1 >/dev/null); code=$?
check "stale .so: exit 0" '[ $code -eq 0 ]'
check "stale .so: warning names the script" 'grep -q "tools/build-framework-native.sh" <<<"$err"'
check "stale .so: names the newer source" 'grep -q "framework/native/Newer.cpp" <<<"$err"'
touch -t 202601030000 "$root/build/framework-native/.built"
err=$("$tool" "$root" 2>&1 >/dev/null); code=$?
check "fresh .so: exit 0" '[ $code -eq 0 ]'
check "fresh .so: silent" '[ -z "$err" ]'
rm -rf "$root/build/framework-native"
err=$("$tool" "$root" 2>&1 >/dev/null); code=$?
check "missing native build: exit 0" '[ $code -eq 0 ]'
check "missing native build: warns" 'grep -q "warning" <<<"$err"'
exit $fail
