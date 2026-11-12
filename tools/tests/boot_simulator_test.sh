#!/usr/bin/env bash
# tools/boot-simulator.sh against a fake xcrun on PATH (no real Simulator): an already booted device is left as it is;
# a shut down one gets exactly one `simctl boot`; a boot that fails ends loudly (FAIL on stderr with simctl's own
# words, exit 1), with no second boot and the device shut down; a boot that "succeeds" while SpringBoard crashes (a new
# SpringBoard-*.ips in the reports folder) ends the same way; `simctl bootstatus` is never called. Then the rule over
# tools/*.sh: no script boots a Simulator itself or calls bootstatus; tools/boot-simulator.sh is the one place.
# Usage: tools/tests/boot_simulator_test.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
WORK=$(mktemp -d)
trap 'rm -rf "${WORK:?}"' EXIT
fail=0
check() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fail=1; fi; }

# The fake xcrun: `simctl list devices` shows the device in $FAKE_STATE; `simctl boot` logs, exits $FAKE_BOOT_EXIT with
# $FAKE_BOOT_ERR on stderr, and with FAKE_CRASH=1 drops a SpringBoard crash report into $BOOT_SIM_REPORTS; every call is
# logged to $FAKE_LOG.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/xcrun" <<'FAKE'
#!/usr/bin/env bash
echo "$*" >> "$FAKE_LOG"
[[ "$1" == simctl ]] || exit 0
case "$2" in
  list) echo "    iPhone Test (2A3826B2-0000-4000-8000-000000000001) (${FAKE_STATE:-Shutdown}) " ;;
  boot)
    [[ "${FAKE_CRASH:-}" == 1 ]] && : > "$BOOT_SIM_REPORTS/SpringBoard-2026-11-12-170206.ips"
    [[ -n "${FAKE_BOOT_ERR:-}" ]] && echo "$FAKE_BOOT_ERR" >&2
    exit "${FAKE_BOOT_EXIT:-0}" ;;
  *) exit 0 ;;
esac
FAKE
chmod 0755 "$WORK/bin/xcrun"
UDID=2A3826B2-0000-4000-8000-000000000001

# Runs the tool in a scenario of its own: $1 the scenario's name, the rest VAR=value settings of the fake.
run() {
  local name=$1
  shift
  mkdir -p "$WORK/$name/reports"
  : > "$WORK/$name/log"
  env PATH="$WORK/bin:$PATH" FAKE_LOG="$WORK/$name/log" BOOT_SIM_REPORTS="$WORK/$name/reports" BOOT_SIM_WATCH=1 "$@" \
    "$ROOT/tools/boot-simulator.sh" "$UDID" > "$WORK/$name/out" 2> "$WORK/$name/err"
  echo $? > "$WORK/$name/code"
}
code() { cat "$WORK/$1/code"; }
calls() { grep -c -E "^simctl $2( |$)" "$WORK/$1/log"; }

if [[ ! -x tools/boot-simulator.sh ]]; then
  echo "FAIL  tools/boot-simulator.sh is missing or not executable"; fail=1
else
  run booted FAKE_STATE=Booted
  check "booted: exit 0" '[[ $(code booted) == 0 ]]'
  check "booted: no simctl boot" '[[ $(calls booted boot) == 0 ]]'

  run clean FAKE_STATE=Shutdown
  check "clean boot: exit 0" '[[ $(code clean) == 0 ]]'
  check "clean boot: one simctl boot" '[[ $(calls clean boot) == 1 ]]'
  check "clean boot: no shutdown" '[[ $(calls clean shutdown) == 0 ]]'

  run einval FAKE_STATE=Shutdown FAKE_BOOT_EXIT=22 \
    FAKE_BOOT_ERR="An error was encountered processing the command (domain=NSPOSIXErrorDomain, code=22)"
  check "failed boot: exit 1" '[[ $(code einval) == 1 ]]'
  check "failed boot: one simctl boot, no retry" '[[ $(calls einval boot) == 1 ]]'
  check "failed boot: shut down" '[[ $(calls einval shutdown) == 1 ]]'
  check "failed boot: FAIL on stderr with simctl's words" \
    'grep -q "^FAIL" "$WORK/einval/err" && grep -q "code=22" "$WORK/einval/err"'

  run crash FAKE_STATE=Shutdown FAKE_CRASH=1
  check "SpringBoard crashing after the boot: exit 1" '[[ $(code crash) == 1 ]]'
  check "SpringBoard crashing: shut down" '[[ $(calls crash shutdown) == 1 ]]'
  check "SpringBoard crashing: FAIL names SpringBoard" 'grep -q "^FAIL.*SpringBoard" "$WORK/crash/err"'

  for name in booted clean einval crash; do
    check "$name: no simctl bootstatus" '[[ $(calls $name bootstatus) == 0 ]]'
  done
fi

# The rule over tools/*.sh (comments left out): only tools/boot-simulator.sh runs `simctl boot`; none runs bootstatus.
bad=$(grep -n -E '^[^#]*simctl[[:space:]]+(boot|bootstatus)([[:space:]]|$)' tools/*.sh |
  grep -v '^tools/boot-simulator\.sh:')
check "no script but tools/boot-simulator.sh boots a Simulator" '[[ -z "$bad" ]]'
[[ -z "$bad" ]] || sed 's/^/      /' <<< "$bad"
bad=$(grep -n -E '^[^#]*simctl[[:space:]]+bootstatus' tools/*.sh)
check "no script calls simctl bootstatus" '[[ -z "$bad" ]]'
exit $fail
