#!/usr/bin/env bash
# The parity benchmark (ADR-0037): builds the benchmark build (Release, APK_EMBED_BENCHMARK=YES),
# installs it, starts every run of the series with -runBenchmark <run> without a debugger, in the
# order bench-plan gives (A B B A, 5 valid runs per variant, an invalid run repeated at once), and
# fetches each run's CSVs to build/benchmark/<run>/ (build/benchmark-simulator/ for the Simulator).
# Results already there count, so a stopped series goes on where it stopped; bench-plan stops it
# after 3 invalid runs in a row of one configuration, or at once when the pair's layers differ.
#   --api gles|vulkan    only this API's series
#   --more <config>      5 more runs per variant of a configuration, e.g. gles-120-heavy (decision 8)
#   --seconds <n>        a shorter draw time than 52 s, for the Simulator (its graphics are not judged)
#   --build-check        checks the build setting instead: a Release build without it holds neither
#                        MinatoBench.framework, the APKs nor the mode, one with it holds the mode, and
#                        both carry the Game Mode keys (decisions 3 and 9). Needs the Simulator.
# Usage: tools/benchmark.sh [--simulator] [--device <name or identifier>] [--api <api>]
#                           [--more <configuration> ...] [--seconds <n>]
#        tools/benchmark.sh --build-check [simulator name or UDID]
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
CHECK=build/benchmark-check

# The build-time values of a Release build in the Simulator, into $CHECK/<name>.
simulator_release() {
  local name=$1 simulator=$2
  shift 2
  xcodebuild -project Minato.xcodeproj -scheme Minato -configuration Release -destination "id=$simulator" \
    -derivedDataPath "$CHECK/$name" -quiet build ARCHS=arm64 ONLY_ACTIVE_ARCH=YES "$@"
  echo "$CHECK/$name/Build/Products/Release-iphonesimulator/Minato.app"
}

# Prints what is wrong with the app at $1; $2 is "with" or "without" the setting.
check_app() {
  local app=$1 setting=$2 problems=""
  [[ "$(plutil -extract LSSupportsGameMode raw "$app/Info.plist" 2>/dev/null)" == true ]] ||
    problems+=" no-LSSupportsGameMode"
  [[ "$(plutil -extract LSApplicationCategoryType raw "$app/Info.plist" 2>/dev/null)" == public.app-category.games ]] ||
    problems+=" no-games-category"
  # The mode's type name, from Swift's reflection metadata; "-runBenchmark" itself is a small string,
  # stored in the code, not as text.
  local mode=0
  [[ "$(strings -a "$app/Minato" | grep -c BenchmarkRunner)" == 0 ]] || mode=1
  if [[ "$setting" == without ]]; then
    [[ "$mode" == 0 ]] || problems+=" mode"
    [[ ! -e "$app/Frameworks/MinatoBench.framework" ]] || problems+=" MinatoBench.framework"
    [[ ! -e "$app/Benchmark" ]] || problems+=" Benchmark"
  else
    [[ "$mode" == 1 ]] || problems+=" no-mode"
  fi
  echo "$problems"
}

if [[ "${1:-}" == "--build-check" ]]; then
  read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${2:-}") || true
  if [[ -z "${SIM_UDID:-}" ]]; then
    echo "No matching iPhone simulator found." >&2
    exit 1
  fi
  tools/gen.sh
  tools/build-native.sh --app
  tools/build-graphics-host.sh
  failed=0
  # A benchmark build is a Release build: in Debug the setting is an error.
  if [[ ! -x tools/embed-benchmark.sh ]]; then
    echo "DIFFERS  no tools/embed-benchmark.sh"
    failed=1
  elif CONFIGURATION=Debug APK_EMBED_BENCHMARK=YES SRCROOT="$PWD" TARGET_BUILD_DIR="$CHECK/debug" \
    tools/embed-benchmark.sh >/dev/null 2>&1; then
    echo "DIFFERS  Debug with APK_EMBED_BENCHMARK=YES is not refused"
    failed=1
  fi
  for setting in without with; do
    value=NO
    [[ "$setting" == without ]] || value=YES
    app=$(simulator_release "$setting" "$SIM_UDID" APK_EMBED_BENCHMARK=$value | tail -1)
    problems=$(check_app "$app" "$setting")
    if [[ -z "$problems" ]]; then
      echo "same     Release $setting APK_EMBED_BENCHMARK"
    else
      echo "DIFFERS  Release $setting APK_EMBED_BENCHMARK:$problems"
      failed=1
    fi
  done
  exit $failed
fi

SIMULATOR=0
WANTED_DEVICE=""
SECONDS_ARGS=()
PLAN_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --simulator) SIMULATOR=1; shift ;;
    --device) WANTED_DEVICE="$2"; shift 2 ;;
    --api | --more) PLAN_ARGS+=("$1" "$2"); shift 2 ;;
    --seconds) SECONDS_ARGS=(-benchmarkSeconds "$2"); shift 2 ;;
    *) echo "Unknown argument $1" >&2; exit 2 ;;
  esac
