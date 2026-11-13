#!/bin/sh
# The checks of the public texts, one command for the release candidate: public-numbers-check.sh,
# docs-paths-check.sh, ndk-names-check.sh and license-check-repo-test.py, one after another, each on the tree.
# Prints one line per check (OK, WRONG or SKIP, its name and its last line of output), the totals, and, when a check is
# red, the first red one by name. Run the named check alone for its full output.
#   OK     the check exited 0
#   WRONG  it exited 1 or anything but 0 and 77, or it is not in the tree
#   SKIP   it exited 77 (not built, no NDK, no sources): never counted as OK
# Exit 0 when all four are OK, 1 when any is WRONG, 77 when none is WRONG but any is SKIP.
# Usage: tools/docs-check.sh [tree]   (the repo root, default: the one this script lives in; read only)
# The scan for local paths, names and addresses (history and tree) is not part of it: it lives outside the repo.
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=${1:-$(cd "$here/.." && pwd)}
if [ ! -d "$tree/tools" ]; then
    echo "usage: $0 [tree]" >&2
    exit 2
fi
ok=0
wrong=0
skip=0
first_red=
for name in public-numbers-check.sh docs-paths-check.sh ndk-names-check.sh license-check-repo-test.py; do
    script="$tree/tools/$name"
    if [ ! -f "$script" ]; then
        printf 'WRONG  %s  not found in %s/tools\n' "$name" "$tree"
        wrong=$((wrong + 1))
        [ -n "$first_red" ] || first_red=$name
        continue
    fi
    case $name in
        *.py) set -- /usr/bin/python3 "$script" ;;
        public-numbers-check.sh | docs-paths-check.sh) set -- "$script" "$tree" ;;
        *) set -- "$script" ;;
    esac
    output=$(cd "$tree" && "$@" 2>&1)
    status=$?
    last=$(printf '%s\n' "$output" | sed '/^[[:space:]]*$/d' | tail -n 1 | cut -c1-100)
    case $status in
        0)
            printf 'OK     %s  %s\n' "$name" "$last"
            ok=$((ok + 1))
            ;;
        77)
            printf 'SKIP   %s  %s\n' "$name" "$last"
            skip=$((skip + 1))
            ;;
        *)
            if [ "$status" -eq 1 ]; then
                printf 'WRONG  %s  %s\n' "$name" "$last"
            else
                printf 'WRONG  %s  exit %s: %s\n' "$name" "$status" "$last"
            fi
            wrong=$((wrong + 1))
            [ -n "$first_red" ] || first_red=$name
            ;;
    esac
done
printf '%s OK, %s WRONG, %s SKIP\n' "$ok" "$wrong" "$skip"
[ -z "$first_red" ] || printf 'first red: %s\n' "$first_red"
[ "$wrong" -eq 0 ] || exit 1
[ "$skip" -eq 0 ] || exit 77
exit 0
