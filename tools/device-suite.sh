#!/usr/bin/env bash
# Runs the fixture tests of fixtures/tests.txt in the app on the connected iPhone or in the iOS
# Simulator (M2 steps 6 and 7): builds the app with the fixtures in its bundle
# (tools/embed-fixtures.sh), installs it, starts it once per test with -runFixture <test>
# (app/Fixtures/FixtureRunner.swift), without a debugger, fetches what the program wrote and
# compares it and the exit status with fixtures/expected, as CTest does on the Mac.
#   M2a (default): the containers were converted and signed on the Mac, at build time.
#   M2b (--sign-on-device): the app converts and signs the program and its own libraries itself,
#       with the certificate imported in the app on an iPhone, ad hoc in the Simulator (ADR-0022).
#       The costs per file are then shown, from output/signing.
#   --without-increased-memory: the iPhone build is signed without its entitlements file, so without
#       com.apple.developer.kernel.increased-memory-limit, as a SideStore install with a free account
#       is (ADR-0012, ADR-0035). Its results go to build/device-suite-without-increased-memory/. The
#       next tools/deploy.sh installs the app with the entitlement again.
#   --frame-rate 60|120: frame-cap-test alone with the FPS unlock (ADR-0066 step 4, -frameRateUnlock): its stdout
#       must be runtime/tests/frame_rate_run.py's lines for that rate. Its results go to <results>-unlock<rate>/.
#   --iphone-only: run the fixtures/iphone-only.txt tests (memory-limit-test), which allocate until iOS ends the app
#       for memory (jetsam) and check last-session.memory (ADR-0032 d8). OPT-IN, and a real iPhone only: they are
#       never in a routine sweep (they would evict the user's background apps), refused under --simulator, and not in
#       fixtures/tests.txt, so ctest and the AVD cannot start them. Also runnable by name (device-suite.sh --device
#       memory-limit-test). Do NOT add them to the regression sweep.
# ART's tests (ADR-0033 d2), staged in build/device-art by runtime/tests/device_art.py, which
# tools/embed-fixtures.sh puts into the bundle, each started with -runART <test>, given 300 s and
# compared by device_art.py check; the results go to <results>-art/ (-art-debug/ with --debug):
#   --art: the Java fixtures (art-hello, codec-test, conscrypt-test, crypto-test, stress-test):
#       their stdout, the status 0, codec-test's offsets, a signal stress-test handled during a
#       collection, and that ART's log names the heap window once, with 4 GiB, or 2 GiB without
#       increased-memory-limit. The benchmarks gc-bench and gc-bench-cms (ADR-0033 decision 5) run
#       only when named: --art gc-bench gc-bench-cms.
#   --art-run-tests [--sample]: the run-tests of fixtures/art-run-tests.txt, or CTest's sample of
#       them: stdout, stderr without the logger's lines and the status, as on the Mac.
#   --debug: ART's debug flavor (-flavor debug), with -Xcheck:jni.
#   --art-dir <dir>: another ART build than build/art, such as a private one before its commit.
#   The runtime's exit measures (output/measures) are shown: run time and footprint peak.
# The iPhone must be paired, unlocked and connected, with Developer Mode on (see tools/deploy.sh).
# Usage: tools/device-suite.sh [--simulator] [--sign-on-device] [--without-increased-memory]
#                              [--device <name or identifier>] [test ...]
#        tools/device-suite.sh --frame-rate 60|120 [--simulator] [--device <...>] [frame-cap-test]
#        tools/device-suite.sh --art | --art-run-tests [--sample] [--debug] [--art-dir <dir>]
#                              [--simulator] [--without-increased-memory] [--device <...>] [test ...]
# SKIP_INSTALL=1 runs the tests of the app that is already installed: no build and no install, only the launch,
# the fetch and the comparison. For a run with the iPhone offline (airplane mode), where iOS verifies a free account's
# profile online at the first launch after an install: install and launch the app once online (this script without the
# flag, or tools/deploy.sh), switch airplane mode on, then run it again with the flag. The tests must be those the
# installed bundle was built with.
# Results: build/device-suite/<test>/ (build/simulator-suite/ for the Simulator) with stdout,
# stderr, status, traced, measures and, with --sign-on-device, signing; on the iPhone also
# <test>.launch.json and <test>.console (devicectl's report and messages). The times include
# starting the app. What a benchmark (measures in fixtures/tests.txt) prints is shown, not compared.
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
SIMULATOR=0
SIGN_ON_DEVICE=0
WITHOUT_INCREASED_MEMORY=0
IPHONE_ONLY=0  # run the fixtures/iphone-only.txt tests (memory-limit-test): a real iPhone only, by opt-in
WANTED_DEVICE=""
ART=""  # the native tests, or ART's: "fixtures" (--art) or "run-tests" (--art-run-tests)
SAMPLE=0
DEBUG=0
ART_DIR=""
FRAME_RATE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --simulator) SIMULATOR=1; shift ;;
    --sign-on-device) SIGN_ON_DEVICE=1; shift ;;
    --without-increased-memory) WITHOUT_INCREASED_MEMORY=1; shift ;;
    # Run the fixtures/iphone-only.txt tests (memory-limit-test): a real iPhone only, refused under --simulator. They
    # allocate until iOS ends the app for memory (jetsam), so they are never in the normal suite (ADR-0032 d8).
    --iphone-only) IPHONE_ONLY=1; shift ;;
    --device) WANTED_DEVICE="$2"; shift 2 ;;
    --art) ART=fixtures; shift ;;
    --art-run-tests) ART=run-tests; shift ;;
    --sample) SAMPLE=1; shift ;;
    --debug) DEBUG=1; shift ;;
    --art-dir) ART_DIR="$(cd "$2" && pwd)"; shift 2 ;;
    --frame-rate) FRAME_RATE="$2"; shift 2 ;;
    *) break ;;
  esac
