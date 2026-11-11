#!/usr/bin/env bash
# Runs the hidden API policy test (the hidden API plan) on the Mac (framework/tests/java_app_run.py) for its three
# builds from one source: hidden-api-test for targetSdk 35, hidden-api-test-33 and hidden-api-test-28. Each app looks up
# hidden members of the platform's classes by reflection (one per list of Android 15's hiddenapi-flags.csv) and logs
# which it found and which of them getDeclaredFields/Methods/Constructors lists, and, through StrictMode's
# detectNonSdkApiUsage, each hidden member the platform reports. Android 15's log of each is
# fixtures/expected/<app>.logcat (tools/android-reference.sh --app <app>).
# Needs framework.jar with the flags in its dex files: tools/hiddenapi-encode.py writes them from ~/.android/minato-
# hiddenapi/hiddenapi-flags.csv (third_party/sources.json, hiddenapi-flags); without that file no member is hidden and
# this check stops with "skipped".
#
# MINATO_HIDDEN_API_POLICY (default just-warn, the policy app_process starts with): ART's policy for the run. With
# just-warn nothing is denied, so only what the policy does not change is compared with Android's log: the target line,
# every StrictMode line (the members the platform reports, in order) and the last line; and no line of the process's
# log may say "denied)". With enabled, Android's default, the whole log is compared, each member found or not; until
# the test API policy is on as well (Android denies a member that is blocked and a test API, ART's default here
# allows it) the one line of addDumpable differs.
# Usage: tools/hidden-api-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

policy=${MINATO_HIDDEN_API_POLICY:-just-warn}
flags=${MINATO_HIDDENAPI_FLAGS:-$HOME/.android/minato-hiddenapi/hiddenapi-flags.csv}
if [[ ! -f "$flags" ]]; then
  echo "skipped: $flags is missing, so framework.jar carries no hidden API flags" \
    "(third_party/sources.json, hiddenapi-flags)"
  exit 0
fi

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

for app in hidden-api-test hidden-api-test-33 hidden-api-test-28; do
  tools/build-java-test-app.sh "$app" >/dev/null
  package=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "fixtures/apps/$app/AndroidManifest.xml" | head -n 1)
  work="build/$app-check"
  mkdir -p "$work"
  expected="fixtures/expected/$app.logcat"
  extra=(--never "denied)")
  if [[ "$policy" == enabled ]]; then
    extra=(--env MINATO_MEASURE_ART_OPTIONS=-Xhidden-api-policy:enabled)
  else
    # Only the lines the policy does not change: the runner takes them as "logcat <line>" lines of a .txt file that must
    # be in the process's log, and --in-order holds their order among the log's other lines.
    expected="$work/expected.txt"
    grep -E "^I/$app: (target SDK|StrictMode: |done)" "fixtures/expected/$app.logcat" >"$work/lines"
    sed 's/^/logcat /' "$work/lines" >"$expected"
    while IFS= read -r line; do extra+=(--in-order "$line"); done <"$work/lines"
    [[ "$policy" == just-warn ]] || extra+=(--env "MINATO_MEASURE_ART_OPTIONS=-Xhidden-api-policy:$policy")
  fi
  if ! output=$(python3 framework/tests/java_app_run.py ${extra[@]+"${extra[@]}"} "$app" "$package" \
      io.github.kisame76.minato.test.hiddenapi.HiddenApiApplication build/art build/bionic/lib build/bionic/system \
      build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
      "$NATIVE/runtime/apk-run" "$work/run" build/framework-native/app_process64 build/framework/framework.jar \
      build/framework-res/framework-res "build/$app/$app.apk" "$expected"); then
    echo "$output"
    exit 1
  fi
  echo "$output"
done
