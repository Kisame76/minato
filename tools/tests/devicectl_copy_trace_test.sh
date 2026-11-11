#!/usr/bin/env bash
# java-start-check.sh's devicectl_copy, on a copy that outlasts its deadline and is killed, prints devicectl's own
# last log lines (where the file service stalled) with device identifiers masked, and none on a copy that finishes.
# Two hung attempts in a row stop the whole check at once with one line and the way out (restart CoreDeviceService)
# and exit status 3; the exit's clean-up then gets one short try per copy, none after that hangs too; a hang between
# answers does not stop it. A fake xcrun plays devicectl: it writes a trace to --log-output, then either stalls past the
# deadline or returns; FAKE_SEQ_FILE, when set, holds the modes of the next calls, one taken per call.
set -uo pipefail
cd "$(dirname "$0")/../.."
JSC=${JSC:-tools/java-start-check.sh}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Build the fake device identifiers at run time, so the source file holds no literal in a device-id form (the privacy
# gate matches that shape, synthetic or not). All-zero placeholders of the right shape: copy_trace_tail's masking
# still sees a UUID and a UDID/ECID form to replace, and the checks below look for these same generated values.
fake_uuid=$(printf '%08X-%04X-%04X-%04X-%012X' 0 0 0 0 0)
fake_ecid=$(printf '%08X-%016X' 0 0)
mkdir -p "$work/bin" "$work/tools"
printf '# the device is always reachable in this test\n' > "$work/tools/require-device.py"  # run by python3, exits 0
cat > "$work/bin/xcrun" <<'FAKE'
#!/usr/bin/env bash
# fake: xcrun devicectl device copy <dir> ... --log-output <path> ... --source ... --destination ...
shift 3   # devicectl device copy
log=""; prev=""
for a in "$@"; do [[ "$prev" == --log-output || "$prev" == -l ]] && log=$a; prev=$a; done
if [[ -n "${FAKE_SEQ_FILE:-}" ]]; then
  read -r FAKE_MODE rest < "$FAKE_SEQ_FILE"
  echo "${rest:-}" > "$FAKE_SEQ_FILE"
fi
if [[ "${FAKE_MODE:-hang}" == ok ]]; then
  [[ -n "$log" ]] && printf 'copy complete\n' > "$log"
  exit 0
fi
# A stall: write the progress up to the step that hangs (with device ids to mask), then outlast the deadline.
if [[ -n "$log" ]]; then
  {
    echo "2.0 connecting to device $FAKE_UUID"
    echo "2.1 opening appDataContainer io.github.kisame76.minato on $FAKE_ECID"
    echo "2.2 transferring tmp/session-check: waiting for the remote file service to respond"
  } > "$log"
fi
sleep "${FAKE_SLEEP:-30}"
exit 0
FAKE
chmod +x "$work/bin/xcrun"
# devicectl_copy and the helpers it calls, as the script has them.
: > "$work/functions.sh"
for fn in fail copy_stamp copy_trace_tail devicectl_copy; do
  sed -n "/^$fn() {/,/^}/p" "$JSC" >> "$work/functions.sh"
done
failures=0
check() { if eval "$2"; then :; else echo "FAIL $1"; failures=$((failures + 1)); fi; }
drive() {  # mode [calls] -> runs devicectl_copy (calls times, 1 by default), echoes its output and RET lines
  (
    cd "$work"
    export PATH="$work/bin:$PATH" FAKE_MODE=$1 FAKE_SLEEP=30 FAKE_UUID="$fake_uuid" FAKE_ECID="$fake_ecid"
    WORK="$work" DEVICE_ID=fake-id DEVICE_UDID=fake-udid BUNDLE_ID=io.github.kisame76.minato
    DEVICECTL_COPY_TIMEOUT_S=2 DEVICECTL_LAST_TRY_S=1 STAMPS=""
    exec 3>&2
    # shellcheck disable=SC1091
    source "$work/functions.sh"
    # The exit's clean-up, as restore() sends the end request: two copies, timed.
    trap 'for n in 1 2; do s=$SECONDS; devicectl_copy to --source "$work/fetched" --destination tmp/session-check \
      && echo "CLEANUP ok" || echo "CLEANUP fail after $((SECONDS - s)) s"; done' EXIT
    for _ in $(seq 1 "${2:-1}"); do
      devicectl_copy from --source tmp/session-check --destination "$work/fetched" && echo "RET ok" || echo "RET fail"
    done
  ) 2>&1
  echo "STATUS $?"
}
# A copy that stalls: killed, retried, killed: two in a row, so a FAIL that names the cause and the way out, the
# masked trace tail, and the check stops at once (no return to the caller).
out=$(drive hang)
printf '%s\n' "$out" | sed 's/^/  hang> /'
check "two hung attempts stop the check"    '[[ "$out" != *"RET "* ]]'
# shellcheck disable=SC2016
stop='FAIL  device file service hung, round stopped: restart CoreDeviceService'
stop+=' (kill -TERM $(pgrep -x CoreDeviceService)) and rerun'
check "the stop is one line"               '[[ "$(grep -c "^FAIL" <<< "$out")" == 1 ]]'
check "the stop names the way out"         'grep -qxF "$stop" <<< "$out"'
check "the stop exits with 3"               'grep -q "^STATUS 3$" <<< "$out"'
check "the clean-up gets one short try"     'grep -qE "^CLEANUP fail after [0-2] s$" <<< "$out"'
check "and none after that hangs"           '[[ "$(grep -c "^CLEANUP fail after 0 s$" <<< "$out")" -ge 1 ]]'
check "prints devicectl's stall step"       'grep -q "waiting for the remote file service to respond" <<< "$out"'
check "keeps our own check path"            'grep -q "tmp/session-check" <<< "$out"'
check "masks the device uuid"               '! grep -q "$fake_uuid" <<< "$out"'
check "masks the device ecid"               '! grep -q "$fake_ecid" <<< "$out"'
# A copy that finishes before the deadline: no kill, no FAIL, no trace tail.
out2=$(drive ok)
printf '%s\n' "$out2" | sed 's/^/  ok>   /'
check "a finished copy returns zero"        '[[ "$out2" == *"RET ok"* ]]'
check "no FAIL on a clean copy"             '! grep -q "FAIL" <<< "$out2"'
check "no trace tail on a clean copy"       '! grep -q "file service stalled" <<< "$out2"'
# A hang, an answer, a hang, an answer (each a retry that worked): no two in a row, so both calls return and no stop;
# the clean-up's two copies answer too.
echo "hang ok hang ok ok ok" > "$work/seq"
out3=$(FAKE_SEQ_FILE="$work/seq" drive seq 2)
printf '%s\n' "$out3" | sed 's/^/  seq>  /'
check "hangs between answers do not stop"   '[[ "$(grep -c "RET ok" <<< "$out3")" == 2 ]]'
check "no stop between answers"             '! grep -q "round stopped" <<< "$out3" && grep -q "^STATUS 0$" <<< "$out3"'
[[ "$failures" == 0 ]] && echo "devicectl_copy_trace_test: ok"
exit $((failures > 0))
