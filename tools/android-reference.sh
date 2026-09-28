#!/usr/bin/env bash
# Runs the NDK fixtures on real Android and compares what they print with fixtures/expected: the
# reference check for the expected outputs, which are derived by hand (ADR-0014 part 1). Android is
# the Android emulator with the AVD apk-runner-35 (Android 15 AOSP image, arm64-v8a), which the script
# creates and starts without a window when no emulator runs, and stops again afterwards. Only an
# emulator is used, because every fixture starts with an empty /data/local/tmp on the device.
# The tests are those of fixtures/tests.txt, run as CTest runs them; tests of the runtime that
# Android cannot run are left out, and what a benchmark (measures) prints is shown, not compared.
# A test app of fixtures/apps (ADR-0028, test 3; ADR-0036) is installed fresh from build/fixtures/apks,
# started as the launcher starts it, and left until it finishes itself; activity-test is sent HOME a second
# after it has its first window and the focus, and started again, before that. Its lines of logcat with its
# own tag (logcat -v tag) must be fixtures/expected/<app>.logcat; those with the tag <app>-device differ by
# device and are only shown.
# Usage: tools/android-reference.sh [test ...]   (default: every test in fixtures/tests.txt)
#        tools/android-reference.sh --app <app>
# Results: build/android-reference/<test>.{out,err,logcat}, <app>.logcat and <app>-device.logcat
# Optional environment:
#   ANDROID_HOME      SDK with emulator, platform-tools and the system image, default: ~/Library/Android/sdk
#   ANDROID_AVD_HOME  where the AVD lives, default: ~/.android/avd
#   FIXTURES_BUILD    the fixtures' build directory (tools/build-fixtures.sh), default: build/fixtures
set -euo pipefail
cd "$(dirname "$0")/.."

SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
AVD_HOME="${ANDROID_AVD_HOME:-$HOME/.android/avd}"
ADB="$SDK/platform-tools/adb"
EMULATOR="$SDK/emulator/emulator"
AVD=apk-runner-35
IMAGE=system-images/android-35/default/arm64-v8a/
FIXTURES="${FIXTURES_BUILD:-build/fixtures}/bin"
APKS="${FIXTURES_BUILD:-build/fixtures}/apks"
RESULTS=build/android-reference
DEVICE_DIR=/data/local/tmp

for tool in "$ADB" "$EMULATOR" "$SDK/$IMAGE/system.img"; do
  if [[ ! -e "$tool" ]]; then
    echo "Missing $tool: install platform-tools, the emulator and $IMAGE into $SDK (docs/STATUS.md)." >&2
    exit 1
  fi
done
if [[ ! -d "$FIXTURES" ]]; then
  echo "No fixtures in $FIXTURES: run tools/build-fixtures.sh first." >&2
  exit 1
fi

# The tests of fixtures/tests.txt, which says what each line means: its test names, and the fields
# of one test (field <test> <key> prints the value of <key>=..., "yes" for a flag, or nothing).
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

