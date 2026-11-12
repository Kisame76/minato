#!/usr/bin/env bash
# tools/docs-paths-check.sh on a small made-up repository: a clean one passes (exit 0); each kind of broken reference in
# README.md or docs/*.md is WRONG with its file and line (exit 1): a relative link to a missing file, a link to a
# heading
# that is not there, a path in code quotes, a script name, an ADR number that docs/DESIGN.md does not list; what is
# generated (build/), a glob, a placeholder, an anchor on the same page that exists and an external URL are no failures,
# and the URLs are only listed.
# Usage: tools/tests/docs_paths_check_test.sh
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/docs-paths-test.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
failures=0
fresh() {
  rm -rf "$scratch/r"; mkdir -p "$scratch/r/docs" "$scratch/r/tools" "$scratch/r/app/Sources"
  : > "$scratch/r/tools/real.sh"; : > "$scratch/r/app/Sources/Real.swift"
  cat > "$scratch/r/docs/DESIGN.md" <<'MD'
# Design decisions

### ADR-0001: No emulation

### ADR-0002: No JIT
MD
  cat > "$scratch/r/docs/OTHER.md" <<'MD'
# Other

## A heading here
MD
  cat > "$scratch/r/README.md" <<'MD'
# Readme

## Building

See [other](docs/OTHER.md), [its heading](docs/OTHER.md#a-heading-here), [design](docs/DESIGN.md) and [up](#building).
Run `tools/real.sh`, read `app/Sources/Real.swift`, and `real.sh`. ADR-0001 and ADR-0002 decide it.
Generated: `build/native/out`, glob `tools/*.sh`, placeholder `tools/<name>.sh`, URL https://example.org/a and
[site](https://example.org/b).
MD
}
run() { "$here/docs-paths-check.sh" "$scratch/r" 2>&1; }
expect() {  # expect <name> <exit status> <pattern>
  local name="$1" want="$2" pattern="$3" out status
  out="$(run)"; status=$?
  if [[ $status -eq $want ]] && grep -q -E "$pattern" <<< "$out"; then echo "ok   $name"
  else echo "FAIL $name (exit $status, wanted $want, pattern: $pattern)"; failures=$((failures + 1)); fi
}
edit() { /usr/bin/python3 - "$scratch/r/$1" "$2" "$3" <<'PY' || { echo "TEST BROKEN: '$2' not in $1"; exit 3; }
import sys
p, a, b = sys.argv[1:]
s = open(p, encoding="utf-8").read()
if a not in s:
    sys.exit(1)
open(p, "w", encoding="utf-8").write(s.replace(a, b, 1))
PY
}
fresh; expect "a clean repository passes and its URLs are only listed" 0 "0 WRONG.*2 URLs listed"
fresh; edit README.md "(docs/OTHER.md)" "(docs/NOPE.md)"
expect "a link to a missing file is WRONG with file and line" 1 "WRONG +README.md:[0-9]+ +link +docs/NOPE.md"
fresh; edit README.md "OTHER.md#a-heading-here" "OTHER.md#no-such-heading"
expect "a link to a heading that is not there is WRONG" 1 \
  "WRONG +README.md:[0-9]+ +anchor +docs/OTHER.md#no-such-heading"
fresh; edit README.md "(#building)" "(#missing-section)"
expect "an anchor on the same page that is not there is WRONG" 1 "WRONG +README.md:[0-9]+ +anchor +#missing-section"
fresh; edit README.md '`app/Sources/Real.swift`' '`app/Sources/Gone.swift`'
expect "a path in code quotes that does not exist is WRONG" 1 "WRONG +README.md:[0-9]+ +path +app/Sources/Gone.swift"
fresh; edit README.md '`real.sh`' '`ghost.sh`'
expect "a script name that no file has is WRONG" 1 "WRONG +README.md:[0-9]+ +script +ghost.sh"
fresh; edit README.md "ADR-0002" "ADR-0099"
expect "an ADR number that DESIGN.md does not list is WRONG" 1 "WRONG +README.md:[0-9]+ +ADR +ADR-0099"
fresh; printf 'The step ADR-0003 and [x](NOPE.md).\n' > "$scratch/r/docs/OTHER.md"
expect "docs/*.md are checked too, relative to their own folder" 1 "WRONG +docs/OTHER.md:[0-9]+ +link +NOPE.md"
fresh; rm "$scratch/r/tools/real.sh"
expect "a deleted script is WRONG where it is named" 1 "WRONG.*tools/real.sh"
fresh; printf 'app/Gen.plist\n' > "$scratch/r/.gitignore"
printf 'The plist `app/Gen.plist` is generated.\n' >> "$scratch/r/README.md"
expect "a path that .gitignore lists (generated) is no failure" 0 "0 WRONG"
fresh; printf 'ARTBox: github.com/someone/Other (`docs/loader-design.md`)\n' >> "$scratch/r/README.md"
expect "a path named next to an external repository is not ours" 0 "0 WRONG"
fresh; printf 'Check scripts are `tools/*-check.sh` and the `*-plan.py` ones.\n' >> "$scratch/r/README.md"
expect "a script name behind a glob star is no failure" 0 "0 WRONG"
fresh; printf 'It ran as `build/gen/scripts/driver.sh` (not in git); later `driver.sh` counts it.\n' \
  >> "$scratch/r/README.md"
expect "a script that the page names under build/ is generated, not missing" 0 "0 WRONG"
fresh; printf 'It ran later with `driver.sh` alone.\n' >> "$scratch/r/README.md"
expect "the same script name without any build/ mention is WRONG" 1 "WRONG +README.md:[0-9]+ +script +driver.sh"
echo "$failures failed"; exit $((failures != 0))
