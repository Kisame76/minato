#!/usr/bin/env bash
# The parity benchmark (ADR-0037): builds the benchmark build (Release, APK_EMBED_BENCHMARK=YES),
# installs it, starts every run of the series with -runBenchmark <run> without a debugger, in the
# order bench-plan gives (A B B A, 5 valid runs per variant, an invalid run repeated at once), and
# fetches each run's CSVs to build/benchmark/<run>/ (build/benchmark-simulator/ for the Simulator).
# Results already there count, so a stopped series goes on where it stopped; bench-plan stops it
# after 3 invalid runs in a row of one configuration, or at once when the pair's layers differ.
#   --scene godot        test 2's series instead (ADR-0042 c): the Godot APKs against the twin, Godot's iOS template in
#                        the benchmark build, through tools/build-godot.sh and APK_EMBED_GODOT=YES; the twin runs on
#                        the iPhone only
#   --api <api>          only this API's series: gles, vulkan, or gles31, bench-gles's scene on ANGLE's Vulkan
#                        backend (ADR-0044 d2)); metal too with --scene godot, the twin's own
#   --more <config>      5 more runs per variant of a configuration, e.g. gles-120-heavy (decision 8)
#   --seconds <n>        a shorter draw time than 52 s, for the Simulator (its graphics are not judged)
#   --build-check        checks the build setting instead: a Release build without it holds neither
#                        MinatoBench.framework, the APKs nor the mode; one with it holds all three and
#                        their license texts (tools/license-check.py); both carry the Game Mode keys
#                        (decisions 3 and 9). The setting is refused in Debug, with the runtime's or
#                        graphics/host's Debug builds, and without the framework or an APK. Needs the Simulator.
# The benchmark build is Release, with tools/build-native.sh --app --release and
# tools/build-graphics-host.sh --release (through tools/deploy.sh and tools/sim.sh); their next
# Debug run packs the Debug builds again.
# Usage: tools/benchmark.sh [--simulator] [--device <name or identifier>] [--scene godot] [--api <api>]
#                           [--more <configuration> ...] [--seconds <n>] [--cap <n> [--unlock 60|120]]
# --cap gives ADR-0037's scene an engine cap, the targetFrameRate its engine sets (-1 or 1 to 1000), and --unlock the
# FPS unlock over it (ADR-0066 step 4): a by-hand measure that run.csv does not record, so its results go to a folder
# of their own, build/benchmark-cap<n>[-unlock<m>].
#        tools/benchmark.sh --build-check [simulator name or UDID]
#        tools/benchmark.sh --embed-check   the build phase's cases of --build-check alone, without a build
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
CHECK=build/benchmark-check

# The build-time values of a Release build in the Simulator, into $CHECK/<name>; prints the app's path. xcodebuild's
# output goes to $CHECK/<name>.log, whose end goes to stderr when the build fails.
simulator_release() {
  local name=$1 simulator=$2
  shift 2
  mkdir -p "$CHECK"
  if ! xcodebuild -project Minato.xcodeproj -scheme Minato -configuration Release -destination "id=$simulator" \
    -derivedDataPath "$CHECK/$name" -quiet build ARCHS=arm64 ONLY_ACTIVE_ARCH=YES "$@" >"$CHECK/$name.log" 2>&1; then
    echo "The Release build $name failed; the end of $CHECK/$name.log:" >&2
    tail -40 "$CHECK/$name.log" >&2
    return 1
  fi
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
  # Test 2's twin and pack come only with APK_EMBED_GODOT=YES, which neither build sets.
  [[ ! -e "$app/Frameworks/MinatoGodot.framework" ]] || problems+=" MinatoGodot.framework"
  [[ ! -e "$app/Minato.pck" ]] || problems+=" Minato.pck"
  if [[ "$setting" == without ]]; then
    [[ "$mode" == 0 ]] || problems+=" mode"
    [[ ! -e "$app/Frameworks/MinatoBench.framework" ]] || problems+=" MinatoBench.framework"
    [[ ! -e "$app/Benchmark" ]] || problems+=" Benchmark"
  else
    [[ "$mode" == 1 ]] || problems+=" no-mode"
    [[ -f "$app/Frameworks/MinatoBench.framework/MinatoBench" ]] || problems+=" no-MinatoBench.framework"
    local apk
    for apk in bench-gles.apk bench-vulkan.apk bench-gles31.apk; do
      [[ -f "$app/Benchmark/$apk" ]] || problems+=" no-$apk"
    done
    python3 tools/license-check.py "$app" >"$CHECK/license-check.txt" 2>&1 || problems+=" license-check"
  fi
  echo "$problems"
}

