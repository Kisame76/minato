#!/usr/bin/env bash
# The framework pair's rule over tools/*.sh: a script that runs tools/build-framework.sh (as a command of its own, after
# ;, &&, ||, if, then or "[[ ... ]] ||", through caffeinate, nice, bash, sh, time or env, behind a root variable such as
# "$ROOT/tools/", or in a "for step in" list; never inside a message's quotes) also runs
# tools/build-framework-native.sh, so a check never starts the guest with a new framework.jar and an older
# libandroid_runtime.so (the jar's natives missing: UnsatisfiedLinkError at the first start). Only the scripts in
# JAR_ONLY may build the jar alone, and only while they start no guest (java_app_run.py, app_process, apk-run);
# build-framework.sh's own last step (tools/warn-native-stale.sh) warns them when the native side is stale.
# Scripts that only read build/framework's classes.jar and stubs.jar (the test apps' builds) build neither and are
# not touched. The rule runs on the repo's scripts and, first, on planted cases in a temp folder.
# Usage: tools/tests/framework_native_pair_test.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
WORK=$(mktemp -d)
trap 'rm -rf "${WORK:?}"' EXIT

# framework-check.sh checks the jar's bounded sources and classes; it starts no guest.
JAR_ONLY="framework-check.sh"

# The rule over the scripts of folder $1: one line per violation, nothing when it holds.
violations() {
  /usr/bin/python3 - "$1" "$JAR_ONLY" <<'PY'
import glob
import os
import re
import sys

folder, jar_only = sys.argv[1], set(sys.argv[2].split())


# Where a command starts: a line's start, after ; & | && || ( {, or after if, then, else, do, ! and "[[ ... ]] ||".
START = r"(?:^|[;&|({]|\b(?:if|then|else|do|while|until)\b|!)\s*(?:\[\[[^\]]*\]\]\s*\|\|\s*)?"
# Words that run the next one: caffeinate -i, nice [-n N], bash, sh, time, env VAR=value.
WRAPPERS = r"(?:(?:caffeinate(?:\s+-\w+)*|nice(?:\s+-n\s*-?\d+)?|bash|sh|time|env(?:\s+\w+=\S+)*)\s+)*"
# The script's path: tools/x.sh, ./tools/x.sh or a root variable's ("$ROOT/tools/x.sh", ${MAIN}/tools/x.sh), quoted
# or not.
PATH = r"(?:\$\{?\w+\}?/|\./)?tools/"


def unquoted(text):
    # A double-quoted string with a space in it is a message ("run tools/x.sh first"): it goes. One without is a word
    # ("$ROOT/tools/x.sh"): its quotes go.
    return re.sub(r'"((?:[^"\\]|\\.)*)"', lambda m: '""' if re.search(r"\s", m.group(1)) else m.group(1), text)


def runs(step, text):
    # A command (wrapped, chained, guarded or behind a root variable) or an element of a "for v in ..." step list.
    command = re.compile(START + WRAPPERS + PATH + re.escape(step) + r"(?=[\s;)|&]|$)", re.M)
    listed = re.compile(r"^\s*for \w+ in .*\"tools/" + re.escape(step) + r"\"", re.M)
    return bool(command.search(unquoted(text)) or listed.search(text))


GUEST = re.compile(r"java_app_run|app_process|apk-run")
for path in sorted(glob.glob(os.path.join(folder, "*.sh"))):
    name = os.path.basename(path)
    if name == "build-framework.sh":
        continue
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    text = "\n".join(l for l in lines if not l.lstrip().startswith("#"))
    if not runs("build-framework.sh", text) or runs("build-framework-native.sh", text):
        continue
    if name not in jar_only:
        print(f"{name}: runs tools/build-framework.sh without tools/build-framework-native.sh")
    elif GUEST.search(text):
        print(f"{name}: builds the jar alone (JAR_ONLY) but starts the guest")
PY
}

fail=0
plant() { mkdir -p "$WORK/$1"; printf '%s\n' "${@:3}" > "$WORK/$1/$2"; }
expect_fail() {
  local out
  out=$(violations "$WORK/$1")
  if [[ -z "$out" ]]; then
    echo "FAIL  planted case $1 passed the rule"; fail=1
  else
    echo "ok    planted case $1: $out"
  fi
}
expect_pass() {
  local out
  out=$(violations "$WORK/$1")
  if [[ -n "$out" ]]; then
    echo "FAIL  planted case $1 failed the rule: $out"; fail=1
  else
    echo "ok    planted case $1 holds"
  fi
}

plant jar-alone x-check.sh '#!/usr/bin/env bash' 'tools/build-framework.sh >/dev/null' \
  'python3 framework/tests/java_app_run.py x'
expect_fail jar-alone
plant jar-alone-guarded x-check.sh '#!/usr/bin/env bash' '[[ -n "${SKIP:-}" ]] || tools/build-framework.sh'
expect_fail jar-alone-guarded
plant jar-only-guest framework-check.sh '#!/usr/bin/env bash' 'tools/build-framework.sh' \
  'build/framework-native/app_process64'
expect_fail jar-only-guest
plant caffeinate x-check.sh '#!/usr/bin/env bash' 'caffeinate -i tools/build-framework.sh >/dev/null'
expect_fail caffeinate
plant bash-run x-check.sh '#!/usr/bin/env bash' 'bash tools/build-framework.sh || exit 1'
expect_fail bash-run
plant root-path x-check.sh '#!/usr/bin/env bash' '"$ROOT/tools/build-framework.sh" >/dev/null'
expect_fail root-path
plant chained x-check.sh '#!/usr/bin/env bash' 'cd "$ROOT" && nice tools/build-framework.sh'
expect_fail chained
plant if-run x-check.sh '#!/usr/bin/env bash' 'if ! tools/build-framework.sh; then exit 1; fi'
expect_fail if-run
plant wrapped-pair x-check.sh '#!/usr/bin/env bash' 'caffeinate -i "$ROOT/tools/build-framework-native.sh" >/dev/null' \
  'bash tools/build-framework.sh >/dev/null'
expect_pass wrapped-pair
plant message x-check.sh '#!/usr/bin/env bash' 'echo "no classes.jar: run bash tools/build-framework.sh" >&2'
expect_pass message
plant messages x-check.sh '#!/usr/bin/env bash' \
  'echo "skipped: no build/framework yet (tools/build-framework.sh makes it)"' \
  'echo "no classes.jar:" \\' '  "tools/build-framework.sh make it." >&2'
expect_pass messages
plant pair x-check.sh '#!/usr/bin/env bash' 'tools/build-framework-native.sh >/dev/null' \
  'tools/build-framework.sh >/dev/null'
expect_pass pair
plant step-list x-check.sh '#!/usr/bin/env bash' \
  'for step in "tools/build-framework-native.sh" "tools/build-framework.sh"; do $step; done'
expect_pass step-list
plant reader x-build.sh '#!/usr/bin/env bash' \
  '[[ -f build/framework/classes.jar ]] || die "no classes.jar: run tools/build-framework.sh"' \
  '# tools/build-framework.sh makes it'
expect_pass reader
plant jar-only framework-check.sh '#!/usr/bin/env bash' '[[ -n "${SKIP_BUILD:-}" ]] || tools/build-framework.sh'
expect_pass jar-only

out=$(violations tools)
if [[ -n "$out" ]]; then
  echo "FAIL  tools/*.sh:"; sed 's/^/  /' <<< "$out"; fail=1
else
  echo "ok    tools/*.sh: every script that builds framework.jar builds its native side too (JAR_ONLY: $JAR_ONLY)"
fi
exit $fail
