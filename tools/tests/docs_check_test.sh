#!/bin/sh
# Tests tools/docs-check.sh on a fake tree whose four checks are stand-ins that exit as a marker file says.
set -u
here=$(cd "$(dirname "$0")" && pwd)
check="$here/../docs-check.sh"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/docs-check.XXXXXX") || exit 1
trap 'rm -rf "$scratch"' EXIT
failed=0
names="public-numbers-check.sh docs-paths-check.sh ndk-names-check.sh license-check-repo-test.py"

fresh() {
    rm -rf "$scratch/t"
    mkdir -p "$scratch/t/tools"
    for name in $names; do
        # A stand-in: prints its name and a last line, then exits with the code in red/<name> (default 0).
        case $name in
            *.py)
                {
                    echo 'import os, sys'
                    echo "print('first line of $name')"
                    echo "print('last line of $name')"
                    echo "here = os.path.dirname(os.path.abspath(__file__))"
                    echo "code = os.path.join(here, '..', 'red', '$name')"
                    echo "sys.exit(int(open(code).read()) if os.path.exists(code) else 0)"
                } > "$scratch/t/tools/$name"
                ;;
            *)
                {
                    echo '#!/bin/sh'
                    echo "echo 'first line of $name'"
                    echo "echo 'last line of $name'"
                    echo "exit \$(cat \"\$(dirname \"\$0\")/../red/$name\" 2>/dev/null || echo 0)"
                } > "$scratch/t/tools/$name"
                ;;
        esac
        chmod 755 "$scratch/t/tools/$name"
    done
    mkdir -p "$scratch/t/red"
}
set_exit() { echo "$2" > "$scratch/t/red/$1"; }
expect() { # label status pattern
    label=$1; want=$2; pattern=$3
    out=$("$check" "$scratch/t" 2>&1); status=$?
    if [ "$status" -eq "$want" ] && printf '%s\n' "$out" | grep -Eq "$pattern"; then echo "ok   $label"
    else
        echo "FAIL $label (status $status, wanted $want, pattern $pattern)"
        printf '%s\n' "$out" | sed 's/^/     /'
        failed=1
    fi
}

fresh
expect "all four green: one OK line each and exit 0" 0 "^OK +public-numbers-check.sh +last line of public-num"
expect "the summary counts them" 0 "^4 OK, 0 WRONG, 0 SKIP"
for name in $names; do
    fresh; set_exit "$name" 1
    expect "$name red: exit 1" 1 "^WRONG +$name"
    expect "$name red: it is named as the first red check" 1 "^first red: $name\$"
done
fresh; set_exit docs-paths-check.sh 1; set_exit ndk-names-check.sh 1
expect "two red: the first in the order is named" 1 "^first red: docs-paths-check.sh\$"
fresh; set_exit ndk-names-check.sh 77
expect "a skipped check is SKIP, never OK, and exit 77" 77 "^SKIP +ndk-names-check.sh"
fresh; set_exit ndk-names-check.sh 77; set_exit public-numbers-check.sh 1
expect "red beats skipped" 1 "^first red: public-numbers-check.sh\$"
fresh; rm "$scratch/t/tools/docs-paths-check.sh"
expect "a check that is not there is WRONG" 1 "^WRONG +docs-paths-check.sh +not found"
fresh; set_exit license-check-repo-test.py 2
expect "any other exit code is WRONG too" 1 "^WRONG +license-check-repo-test.py +exit 2"
exit $failed