app=""
if [[ $# -eq 2 && "$1" == --app ]]; then
  app=$2
  fixtures=()
elif [[ $# -gt 0 ]]; then
  fixtures=("$@")
else
  fixtures=()
  for name in $(test_names); do
    fixtures+=("$name")
  done
fi

create_avd() {
  mkdir -p "$AVD_HOME/$AVD.avd"
  cat >"$AVD_HOME/$AVD.ini" <<EOF
avd.ini.encoding=UTF-8
path=$AVD_HOME/$AVD.avd
path.rel=avd/$AVD.avd
target=android-35
EOF
  cat >"$AVD_HOME/$AVD.avd/config.ini" <<EOF
AvdId=$AVD
avd.ini.displayname=$AVD
avd.ini.encoding=UTF-8
PlayStore.enabled=false
abi.type=arm64-v8a
hw.cpu.arch=arm64
hw.cpu.ncore=4
hw.ramSize=2048
disk.dataPartition.size=2G
image.sysdir.1=$IMAGE
tag.id=default
tag.display=Default Android System Image
EOF
}

mkdir -p "$RESULTS"
"$ADB" start-server >/dev/null 2>&1
serial=$("$ADB" devices | awk '$1 ~ /^emulator-/ && $2 == "device" { print $1; exit }')
started=""
if [[ -z "$serial" ]]; then
  [[ -f "$AVD_HOME/$AVD.ini" ]] || create_avd
  serial=emulator-5554
  echo "Starting the emulator ($AVD) ..."
  ANDROID_HOME="$SDK" ANDROID_SDK_ROOT="$SDK" ANDROID_AVD_HOME="$AVD_HOME" "$EMULATOR" -avd "$AVD" -port 5554 -no-window -no-audio \
    -no-boot-anim -no-snapshot -gpu swiftshader_indirect >"$RESULTS/emulator.log" 2>&1 &
  started=$!
  trap '"$ADB" -s "$serial" emu kill >/dev/null 2>&1 || kill "$started" 2>/dev/null || true' EXIT
  for _ in $(seq 1 180); do
    if ! kill -0 "$started" 2>/dev/null; then
      echo "The emulator stopped; see $RESULTS/emulator.log." >&2
      exit 1
    fi
    [[ "$("$ADB" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]] && break
    sleep 2
  done
  if [[ "$("$ADB" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" != 1 ]]; then
    echo "The emulator did not finish booting in 6 minutes; see $RESULTS/emulator.log." >&2
    exit 1
  fi
fi
# Never with the terminal as stdin or a pty: stdout and stderr stay apart and byte for byte.
device() { "$ADB" -s "$serial" "$@" </dev/null; }
run() { device shell -T "$@"; }

echo "Android $(run getprop ro.build.version.release | tr -d '\r'), build $(run getprop ro.build.fingerprint | tr -d '\r')"

# Messages to the device's logger: in the runtime they go to stderr in logcat's "tag" format, on
# Android to logcat. An expected .err file therefore splits into logcat lines and stderr lines.
logcat_line='^[VDIWEFS]/[^:]+: '

# The app's lines of logcat with the tag $1.
app_lines() { run logcat -d -v tag -s "$1:V" | grep -v '^--------- ' || true; }
# Waits up to 30 s until the app's lines with the tag $1 contain the text $2.
wait_for() {
  for _ in $(seq 1 60); do
    app_lines "$1" | grep -q -F "$2" && return 0
    sleep 0.5
  done
  echo "$1 did not log \"$2\" within 30 s." >&2
  return 1
}

run_app() {
  local apk="$APKS/$app.apk" package activity
  if [[ ! -f "$apk" ]]; then
    echo "No $apk: run tools/build-fixtures.sh first." >&2
    return 1
  fi
  package=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "fixtures/apps/$app/AndroidManifest.xml" | head -n 1)
  activity="$package/android.app.NativeActivity"
  device uninstall "$package" >/dev/null 2>&1 || true
  device install "$apk" >/dev/null
  run logcat -c
  run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
  if [[ "$app" == activity-test ]]; then
    wait_for "$app" APP_CMD_GAINED_FOCUS || return 1
    sleep 1
    run input keyevent KEYCODE_HOME
    wait_for "$app" APP_CMD_SAVE_STATE || return 1
    sleep 1
    run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
  fi
  wait_for "$app" "android_main returns" || return 1
  sleep 1
  app_lines "$app" >"$RESULTS/$app.logcat"
  app_lines "$app-device" >"$RESULTS/$app-device.logcat"
  device uninstall "$package" >/dev/null 2>&1 || true
  if cmp -s "fixtures/expected/$app.logcat" "$RESULTS/$app.logcat"; then
    echo "same     $app"
  else
    echo "DIFFERS  $app"
    diff -u "fixtures/expected/$app.logcat" "$RESULTS/$app.logcat" | sed 's/^/    /' || true
    return 1
  fi
  sed 's/^/    /' "$RESULTS/$app-device.logcat"
}

if [[ -n "$app" ]]; then
  run_app
  exit $?
fi

failed=0
for name in "${fixtures[@]}"; do
  if [[ "$(field "$name" runtime-only)" == yes ]]; then
    echo "skipped  $name: a test of the runtime only, see fixtures/tests.txt"
    continue
  fi
  program=$(field "$name" program)
  program=${program:-$name}
  libraries=$(field "$name" libraries)
  run "rm -rf $DEVICE_DIR/* $DEVICE_DIR/.[!.]*" >/dev/null
  # shellcheck disable=SC2046
  if ! device push "$FIXTURES/$program" $(for library in ${libraries//,/ }; do echo "$FIXTURES/$library"; done) \
    "$DEVICE_DIR/" >/dev/null 2>&1; then
    echo "Cannot copy $program to the device." >&2
    exit 1
  fi
  environment="export LD_LIBRARY_PATH=$DEVICE_DIR && "
  [[ "$(field "$name" no-library-path)" != yes ]] || environment=""
  run logcat -c
  # The shell prints its pid before it becomes the fixture (exec), for logcat's --pid.
  status=0
  run "echo \$\$; cd $DEVICE_DIR && chmod 755 ./$program && ${environment}exec ./$program" \
    >"$RESULTS/$name.raw" 2>"$RESULTS/$name.err" || status=$?
  pid=$(head -n 1 "$RESULTS/$name.raw")
  tail -n +2 "$RESULTS/$name.raw" >"$RESULTS/$name.out"
  rm "$RESULTS/$name.raw"
  run logcat -d -v tag --pid="$pid" | grep -v '^--------- ' >"$RESULTS/$name.logcat" || true

  expected=fixtures/expected/$name
  expected_err=$(mktemp)
  expected_logcat=$(mktemp)
  if [[ -f "$expected.err" ]]; then
    grep -Ev "$logcat_line" "$expected.err" >"$expected_err" || true
    grep -E "$logcat_line" "$expected.err" >"$expected_logcat" || true
  fi
  measures=$(field "$name" measures)
  problems=""
  [[ "$measures" == yes ]] || cmp -s "$expected.out" "$RESULTS/$name.out" || problems+=" stdout"
  cmp -s "$expected_err" "$RESULTS/$name.err" || problems+=" stderr"
  cmp -s "$expected_logcat" "$RESULTS/$name.logcat" || problems+=" logcat"
  expected_status=$(field "$name" status)
  [[ $status -eq ${expected_status:-0} ]] || problems+=" exit-status-$status"
  if [[ -z "$problems" && "$measures" == yes ]]; then
    echo "measured $name"
    sed 's/^/    /' "$RESULTS/$name.out"
  elif [[ -z "$problems" ]]; then
    echo "same     $name"
  else
    failed=1
    echo "DIFFERS  $name:$problems"
    if [[ "$measures" == yes ]]; then
      sed 's/^/    /' "$RESULTS/$name.out"
    else
      diff -u "$expected.out" "$RESULTS/$name.out" | sed 's/^/    /' || true
    fi
    diff -u "$expected_err" "$RESULTS/$name.err" | sed 's/^/    /' || true
    diff -u "$expected_logcat" "$RESULTS/$name.logcat" | sed 's/^/    /' || true
  fi
  rm -f "$expected_err" "$expected_logcat"
done
run "rm -rf $DEVICE_DIR/* $DEVICE_DIR/.[!.]*" >/dev/null
exit $failed
