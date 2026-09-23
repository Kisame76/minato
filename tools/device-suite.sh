#!/usr/bin/env bash
# Runs the fixture tests of fixtures/tests.txt on the connected iPhone (M2 step 6, M2a): builds the
# app with the fixtures as signed containers in its bundle (tools/embed-fixtures.sh), installs it,
# starts it once per test with -runFixture <test> (app/Fixtures/FixtureRunner.swift), without a
# debugger, fetches what the program wrote and compares it and the exit status with
# fixtures/expected, as CTest does on the Mac.
# Needs the iPhone paired, unlocked and connected, with Developer Mode on (see tools/deploy.sh).
# Usage: tools/device-suite.sh [--device <name or identifier>] [test ...]
# Results: build/device-suite/<test>/{stdout,stderr,status,traced}, <test>.launch.json and
# <test>.console (devicectl's report and messages). The times include starting the app.
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.apkrunner"
RESULTS=build/device-suite
WANTED_DEVICE=""
if [[ "${1:-}" == "--device" && $# -ge 2 ]]; then
  WANTED_DEVICE="$2"
  shift 2
fi

# The fields of a test in fixtures/tests.txt, as in tools/android-reference.sh.
test_names() { awk '!/^#/ && NF { print $1 }' fixtures/tests.txt; }
field() {
  local item
  for item in $(awk -v name="$1" '$1 == name { $1 = ""; print; exit }' fixtures/tests.txt); do
    case "$item" in
      "$2"=*) echo "${item#*=}"; return ;;
      "$2") echo yes; return ;;
    esac
  done
}

if [[ $# -gt 0 ]]; then
  tests=("$@")
else
  tests=()
  for name in $(test_names); do
    tests+=("$name")
  done
fi

# What goes into the app bundle: the fixtures, bionic, and apk-convert, which makes containers of them.
tools/build-fixtures.sh
tools/build-bionic.sh >/dev/null
cmake --preset macos >/dev/null
cmake --build --preset macos --target apk-convert
BUILD_SETTINGS="APK_EMBED_FIXTURES=YES" INSTALL_ONLY=1 tools/deploy.sh ${WANTED_DEVICE:+"$WANTED_DEVICE"}
read -r DEVICE_ID _ < <(python3 tools/find-device.py "$WANTED_DEVICE") || true
if [[ -z "${DEVICE_ID:-}" ]]; then
  echo "No paired iPhone found." >&2
  exit 1
fi

mkdir -p "$RESULTS"
failed=0
started=$(date +%s)
for name in "${tests[@]}"; do
  rm -rf "${RESULTS:?}/$name" "$RESULTS/$name.launch.json" "$RESULTS/$name.console"
  begin=$(python3 -c 'import time; print(int(time.time() * 1000))')
  # "--" ends devicectl's options, which would take -runFixture for a cluster of its own short options.
  # devicectl exits with the app's exit code, so its JSON tells whether the app ran.
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --console --timeout 120 \
    --json-output "$RESULTS/$name.launch.json" "$BUNDLE_ID" -- -runFixture "$name" >"$RESULTS/$name.console" 2>&1 || true
  milliseconds=$(($(python3 -c 'import time; print(int(time.time() * 1000))') - begin))
  if ! python3 -c 'import json, sys; json.load(open(sys.argv[1]))["result"]["terminationResult"]' \
    "$RESULTS/$name.launch.json" 2>/dev/null; then
    echo "FAILED   $name: devicectl could not run it, see $RESULTS/$name.console"
    failed=1
    continue
  fi
  if ! xcrun devicectl device copy from --device "$DEVICE_ID" --domain-type appDataContainer \
    --domain-identifier "$BUNDLE_ID" --source "tmp/fixtures/$name/output" --destination "$RESULTS/$name" \
    >>"$RESULTS/$name.console" 2>&1; then
    echo "FAILED   $name: no output on the device, see $RESULTS/$name.console"
    failed=1
    continue
  fi

  # The exit status as the runtime reported it (apkrt_report_exit_status); devicectl's exit code is
  # not reliable for a process that ends right after its start.
  status=$(tr -d '\n' <"$RESULTS/$name/status" 2>/dev/null || true)
  expected=fixtures/expected/$name
  expected_status=$(field "$name" status)
  problems=""
  cmp -s "$expected.out" "$RESULTS/$name/stdout" || problems+=" stdout"
  if [[ -f "$expected.err" ]]; then
    cmp -s "$expected.err" "$RESULTS/$name/stderr" || problems+=" stderr"
  elif [[ -s "$RESULTS/$name/stderr" ]]; then
    problems+=" stderr"
  fi
  [[ "$status" == "${expected_status:-0}" ]] || problems+=" exit-status-${status:-unknown}"
  # M2's criterion: without a debugger (app/Fixtures/FixtureRunner.swift checks P_TRACED).
  [[ "$(cat "$RESULTS/$name/traced" 2>/dev/null)" == 0 ]] || problems+=" debugger-attached"
  if [[ -z "$problems" ]]; then
    printf 'same     %-26s %5d ms with the launch\n' "$name" "$milliseconds"
  else
    failed=1
    echo "DIFFERS  $name:$problems"
    diff -u "$expected.out" "$RESULTS/$name/stdout" | sed 's/^/    /' || true
    if [[ -f "$expected.err" ]]; then
      diff -u "$expected.err" "$RESULTS/$name/stderr" | sed 's/^/    /' || true
    else
      sed 's/^/    stderr: /' "$RESULTS/$name/stderr"
    fi
  fi
done
echo "${#tests[@]} tests on the iPhone in $(($(date +%s) - started)) s"
exit $failed