# The build phase alone, tools/embed-benchmark.sh, in scratch trees: each case with only its own flaw, and one without a
# flaw that it takes, so a refusal is the flaw's. A benchmark build is a Release build of Release libraries with the
# framework and both APKs; with APK_EMBED_GODOT=YES also test 2's APKs, and on iOS devices the twin and its pack
# (ADR-0042 c). Every case starts from what an earlier build left in the app, which a build must remove unless it
# embeds it again. The outcome names what the app holds: bench (MinatoBench.framework and the scene's APKs),
# godot-apks, twin (MinatoGodot.framework) and pack (Minato.pck).
embed_checks() {
  local failed=0
  embed_case() {
    local expected=$2 name=$3 configuration=$4 libraries=$5 missing=$6 platform=${7:-iphonesimulator}
    local godot=${8:-NO} benchmark=${9:-YES} tree="$CHECK/tree-$1" app outcome slice xcframework
    rm -rf "$tree"
    for slice in ios-arm64 ios-arm64-simulator; do
      mkdir -p "$tree/build/MinatoBench.xcframework/$slice/MinatoBench.framework"
      echo stand-in >"$tree/build/MinatoBench.xcframework/$slice/MinatoBench.framework/MinatoBench"
    done
    mkdir -p "$tree/build/MinatoGodot.xcframework/ios-arm64/MinatoGodot.framework" "$tree/build/graphics/bench" \
      "$tree/build/godot"
    echo stand-in >"$tree/build/MinatoGodot.xcframework/ios-arm64/MinatoGodot.framework/MinatoGodot"
    touch "$tree/build/graphics/bench/bench-gles.apk" "$tree/build/graphics/bench/bench-vulkan.apk" \
      "$tree/build/graphics/bench/bench-gles31.apk" "$tree/build/godot/godot-bench-gles.apk" \
      "$tree/build/godot/godot-bench-vulkan.apk" "$tree/build/godot/Minato.pck"
    for xcframework in APKRuntime APKConvert MinatoGraphics; do
      echo "$libraries" >"$tree/build/$xcframework.xcframework.configuration"
    done
    app="$tree/app/Minato.app"
    mkdir -p "$app/Frameworks/MinatoBench.framework" "$app/Frameworks/MinatoGodot.framework" "$app/Benchmark"
    touch "$app/Minato.pck" "$app/Benchmark/godot-bench-vulkan.apk"
    [[ -z "$missing" ]] || rm -rf "${tree:?}/$missing"
    if CONFIGURATION=$configuration APK_EMBED_BENCHMARK=$benchmark APK_EMBED_GODOT=$godot SRCROOT="$tree" \
      TARGET_BUILD_DIR="$tree/app" FRAMEWORKS_FOLDER_PATH=Minato.app/Frameworks \
      UNLOCALIZED_RESOURCES_FOLDER_PATH=Minato.app PLATFORM_NAME=$platform tools/embed-benchmark.sh \
      >"$tree/out.txt" 2>&1; then
      outcome=embedded
      [[ ! -e "$app/Frameworks/MinatoBench.framework" ]] || outcome+=" bench"
      [[ ! -e "$app/Benchmark/godot-bench-vulkan.apk" ]] || outcome+=" godot-apks"
      [[ ! -e "$app/Frameworks/MinatoGodot.framework" ]] || outcome+=" twin"
      [[ ! -e "$app/Minato.pck" ]] || outcome+=" pack"
      [[ "$outcome" != embedded ]] || outcome="embedded nothing"
    else
      outcome=refused
    fi
    if [[ "$outcome" == "$expected" ]]; then
      echo "same     $outcome: $name"
    else
      echo "DIFFERS  $outcome, not $expected: $name"
      failed=1
    fi
  }
  embed_case complete "embedded bench" "a Release build of Release libraries" Release Release ""
  embed_case device "embedded bench" "the same for an iOS device" Release Release "" iphoneos
  embed_case debug refused "a Debug build" Debug Release ""
  embed_case libraries refused "the runtime's Debug build" Release Debug ""
  embed_case framework refused "no MinatoBench.framework" Release Release build/MinatoBench.xcframework
  embed_case apk refused "no bench-vulkan.apk" Release Release build/graphics/bench/bench-vulkan.apk
  embed_case apk-es31 refused "no bench-gles31.apk" Release Release build/graphics/bench/bench-gles31.apk
  embed_case plain "embedded nothing" "a Release build without the setting" Release Release "" iphoneos NO NO
  embed_case plain-debug "embedded nothing" "a Debug build without the setting" Debug Debug "" iphonesimulator NO NO
  embed_case godot "embedded bench godot-apks twin pack" "test 2 for an iOS device" Release Release "" iphoneos YES
  embed_case godot-simulator "embedded bench godot-apks" "test 2 in the Simulator: no twin, no pack" \
    Release Release "" iphonesimulator YES
  embed_case godot-twin refused "test 2 without the twin" Release Release build/MinatoGodot.xcframework iphoneos YES
  embed_case godot-pack refused "test 2 without Minato.pck" Release Release build/godot/Minato.pck iphoneos YES
  embed_case godot-apk refused "test 2 without godot-bench-gles.apk" Release Release build/godot/godot-bench-gles.apk \
    iphonesimulator YES
  embed_case godot-alone refused "test 2 without the benchmark setting" Release Release "" iphoneos YES NO
  return $failed
}

