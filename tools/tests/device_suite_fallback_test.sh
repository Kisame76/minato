#!/usr/bin/env bash
# tools/device-suite.sh's launch on the iPhone when devicectl refuses --console (CoreDeviceError 10002, POSIX 22): the
# test is started without the console and judged by the files it leaves. A fake xcrun plays devicectl: the launch with
# --console fails like that, the launch without it works and the device's output folder shows the new run only after it.
# Cases: a fresh output is taken (and not the last run's), a launch that never produces a new output fails after
# $TIMEOUT s, and another devicectl failure does not start the fallback.
set -euo pipefail
cd "$(dirname "$0")/../.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/device/tmp/fixtures/t/output"
printf '0\n' > "$work/device/tmp/fixtures/t/output/status"
printf 'old\n' > "$work/device/tmp/fixtures/t/output/stdout"
cat > "$work/bin/xcrun" <<'FAKE'
#!/usr/bin/env bash
# fake: xcrun devicectl device process launch | copy from | copy to ...
shift 3   # devicectl device process|copy
if [[ "$1" == launch ]]; then
  for arg in "$@"; do
    if [[ "$arg" == --console ]]; then
      case "$FAKE_MODE" in
        other) echo "CoreDeviceError 1234: something else"; exit 1 ;;
        *) echo "CoreDeviceError 10002 (POSIX error 22: Invalid argument)"; exit 1 ;;
      esac
    fi
  done
  json=""
  while [[ $# -gt 0 ]]; do [[ "$1" == --json-output ]] && json=$2; shift; done
  [[ -n "$json" ]] && echo '{"result":{}}' > "$json"
  if [[ "$FAKE_MODE" != never ]]; then  # the app removes the test's folder when it starts the test
    rm -rf "$FAKE_DEVICE/tmp/fixtures/t/output"
    mkdir -p "$FAKE_DEVICE/tmp/fixtures/t/output"
    printf '0\n' > "$FAKE_DEVICE/tmp/fixtures/t/output/status"
    printf 'new run\n' > "$FAKE_DEVICE/tmp/fixtures/t/output/stdout"
  fi
  exit 0
fi
direction=$1
source=""; destination=""
shift
while [[ $# -gt 0 ]]; do
  case "$1" in --source) source=$2; shift ;; --destination) destination=$2; shift ;; esac
  shift
done
if [[ "$direction" == to ]]; then
  [[ -d "$FAKE_DEVICE/$(dirname "$destination")" ]] && cp "$source" "$FAKE_DEVICE/$destination"
  exit 0
fi
[[ -d "$FAKE_DEVICE/$source" ]] || exit 1
cp -R "$FAKE_DEVICE/$source" "$destination"
FAKE
chmod +x "$work/bin/xcrun"
# The launch and fetch functions of the script, as they are.
sed -n '/^# Starts the test in the app and waits/,/^# The run time and the footprint peak/p' tools/device-suite.sh |
  sed '$d' > "$work/functions.sh"
failures=0
run_case() {  # mode expected-return expected-text
  rm -rf "$work/results" "$work/device"
  mkdir -p "$work/results" "$work/device/tmp/fixtures/t/output"
  printf '0\n' > "$work/device/tmp/fixtures/t/output/status"
  printf 'old\n' > "$work/device/tmp/fixtures/t/output/stdout"
  local got=0
  (
    export PATH="$work/bin:$PATH" FAKE_MODE=$1 FAKE_DEVICE="$work/device"
    SIMULATOR=0 ART="" DEBUG=0 DEVICE_ID=fake BUNDLE_ID=fake.bundle RESULTS="$work/results" TIMEOUT=4 POLL_SECONDS=1
    APP_ARGUMENTS=()
    # shellcheck disable=SC1091
    source "$work/functions.sh"
    launch t && fetch t
  ) > "$work/out" 2>&1 || got=$?
  if [[ "$got" != "$2" ]]; then echo "FAIL $1: exit $got, wanted $2"; failures=$((failures + 1)); return; fi
  if [[ -n "${3:-}" ]] && ! grep -q "$3" "$work/results/t/stdout" 2>/dev/null; then
    echo "FAIL $1: the fetched stdout lacks '$3'"; failures=$((failures + 1))
  fi
}
run_case fallback 0 "new run"      # the fresh output, not the last run's
run_case never 1                   # the launch works but nothing new appears: false after the time-out
run_case other 1                   # another failure: no fallback
[[ "$failures" == 0 ]] && echo "device_suite_fallback_test: ok"
exit $((failures > 0))