done
RESULTS=build/benchmark
[[ "$SIMULATOR" == 0 ]] || RESULTS=build/benchmark-simulator
mkdir -p "$RESULTS"

swift build --package-path importer/APKKit --scratch-path build/swift-app -j 6 --product bench-plan >/dev/null
PLAN="$(swift build --package-path importer/APKKit --scratch-path build/swift-app --show-bin-path)/bench-plan"

# The run.csv names the commit; -dirty when the tree has changes.
COMMIT=$(git rev-parse --short HEAD)
[[ -z "$(git status --porcelain)" ]] || COMMIT+=-dirty
SETTINGS="APK_EMBED_BENCHMARK=YES MINATO_COMMIT=$COMMIT"
if [[ "$SIMULATOR" == 1 ]]; then
  CONFIGURATION=Release BUILD_SETTINGS="$SETTINGS ARCHS=arm64 ONLY_ACTIVE_ARCH=YES" INSTALL_ONLY=1 \
    tools/sim.sh --headless ${WANTED_DEVICE:+"$WANTED_DEVICE"}
  read -r DEVICE_ID _ < <(python3 tools/find-simulator.py "$WANTED_DEVICE") || true
else
  CONFIGURATION=Release BUILD_SETTINGS="$SETTINGS" INSTALL_ONLY=1 tools/deploy.sh ${WANTED_DEVICE:+"$WANTED_DEVICE"}
  read -r DEVICE_ID _ < <(python3 tools/find-device.py "$WANTED_DEVICE") || true
fi
if [[ -z "${DEVICE_ID:-}" ]]; then
  echo "No device found." >&2
  exit 1
fi

# Starts the run and waits until the app has ended: the thermal wait, the draw time and the CSVs.
launch() {
  local run=$1
  if [[ "$SIMULATOR" == 1 ]]; then
    xcrun simctl launch --console --terminate-running-process "$DEVICE_ID" "$BUNDLE_ID" \
      -runBenchmark "$run" ${SECONDS_ARGS[@]+"${SECONDS_ARGS[@]}"} >"$RESULTS/$run.console" 2>&1 || true
    return
  fi
  # "--" ends devicectl's options. Without --start-stopped no debugger attaches.
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --console --timeout 900 \
    "$BUNDLE_ID" -- -runBenchmark "$run" ${SECONDS_ARGS[@]+"${SECONDS_ARGS[@]}"} \
    >"$RESULTS/$run.console" 2>&1 || true
}

# Copies the app's tmp/benchmark/<run> to $RESULTS/<run>. Only into the app's own data container,
# and never with --remove-existing-content, which empties the whole container.
fetch() {
  local run=$1
  if [[ "$SIMULATOR" == 1 ]]; then
    local container
    container=$(xcrun simctl get_app_container "$DEVICE_ID" "$BUNDLE_ID" data) || return 1
    cp -R "$container/tmp/benchmark/$run" "$RESULTS/$run"
    return
  fi
  xcrun devicectl device copy from --device "$DEVICE_ID" --domain-type appDataContainer \
    --domain-identifier "$BUNDLE_ID" --source "tmp/benchmark/$run" --destination "$RESULTS/$run" \
    >>"$RESULTS/$run.console" 2>&1
}

started=$(date +%s)
while true; do
  status=0
  step=$("$PLAN" next "$RESULTS" ${PLAN_ARGS[@]+"${PLAN_ARGS[@]}"}) || status=$?
  if [[ "$status" != 0 ]]; then
    echo "STOPPED  the series, see above" >&2
    exit 1
  fi
  [[ "$step" != done ]] || break
  # The run names a folder that is emptied below: anything but "run <run ID>" stops the series
  # before it is used, so a wrong line cannot remove the results.
  if [[ ! "$step" =~ ^run\ ((gles|vulkan)-[0-9]+-(light|heavy)-(native|apk)-[0-9]{2})$ ]]; then
    echo "STOPPED  bench-plan answered neither \"done\" nor \"run <run ID>\": '$step'" >&2
    exit 1
  fi
  run=${BASH_REMATCH[1]}
  rm -rf "${RESULTS:?}/${run:?}" "$RESULTS/$run.console"
  launch "$run"
  if ! fetch "$run" || [[ ! -f "$RESULTS/$run/run.csv" ]]; then
    echo "FAILED   $run: no run.csv, see $RESULTS/$run.console" >&2
    exit 1
  fi
  python3 - "$RESULTS/$run" <<'PY'
import csv, pathlib, sys
folder = pathlib.Path(sys.argv[1])
row = next(csv.DictReader(open(folder / "run.csv")))
error = folder / "error"
note = f"  ({error.read_text().strip()})" if error.exists() else ""
state = "valid   " if row["valid"] == "1" else "invalid "
print(f"{state} {row['run']}: {row['invalid_reasons'] or 'ok'}{note}")
PY
done
where="in the Simulator (graphics not judged there)"
[[ "$SIMULATOR" == 1 ]] || where="on the iPhone"
echo "Series done $where in $(($(date +%s) - started)) s; results in $RESULTS"