if [[ "${1:-}" == "--embed-check" ]]; then
  embed_checks
  exit $?
fi

if [[ "${1:-}" == "--build-check" ]]; then
  read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${2:-}") || true
  if [[ -z "${SIM_UDID:-}" ]]; then
    echo "No matching iPhone simulator found." >&2
    exit 1
  fi
  failed=0
  embed_checks || failed=1
  tools/gen.sh
  tools/build-bench.sh
  tools/build-bench.sh --ios
  tools/build-native.sh --app --release
  tools/build-graphics-host.sh --release
  for setting in without with; do
    value=NO
    [[ "$setting" == without ]] || value=YES
    if ! app=$(simulator_release "$setting" "$SIM_UDID" APK_EMBED_BENCHMARK=$value); then
      echo "FAILED   Release $setting APK_EMBED_BENCHMARK: the build failed"
      failed=1
      continue
    fi
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
GODOT=0
WANTED_DEVICE=""
SECONDS_ARGS=()
CAP_ARGS=()
CAP_NAME=""
PLAN_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --simulator) SIMULATOR=1; shift ;;
    --device) WANTED_DEVICE="$2"; shift 2 ;;
    --scene)
      [[ "${2:-}" == godot ]] || { echo "Unknown scene ${2:-}" >&2; exit 2; }
      GODOT=1; PLAN_ARGS+=(--scene godot); shift 2 ;;
    --api | --more) PLAN_ARGS+=("$1" "$2"); shift 2 ;;
    --seconds) SECONDS_ARGS=(-benchmarkSeconds "$2"); shift 2 ;;
    --cap) CAP_ARGS+=(-benchmarkCap "$2"); CAP_NAME="-cap$2$CAP_NAME"; shift 2 ;;
    --unlock) CAP_ARGS+=(-benchmarkUnlock "$2"); CAP_NAME="$CAP_NAME-unlock$2"; shift 2 ;;
    *) echo "Unknown argument $1" >&2; exit 2 ;;
  esac
done
# The iPhone only by cable, never over the network, checked before the builds.
[[ "$SIMULATOR" == 1 ]] || python3 tools/require-cable.py "$WANTED_DEVICE" || exit 1
RESULTS=build/benchmark
[[ "$SIMULATOR" == 0 ]] || RESULTS=build/benchmark-simulator
RESULTS=$RESULTS$CAP_NAME
mkdir -p "$RESULTS"

swift build --package-path importer/APKKit --scratch-path build/swift-app -j 6 --product bench-plan >/dev/null
# The APKs and MinatoBench.framework, which the build embeds (tools/embed-benchmark.sh).
tools/build-bench.sh
tools/build-bench.sh --ios
# Test 2's: both APKs, and for the iPhone the twin's pack and framework.
if [[ "$GODOT" == 1 ]]; then
  tools/build-godot.sh
  if [[ "$SIMULATOR" == 0 ]]; then
    tools/build-godot.sh --pack
    tools/build-godot.sh --twin
  fi
fi
PLAN="$(swift build --package-path importer/APKKit --scratch-path build/swift-app --show-bin-path)/bench-plan"

# The run.csv names the commit; -dirty when the tree has changes.
COMMIT=$(git rev-parse --short HEAD)
[[ -z "$(git status --porcelain)" ]] || COMMIT+=-dirty
SETTINGS="APK_EMBED_BENCHMARK=YES MINATO_COMMIT=$COMMIT"
[[ "$GODOT" == 0 ]] || SETTINGS+=" APK_EMBED_GODOT=YES"
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
      -runBenchmark "$run" ${SECONDS_ARGS[@]+"${SECONDS_ARGS[@]}"} ${CAP_ARGS[@]+"${CAP_ARGS[@]}"} \
      >"$RESULTS/$run.console" 2>&1 || true
    return
  fi
  # "--" ends devicectl's options. Without --start-stopped no debugger attaches.
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --console --timeout 900 \
    "$BUNDLE_ID" -- -runBenchmark "$run" ${SECONDS_ARGS[@]+"${SECONDS_ARGS[@]}"} ${CAP_ARGS[@]+"${CAP_ARGS[@]}"} \
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
  if [[ ! "$step" =~ ^run\ ((godot-)?(gles(31)?|vulkan|metal)-[0-9]+-(light|heavy)-(native|apk|twin)-[0-9]{2})$ ]]; then
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