done
if [[ "$SIMULATOR" == 1 && "$WITHOUT_INCREASED_MEMORY" == 1 ]]; then
  echo "--without-increased-memory is for the iPhone: the Simulator has no memory limit." >&2
  exit 2
fi
if [[ -z "$ART" && ( "$SAMPLE" == 1 || "$DEBUG" == 1 || -n "$ART_DIR" ) ]] ||
   [[ "$SAMPLE" == 1 && "$ART" != run-tests ]] || [[ -n "$ART" && "$SIGN_ON_DEVICE" == 1 ]]; then
  echo "--sample goes with --art-run-tests, --debug and --art-dir with --art or --art-run-tests," \
    "and ART's tests have containers signed on the Mac only." >&2
  exit 2
fi
if [[ -n "$FRAME_RATE" ]]; then
  if [[ "$FRAME_RATE" != 60 && "$FRAME_RATE" != 120 ]] || [[ -n "$ART" ]] ||
     [[ $# -gt 1 ]] || [[ $# -eq 1 && "$1" != frame-cap-test ]]; then
    echo "--frame-rate takes 60 or 120 and runs frame-cap-test alone." >&2
    exit 2
  fi
  set -- frame-cap-test
fi
# The iPhone paired and reachable, by cable or over the local network, checked before the builds.
[[ "$SIMULATOR" == 1 ]] || python3 tools/require-device.py "$WANTED_DEVICE" || exit 1
RESULTS=build/device-suite
[[ "$SIMULATOR" == 0 ]] || RESULTS=build/simulator-suite
[[ "$WITHOUT_INCREASED_MEMORY" == 0 ]] || RESULTS=build/device-suite-without-increased-memory
if [[ -n "$ART" ]]; then
  RESULTS+=-art
  [[ "$DEBUG" == 0 ]] || RESULTS+=-debug
fi
[[ -z "$FRAME_RATE" ]] || RESULTS+=-unlock$FRAME_RATE
BUILD_SETTINGS="APK_EMBED_FIXTURES=YES"
[[ "$WITHOUT_INCREASED_MEMORY" == 0 ]] || BUILD_SETTINGS+=" CODE_SIGN_ENTITLEMENTS="
[[ -z "$ART_DIR" ]] || BUILD_SETTINGS+=" APK_ART_DIR=$ART_DIR"
APP_ARGUMENTS=()
[[ "$SIGN_ON_DEVICE" == 0 ]] || APP_ARGUMENTS+=(-signOnDevice)
[[ -z "$FRAME_RATE" ]] || APP_ARGUMENTS+=(-frameRateUnlock "$FRAME_RATE")
# How long a test may run: as CTest gives ART's tests on the Mac (runtime/art/run_tests.py).
TIMEOUT=120
[[ -z "$ART" ]] || TIMEOUT=300

# The fields of a test in fixtures/tests.txt, as in tools/android-reference.sh.
test_names() { awk '!/^#/ && NF { print $1 }' fixtures/tests.txt; }
# The tests the suite leaves out unless they are named: cacerts-test reads Conscrypt's APEX, which FixtureRunner does
# not mount for a fixture yet (ADR-0046 decision 4).
NOT_ON_DEVICE=(cacerts-test)
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
elif [[ -z "$ART" ]]; then
  tests=()
  for name in $(test_names); do
    [[ " ${NOT_ON_DEVICE[*]} " == *" $name "* ]] || tests+=("$name")
  done
fi

# fixtures/iphone-only.txt (memory-limit-test): the memory-watermark device check (ADR-0032 d8). These are NOT in
# fixtures/tests.txt, so the normal suite -- and Mac ctest and the AVD, which read only tests.txt -- never start them.
# They allocate until iOS ends the app for memory (jetsam), so a real iPhone only. Run them with --iphone-only, or by
# naming one; the block after the main loop handles them and refuses the Simulator. A plain device run skips them.
iphone_only_tests=()
if [[ -z "$ART" ]]; then
  # A tree without the list (a private test copy) has none; under pipefail a missing file must not end the run.
  iphone_only_all=" $({ awk '!/^#/ && NF { print $1 }' fixtures/iphone-only.txt 2>/dev/null || true; } | tr '\n' ' ') "
  remaining=()
  for name in ${tests[@]+"${tests[@]}"}; do
    if [[ "$iphone_only_all" == *" $name "* ]]; then iphone_only_tests+=("$name"); else remaining+=("$name"); fi
  done
  tests=(${remaining[@]+"${remaining[@]}"})
  if [[ "$IPHONE_ONLY" == 1 ]]; then
    for name in $iphone_only_all; do
      [[ " ${iphone_only_tests[*]-} " == *" $name "* ]] || iphone_only_tests+=("$name")
    done
  fi
fi

# What goes into the app bundle: the fixtures, bionic, and apk-convert, which makes containers of them. Not with
# SKIP_INSTALL=1: the installed bundle stays as it is.
SKIP_INSTALL=${SKIP_INSTALL:-0}
if [[ "$SKIP_INSTALL" != 1 ]]; then
  tools/build-fixtures.sh
  tools/build-bionic.sh >/dev/null
  cmake --preset macos >/dev/null
  cmake --build --preset macos --target apk-convert
fi
if [[ -n "$ART" ]]; then
  # The whole stage, whichever tests run, so that the bundle stays the same from mode to mode.
  python3 runtime/tests/device_art.py stage "${ART_DIR:-build/art}" build/fixtures/bin . build/device-art
  if [[ $# -eq 0 ]]; then
    tests=()
    for name in $(python3 -c 'import json, sys
kind = "java" if sys.argv[2] == "fixtures" else "run-test"
for test, entry in sorted(json.load(open(sys.argv[1])).items()):
    if entry["kind"] == kind and not entry.get("benchmark") and (sys.argv[3] == "0" or entry.get("sample")):
        print(test)' build/device-art/tests.json "$ART" "$SAMPLE"); do
      tests+=("$name")
    done
  fi
fi
if [[ "$SIMULATOR" == 1 ]]; then
  [[ "$SKIP_INSTALL" == 1 ]] ||
    BUILD_SETTINGS="$BUILD_SETTINGS" INSTALL_ONLY=1 tools/sim.sh --headless ${WANTED_DEVICE:+"$WANTED_DEVICE"}
  read -r DEVICE_ID _ < <(python3 tools/find-simulator.py "$WANTED_DEVICE") || true
else
  [[ "$SKIP_INSTALL" == 1 ]] ||
    BUILD_SETTINGS="$BUILD_SETTINGS" INSTALL_ONLY=1 tools/deploy.sh ${WANTED_DEVICE:+"$WANTED_DEVICE"}
  read -r DEVICE_ID _ < <(python3 tools/find-device.py "$WANTED_DEVICE") || true
  # Whether the installed build has the entitlement, from its signature, as tools/spikes.sh shows it.
  entitlements=$(codesign -d --entitlements - --xml \
    "${DERIVED:-build/DerivedData}/Build/Products/Debug-iphoneos/Minato.app" \
    2>/dev/null || true)
  ENTITLEMENT=without
  [[ "$entitlements" != *com.apple.developer.kernel.increased-memory-limit* ]] || ENTITLEMENT=with
  if [[ "$WITHOUT_INCREASED_MEMORY" == 1 && "$ENTITLEMENT" == with ]]; then
    echo "The build still has increased-memory-limit." >&2
    exit 1
  fi
fi
if [[ -z "${DEVICE_ID:-}" ]]; then
  echo "No device found." >&2
  exit 1
fi

# Starts the test in the app and waits until the program has ended the app, at most $TIMEOUT s;
# true when it ran.
launch() {
  local name=$1
  local run=(-runFixture "$name")
  if [[ -n "$ART" ]]; then
    run=(-runART "$name")
    [[ "$DEBUG" == 0 ]] || run+=(-flavor debug)
  fi
  if [[ "$SIMULATOR" == 1 ]]; then
    # The app removes the test's folder only once it starts the test, so a launch that never reached the app
    # would leave the last run's output there, fetched as this run's (2026-10-04: CoreSimulator found no
    # Simulator runtime for a moment). It goes first, from the Simulator's data container only.
    local container
    container=$(xcrun simctl get_app_container "$DEVICE_ID" "$BUNDLE_ID" data) || return 1
    rm -rf "${container:?}/tmp/fixtures/${name:?}"
    xcrun simctl launch --console --terminate-running-process "$DEVICE_ID" "$BUNDLE_ID" \
      "${run[@]}" ${APP_ARGUMENTS[@]+"${APP_ARGUMENTS[@]}"} >"$RESULTS/$name.console" 2>&1 &
    local launched=$! waited=0
    while kill -0 "$launched" 2>/dev/null; do
      if [[ "$waited" == "$TIMEOUT" ]]; then
        echo "no end within $TIMEOUT s" >>"$RESULTS/$name.console"
        xcrun simctl terminate "$DEVICE_ID" "$BUNDLE_ID" >/dev/null 2>&1 || true
      fi
      sleep 1
      waited=$((waited + 1))
    done
    wait "$launched" || true
    # simctl's first line names the process it started; without it the app never ran the test.
    head -n 1 "$RESULTS/$name.console" | grep -q "^$BUNDLE_ID: [0-9]"
    return
  fi
  # "--" ends devicectl's options, which would take -runFixture for a cluster of its own short options.
  # devicectl exits with the app's exit code, so its JSON tells whether the app ran.
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --console \
    --timeout "$TIMEOUT" --json-output "$RESULTS/$name.launch.json" "$BUNDLE_ID" -- "${run[@]}" \
    ${APP_ARGUMENTS[@]+"${APP_ARGUMENTS[@]}"} >"$RESULTS/$name.console" 2>&1 || true
  if python3 -c 'import json, sys; json.load(open(sys.argv[1]))["result"]["terminationResult"]' \
      "$RESULTS/$name.launch.json" 2>/dev/null; then
    return 0
  fi
  # With the phone asleep or its console connection down devicectl refuses the launch with --console (CoreDeviceError
  # 10002, POSIX error 22) while the same launch without it works. Then the test is started without the console and
  # judged by the files it leaves: wait until a fresh output/status is there.
  if grep -q -E "10002|POSIX.*22|Invalid argument" "$RESULTS/$name.console" 2>/dev/null; then
    echo "note: devicectl refused --console (10002 / EINVAL): launched $name without it" >>"$RESULTS/$name.console"
    launch_without_console "$name" "${run[@]}" ${APP_ARGUMENTS[@]+"${APP_ARGUMENTS[@]}"}
    return
  fi
  return 1
}

# The launch without devicectl's console: a marker file goes into the test's output folder on the device first, the
# app removes that folder when it starts the test, and so a status without the marker is the new run's. The launch
# then polls the folder, at most $TIMEOUT s; the fetched files are kept in $RESULTS/<test>.
launch_without_console() {
  local name=$1
  shift
  local marker="$RESULTS/$name.marker" now="$RESULTS/$name.poll" waited=0
  rm -rf "$now"
  : > "$marker"
  xcrun devicectl device copy to --device "$DEVICE_ID" --domain-type appDataContainer \
    --domain-identifier "$BUNDLE_ID" --source "$marker" --destination "tmp/fixtures/$name/output/stale-marker" \
    >/dev/null 2>&1 || true
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --json-output \
    "$RESULTS/$name.launch.json" "$BUNDLE_ID" -- "$@" >>"$RESULTS/$name.console" 2>&1 || return 1
  while [[ "$waited" -lt "$TIMEOUT" ]]; do
    sleep "${POLL_SECONDS:-2}"
    waited=$((waited + ${POLL_SECONDS:-2}))
    rm -rf "$now"
    if xcrun devicectl device copy from --device "$DEVICE_ID" --domain-type appDataContainer \
        --domain-identifier "$BUNDLE_ID" --source "tmp/fixtures/$name/output" --destination "$now" >/dev/null 2>&1 &&
        [[ -f "$now/status" && ! -e "$now/stale-marker" ]]; then
      rm -rf "${RESULTS:?}/$name" "$marker"
      mv "$now" "$RESULTS/$name"
      FETCHED=$name
      return 0
    fi
  done
  rm -rf "$now" "$marker"
  echo "no new output/status within $TIMEOUT s" >>"$RESULTS/$name.console"
  return 1
}

# Copies what the program left in the app's tmp/fixtures/<test>/output to $RESULTS/<test>.
fetch() {
  local name=$1
  if [[ "$SIMULATOR" == 1 ]]; then
    local container
    container=$(xcrun simctl get_app_container "$DEVICE_ID" "$BUNDLE_ID" data) || return 1
    cp -R "$container/tmp/fixtures/$name/output" "$RESULTS/$name"
    return
  fi
  [[ "${FETCHED:-}" != "$name" ]] || return 0  # the launch without a console fetched it already
  xcrun devicectl device copy from --device "$DEVICE_ID" --domain-type appDataContainer \
    --domain-identifier "$BUNDLE_ID" --source "tmp/fixtures/$name/output" --destination "$RESULTS/$name" \
    >>"$RESULTS/$name.console" 2>&1
}

# The run time and the footprint peak from the runtime's exit measures (apkrt_report_exit_measures),
# and the memory available at the program's start where the system tells it (the iPhone; 0 in the Simulator).
measured() {
  awk '$1 == "run-ms" { run = $2 } $1 == "footprint-peak" { peak = $2 } $1 == "available-at-start" { available = $2 }
       END { if (run != "") printf "; run %s ms, footprint peak %.1f MB", run, peak / 1e6
             if (available > 0) printf ", %.2f GB available at the start", available / 1e9 }' "$1" 2>/dev/null || true
}

mkdir -p "$RESULTS"
failed=0
started=$(date +%s)
for name in "${tests[@]}"; do
  rm -rf "${RESULTS:?}/$name" "$RESULTS/$name.launch.json" "$RESULTS/$name.console" "$RESULTS/$name.expected"
  begin=$(python3 -c 'import time; print(int(time.time() * 1000))')
  if ! launch "$name"; then
    echo "FAILED   $name: the app did not run it, see $RESULTS/$name.console"
    failed=1
    continue
  fi
  milliseconds=$(($(python3 -c 'import time; print(int(time.time() * 1000))') - begin))
  if ! fetch "$name"; then
    echo "FAILED   $name: no output on the device, see $RESULTS/$name.console"
    failed=1
    continue
  fi
  if [[ -n "$ART" ]]; then
    # Compared as on the Mac (runtime/tests/device_art.py); without increased-memory-limit the
    # heap window is its upper 2 GiB.
    window=()
    [[ "${ENTITLEMENT:-with}" == with ]] || window=(--upper-half)
    traced=$(cat "$RESULTS/$name/traced" 2>/dev/null || true)
    if differences=$(python3 runtime/tests/device_art.py check build/device-art "$RESULTS" . \
         ${window[@]+"${window[@]}"} "$name") && [[ "$traced" == 0 ]]; then
      printf 'same     %-26s %5d ms with the launch%s\n' "$name" "$milliseconds" "$(measured "$RESULTS/$name/measures")"
    else
      failed=1
      echo "${differences:-DIFFERS  $name:}"
      [[ "$traced" == 0 ]] || echo "    debugger-attached"
    fi
    continue
  fi

  # The exit status as the runtime reported it (apkrt_report_exit_status); devicectl's exit code is
  # not reliable for a process that ends right after its start. The bare number is the first line; a self-kill's
  # cause follows on the lines after it (ADR-0032 d8, process_exit.h note_self_kill), so compare the number alone.
  status=$(head -n 1 "$RESULTS/$name/status" 2>/dev/null | tr -d '\n' || true)
  expected=fixtures/expected/$name
  expected_out=$expected.out
  if [[ -n "$FRAME_RATE" ]]; then
    expected_out=$RESULTS/$name.expected
    python3 runtime/tests/frame_rate_run.py --lines "$FRAME_RATE" >"$expected_out"
  fi
  expected_status=$(field "$name" status)
  measures=$(field "$name" measures)
  problems=""
  [[ "$measures" == yes ]] || cmp -s "$expected_out" "$RESULTS/$name/stdout" || problems+=" stdout"
  if [[ -f "$expected.err" ]]; then
    cmp -s "$expected.err" "$RESULTS/$name/stderr" || problems+=" stderr"
  elif [[ -s "$RESULTS/$name/stderr" ]]; then
    problems+=" stderr"
  fi
  [[ "$status" == "${expected_status:-0}" ]] || problems+=" exit-status-${status:-unknown}"
  # M2's criterion: without a debugger (app/Fixtures/FixtureRunner.swift checks P_TRACED).
  [[ "$(cat "$RESULTS/$name/traced" 2>/dev/null)" == 0 ]] || problems+=" debugger-attached"
  signing=""
  if [[ "$SIGN_ON_DEVICE" == 1 ]]; then
    if [[ -s "$RESULTS/$name/signing" ]]; then
      # name, container bytes, milliseconds converting, milliseconds signing
      signing=$(awk '{ files++; bytes += $2; convert += $3; sign += $4 }
        END { plural = (files == 1) ? "" : "s"
              printf "; on the device %d file%s, %.0f KiB: converted in %.1f ms, signed in %.1f ms",
                     files, plural, bytes / 1024, convert, sign }' "$RESULTS/$name/signing")
    else
      problems+=" not-signed-on-the-device"
    fi
  fi
  if [[ -z "$problems" && "$measures" == yes ]]; then
    printf 'measured %-26s %5d ms with the launch%s\n' "$name" "$milliseconds" "$signing"
    sed 's/^/    /' "$RESULTS/$name/stdout"
  elif [[ -z "$problems" ]]; then
    printf 'same     %-26s %5d ms with the launch%s\n' "$name" "$milliseconds" "$signing"
  else
    failed=1
    echo "DIFFERS  $name:$problems"
    if [[ "$measures" == yes ]]; then
      sed 's/^/    /' "$RESULTS/$name/stdout"
    else
      diff -u "$expected_out" "$RESULTS/$name/stdout" | sed 's/^/    /' || true
    fi
    if [[ -f "$expected.err" ]]; then
      diff -u "$expected.err" "$RESULTS/$name/stderr" | sed 's/^/    /' || true
    else
      sed 's/^/    stderr: /' "$RESULTS/$name/stderr"
    fi
  fi
done

# The iphone-only tests (fixtures/iphone-only.txt, memory-limit-test): each allocates until iOS ends the app for
# memory (jetsam), so the app dying IS the expected outcome, not a failure. Afterwards output/memory must hold the
# watermark the runtime pwrote before the end (apkrt_report_memory_to): a footprint that grew large and a peak at
# least that. Refused under --simulator, which runs on the Mac's memory and has no such limit.
for name in ${iphone_only_tests[@]+"${iphone_only_tests[@]}"}; do
  if [[ "$SIMULATOR" == 1 ]]; then
    echo "SKIPPED  $name: allocates until iOS jetsam, a real iPhone only, never the Simulator" \
      "(fixtures/iphone-only.txt)"
    continue
  fi
  rm -rf "${RESULTS:?}/$name" "$RESULTS/$name.launch.json" "$RESULTS/$name.console"
  launch "$name" || true  # iOS ends the app at its memory limit: expected, not a failure
  if ! fetch "$name"; then
    echo "FAILED   $name: no output on the device, see $RESULTS/$name.console"
    failed=1
    continue
  fi
  last=$(grep "MiB resident" "$RESULTS/$name/stdout" 2>/dev/null | tail -n 1 || true)
  memory="$RESULTS/$name/memory"
  if [[ ! -s "$memory" ]]; then
    echo "DIFFERS  $name: no memory watermark after the end -- last-session.memory is empty${last:+; last step: $last}"
    failed=1
    continue
  fi
  read -r footprint available peak warning < <(awk '
    $1 == "footprint" { f = $2 } $1 == "available" { a = $2 } $1 == "peak" { p = $2 } $1 == "warning" { w = $2 }
    END { print f + 0, a + 0, p + 0, w + 0 }' "$memory")
  if [[ "$footprint" -gt 1000000000 && "$peak" -ge "$footprint" ]]; then
    awk -v n="$name" -v f="$footprint" -v a="$available" -v p="$peak" -v w="$warning" -v s="$last" \
      'BEGIN { printf "memory   %-26s footprint %.0f MB, peak %.0f MB, available %.0f MB, warning %s; %s\n",
               n, f / 1e6, p / 1e6, a / 1e6, w, s }'
  else
    echo "DIFFERS  $name: footprint $footprint peak $peak" \
      "(want a footprint over 1 GB and peak >= footprint)${last:+; $last}"
    failed=1
  fi
done

where="in the Simulator"
[[ "$SIMULATOR" == 1 ]] || where="on the iPhone, $ENTITLEMENT increased-memory-limit"
how="signed on the Mac (M2a)"
[[ "$SIGN_ON_DEVICE" == 0 ]] || how="converted and signed by the app (M2b)"
[[ -z "$FRAME_RATE" ]] || how+=", with the FPS unlock at $FRAME_RATE"
what="tests"
if [[ -n "$ART" ]]; then
  what="of ART's tests in its release flavor"
  [[ "$DEBUG" == 0 ]] || what="of ART's tests in its debug flavor"
fi
echo "${#tests[@]} $what $where, $how, in $(($(date +%s) - started)) s"
exit $failed
