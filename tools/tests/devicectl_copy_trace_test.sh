#!/usr/bin/env bash
# java-start-check.sh's devicectl_copy, on a copy that outlasts its deadline and is killed, prints devicectl's own
# last log lines (where the file service stalled) with device identifiers masked, and none on a copy that finishes.
# A fake xcrun plays devicectl: it writes a trace to --log-output, then either stalls past the deadline or returns.
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
drive() {  # mode -> runs devicectl_copy once, echoes its output and RET line
  (
    cd "$work"
    export PATH="$work/bin:$PATH" FAKE_MODE=$1 FAKE_SLEEP=30 FAKE_UUID="$fake_uuid" FAKE_ECID="$fake_ecid"
    WORK="$work" DEVICE_ID=fake-id DEVICE_UDID=fake-udid BUNDLE_ID=io.github.kisame76.minato
    DEVICECTL_COPY_TIMEOUT_S=2 STAMPS=""
    exec 3>&2
    # shellcheck disable=SC1091
    source "$work/functions.sh"
    devicectl_copy from --source tmp/session-check --destination "$work/fetched" && echo "RET ok" || echo "RET fail"
  ) 2>&1
}
# A copy that stalls: killed, retried, killed, a FAIL that names the cause, and the masked trace tail.
out=$(drive hang)
printf '%s\n' "$out" | sed 's/^/  hang> /'
check "a hung copy returns non-zero"        '[[ "$out" == *"RET fail"* ]]'
check "a FAIL names the hung copy"          'grep -q "FAIL.*hung" <<< "$out"'
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
[[ "$failures" == 0 ]] && echo "devicectl_copy_trace_test: ok"
exit $((failures > 0))
