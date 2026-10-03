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
# after it has its first window and the focus, and started again, before that; input-test gets the gestures
# of fixtures/apps/input-test/gestures.txt through the emulator's console once it logs "ready"; rotation-test is
# turned to landscape once it logs "ready" and back to portrait a second after its first change (ADR-0038, test 3),
# through the window manager's user rotation, which the script restores afterwards. audio-live, which plays until it
# is quit, is sent HOME 3 s after it plays and started again, and quit with BACK 2 s after it plays again; no adb
# command of the emulator disconnects its stream (killing audioserver restores the stream without an error
# callback), so its error callback has no reference here (ADR-0036, after M3). present-gles and present-vulkan run
# with service.sf.present_timestamp at 1, as on a device with reliable present fences (ADR-0041, decision 11),
# through a root adbd; a running emulator gets the property and adbd back afterwards. config-test (ADR-0039, test 3)
# is a Java app from build/config-test (tools/build-java-test-app.sh), started through the framework's
# android.app.Activity, on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at density 480: the AVD
# apk-runner-35-iphone, the same image with that display, which the script creates and starts for it alone (the
# window manager forces at most three times apk-runner-35's 320 x 480 px on a side). styled-text-test (ADR-0039
# b3.4b), package-manager-test (b3.5a) and package-manager-legacy-test (b3.5b1) are Java apps of the same kind on
# apk-runner-35's display, on which their styled strings and PackageManager's answers do not depend. The package
# manager's two run on apk-runner-35-bare (ADR-0045 test 1), the same image without sensors, GPS, modem and cameras,
# whose hardware is closer to Minato's; its image still declares apk-runner-35's system features. --features writes
# that AVD's `pm list features`, which must be fixtures/expected/system-features.txt (b3.5b1). A test app's lines of logcat with its own tag (logcat -v tag) must
# be fixtures/expected/<app>.logcat; those with the tag <app>-device differ by device and are only shown.
# The benchmark's APKs bench-gles and bench-vulkan (ADR-0037,
# step c) come from build/graphics/bench (tools/build-bench.sh) and run with the emulator's defaults, as they find
# no bench.conf: their lines are only shown, and the line of their run must count frames and no failure.
# A Java program of fixtures/java (ADR-0033 b) runs with --art as `dalvikvm64 -Xint` runs it from
# /data/local/tmp, its jar from build/art/test (tools/build-art.sh) and its JNI library, if it names one, from the
# fixtures' build, found through -Djava.library.path: its stdout must be fixtures/expected/<program>.out, and the
# last line of its stderr, which differs by device, is only shown.
# ART's run-tests (ADR-0033 test 3) run with --art-run-tests: the subset of runtime/art/run_tests.py, built into
# build/art-run-tests/jars, runs from one script on the device, each test as the guest runs it, with dalvikvm64; the
# results write fixtures/art-run-tests.txt and Android 15's own outputs in fixtures/expected/art-run-tests.
# Usage: tools/android-reference.sh [test ...]   (default: every test in fixtures/tests.txt)
#        tools/android-reference.sh --app <app>
#        tools/android-reference.sh --art <program> [<library>]
#        tools/android-reference.sh --art-run-tests
#        tools/android-reference.sh --features
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
BENCH=build/graphics/bench
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
art=""
art_library=""
art_run_tests=""
features=""
if [[ $# -eq 1 && "$1" == --art-run-tests ]]; then
  art_run_tests=yes
  fixtures=()
elif [[ $# -eq 1 && "$1" == --features ]]; then
  features=yes
  fixtures=()
elif [[ $# -eq 2 && "$1" == --app ]]; then
  app=$2
  fixtures=()
elif [[ ($# -eq 2 || $# -eq 3) && "$1" == --art ]]; then
  art=$2
  art_library=${3:-}
  fixtures=()
elif [[ $# -gt 0 ]]; then
  fixtures=("$@")
else
  fixtures=()
  for name in $(test_names); do
    fixtures+=("$name")
  done
fi

# config-test's AVD has the M4 plan's display; every other test keeps apk-runner-35's, which its reference has.
avd_display=""
if [[ "$app" == config-test ]]; then
  AVD=apk-runner-35-iphone
  avd_display=$'hw.lcd.width=1290\nhw.lcd.height=2796\nhw.lcd.density=480\n'
fi
# The package manager's tests and the system features: apk-runner-35-bare, without the hardware Minato reports as
# absent (ADR-0045 decision 4); the microphone stays.
if [[ -n "$features" || "$app" == package-manager-test || "$app" == package-manager-legacy-test ]]; then
  AVD=apk-runner-35-bare
  avd_display=""
  for key in hw.gsmModem hw.gps hw.accelerometer hw.accelerometer_uncalibrated hw.gyroscope hw.sensors.light \
      hw.sensors.pressure hw.sensors.humidity hw.sensors.proximity hw.sensors.magnetic_field \
      hw.sensors.magnetic_field_uncalibrated hw.sensors.gyroscope_uncalibrated hw.sensors.orientation \
      hw.sensors.temperature hw.sensors.rgbclight hw.sensors.heart_rate hw.sensors.wrist_tilt hw.sensors.heading; do
    avd_display+="$key=no"$'\n'
  done
  avd_display+=$'hw.camera.back=none\nhw.camera.front=none\n'
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
  printf '%s' "$avd_display" >>"$AVD_HOME/$AVD.avd/config.ini"
}

mkdir -p "$RESULTS"
"$ADB" start-server >/dev/null 2>&1
serial=$("$ADB" devices | awk '$1 ~ /^emulator-/ && $2 == "device" { print $1; exit }')
started=""
# The audio test apps (ADR-0036, step c) need the emulator's audio device; they play only zeros. Every other
# test runs without it. A running emulator started with -no-audio would give them a wrong reference.
audio=-no-audio
case "$app" in aaudio-test | opensl-test | audio-live) audio="" ;; esac
if [[ -n "$serial" && -z "$audio" && "$(ps -Ao args | grep -e '-avd ' | grep -c -e '-no-audio')" != 0 ]]; then
  echo "The running emulator has no audio device (-no-audio): stop it, and this script starts it with audio." >&2
  exit 1
fi
if [[ -n "$serial" ]]; then
  running=$("$ADB" -s "$serial" emu avd name 2>/dev/null | tr -d '\r' | head -n 1)
  if [[ "$running" != "$AVD" ]]; then
    echo "The running emulator is the AVD $running, and this test needs $AVD: stop it, and this script starts $AVD." >&2
    exit 1
  fi
fi
if [[ -z "$serial" ]]; then
  [[ -f "$AVD_HOME/$AVD.ini" ]] || create_avd
  serial=emulator-5554
  echo "Starting the emulator ($AVD) ..."
  ANDROID_HOME="$SDK" ANDROID_SDK_ROOT="$SDK" ANDROID_AVD_HOME="$AVD_HOME" "$EMULATOR" -avd "$AVD" -port 5554 -no-window ${audio:+"$audio"} \
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
# Waits up to $3 seconds (30 without it) until the app's lines with the tag $1 contain the text $2.
wait_for() {
  local seconds=${3:-30}
  for _ in $(seq 1 $((seconds * 2))); do
    app_lines "$1" | grep -q -F "$2" && return 0
    sleep 0.5
  done
  echo "$1 did not log \"$2\" within $seconds s." >&2
  return 1
}

# Waits up to 20 s until adbd, restarted by adb root or unroot, runs the shell as root ($1 = 0) or not (else).
wait_for_adbd() {
  local uid
  for _ in $(seq 1 40); do
    uid=$(run id -u 2>/dev/null | tr -d '\r') || true
    if [[ "$1" == 0 && "$uid" == 0 ]] || [[ "$1" != 0 && "$uid" =~ ^[1-9][0-9]*$ ]]; then return 0; fi
    sleep 0.5
  done
  echo "adbd did not come back within 20 s." >&2
  return 1
}

# present-test's apps run as on a device with reliable present fences, which Minato is (ADR-0041, decision 11):
# Android 15's loader offers VK_GOOGLE_display_timing only with service.sf.present_timestamp at 1
# (vulkan/libvulkan/driver.cpp, EnumerateDeviceExtensionProperties), and SurfaceFlinger sets it to 0 because the
# emulator's composer reports PRESENT_FENCE_IS_NOT_RELIABLE. Setting it takes a root adbd.
timestamp_was="" rooted=""
present_timestamp_on() {
  if [[ "$(run id -u | tr -d '\r')" != 0 ]]; then
    rooted=yes
    device root >/dev/null
    wait_for_adbd 0
  fi
  timestamp_was=$(run getprop service.sf.present_timestamp | tr -d '\r')
  run setprop service.sf.present_timestamp 1 || true
  if [[ "$(run getprop service.sf.present_timestamp | tr -d '\r')" != 1 ]]; then
    echo "Could not set service.sf.present_timestamp to 1." >&2
    return 1
  fi
}
present_timestamp_back() {
  [[ -z "$timestamp_was" ]] || run setprop service.sf.present_timestamp "$timestamp_was" || true
  if [[ -n "$rooted" ]]; then
    device unroot >/dev/null || true
    wait_for_adbd shell || true
  fi
}

run_app() {
  local apk="$APKS/$app.apk" manifest="fixtures/apps/$app/AndroidManifest.xml" build=tools/build-fixtures.sh
  local last="android_main returns" limit=30 package activity activity_class=android.app.NativeActivity mask=
  # A benchmark's APK ends after the line of its run, which gives its frames and failures.
  case "$app" in
    bench-gles | bench-vulkan)
      apk="$BENCH/$app.apk" manifest="graphics/bench/android/$app/AndroidManifest.xml" build=tools/build-bench.sh
      last=" failures"
      ;;
    # A Java app (ADR-0039) with the framework's activity, which ends its log with "done"; its own certificate's
    # SHA-256 is marked (b3.5b1).
    config-test | styled-text-test | package-manager-test | package-manager-legacy-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      mask=yes
      ;;
    # present-test's apps draw 1200 frames, at 60 Hz on the emulator also in their 120 Hz half: about 45 s.
    present-gles | present-vulkan)
      limit=120
      ;;
  esac
  if [[ ! -f "$apk" ]]; then
    echo "No $apk: run $build first." >&2
    return 1
  fi
  package=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "$manifest" | head -n 1)
  activity="$package/$activity_class"
  device uninstall "$package" >/dev/null 2>&1 || true
  device install "$apk" >/dev/null
  run logcat -c
  local rotation=""
  if [[ "$app" == rotation-test ]]; then
    # The activity leaves its orientation unspecified, so with the rotation locked it follows the user rotation.
    # "cmd window user-rotation" says "free" or "lock <rotation>", which it takes back as it is.
    rotation=$(run cmd window user-rotation | tr -d '\r')
    run cmd window user-rotation lock 0 >/dev/null
  fi
  run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
  if [[ "$app" == rotation-test ]]; then
    local turned=yes
    { wait_for "$app" ready && run cmd window user-rotation lock 1 >/dev/null &&
      wait_for "$app" "resized: window landscape" && sleep 1 && run cmd window user-rotation lock 0 >/dev/null; } ||
      turned=no
    # Word splitting is wanted: "lock 1" is two arguments.
    # shellcheck disable=SC2086
    run cmd window user-rotation $rotation >/dev/null
    [[ "$turned" == yes ]] || return 1
  fi
  if [[ "$app" == activity-test ]]; then
    wait_for "$app" APP_CMD_GAINED_FOCUS || return 1
    sleep 1
    run input keyevent KEYCODE_HOME
    wait_for "$app" APP_CMD_SAVE_STATE || return 1
    sleep 1
    run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
  fi
  if [[ "$app" == audio-live ]]; then
    wait_for "$app-device" playing || return 1
    sleep 3
    run input keyevent KEYCODE_HOME
    wait_for "$app" "pause: requestPause" || return 1
    sleep 1
    run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
    local resumed=0
    for _ in $(seq 1 60); do
      resumed=$(app_lines "$app" | grep -c "resume: requestStart" || true)
      [[ "$resumed" -lt 2 ]] || break
      sleep 0.5
    done
    [[ "$resumed" -ge 2 ]] || { echo "$app did not start again within 30 s." >&2; return 1; }
    sleep 2
    run input keyevent KEYCODE_BACK
  fi
  if [[ "$app" == input-test ]]; then
    # The gestures as reports of the emulator's multi-touch screen through its console (ABS_MT_*, 0 to 32767
    # across the screen): a new pointer takes the slot of its ID with a new tracking ID and some pressure,
    # without which the reader takes it for a hover; a lifted one ends its tracking ID; the samples of a MOVE
    # are reports in one command.
    local delay command
    wait_for "$app" ready || return 1
    awk '
      function raw(fraction) { fraction = int(fraction * 32768 + 0.5); return fraction > 32767 ? 32767 : fraction }
      function slot(pointer, parts) { split(pointer, parts, /[:,]/); return "EV_ABS:ABS_MT_SLOT:" parts[1] }
      function place(pointer, parts) {
        split(pointer, parts, /[:,]/)
        return " EV_ABS:ABS_MT_POSITION_X:" raw(parts[2]) " EV_ABS:ABS_MT_POSITION_Y:" raw(parts[3])
      }
      /^[[:space:]]*(#|$)/ { next }
      {
        action = $2; at = 0
        if (match(action, /\([0-9]+\)/)) {
          at = substr(action, RSTART + 1, RLENGTH - 2); action = substr(action, 1, RSTART - 1)
        }
        pointers_of = $0; sub(/^[[:space:]]*[^[:space:]]+[[:space:]]+[^[:space:]]+/, "", pointers_of)
        samples = split(pointers_of, sample, "|")
        split(sample[1], pointers, " ")
        out = ""
        if (action == "DOWN" || action == "POINTER_DOWN") {
          out = slot(pointers[at + 1]) " EV_ABS:ABS_MT_TRACKING_ID:" (++tracking) place(pointers[at + 1]) \
            " EV_ABS:ABS_MT_PRESSURE:512 EV_SYN:0:0"
        } else if (action == "UP" || action == "POINTER_UP") {
          out = slot(pointers[at + 1]) " EV_ABS:ABS_MT_TRACKING_ID:-1 EV_SYN:0:0"
        } else if (action == "MOVE") {
          for (s = 1; s <= samples; ++s) {
            count = split(sample[s], pointers, " ")
            for (k = 1; k <= count; ++k) out = out slot(pointers[k]) place(pointers[k]) " "
            out = out "EV_SYN:0:0 "
          }
        } else {
          print "gestures.txt:" NR ": a touchscreen cannot play " $2 > "/dev/stderr"
          exit 1
        }
        print $1 / 1000, out
      }' "fixtures/apps/$app/gestures.txt" |
      while read -r delay command; do
        sleep "$delay"
        # shellcheck disable=SC2086
        device emu event send $command >/dev/null
      done
  fi
  wait_for "$app" "$last" "$limit" || return 1
  sleep 1
  app_lines "$app" >"$RESULTS/$app.logcat"
  app_lines "$app-device" >"$RESULTS/$app-device.logcat"
  if [[ -n "$mask" ]]; then
    # make-apk signs with a per-machine debug key (ADR-0031): each certificate SHA-256 the APK's check proved becomes
    # the mark of its role that framework/tests/java_app_run.py puts in our log, so the expected file holds no
    # machine's certificate.
    swift build --package-path importer/APKKit --product apk-inspect >/dev/null
    local inspect
    inspect="$(swift build --package-path importer/APKKit --show-bin-path)/apk-inspect"
    python3 framework/tests/certificate_mask.py "$inspect" "$apk" <"$RESULTS/$app.logcat" >"$RESULTS/$app.logcat.masked"
    mv "$RESULTS/$app.logcat.masked" "$RESULTS/$app.logcat"
  fi
  if [[ "$last" == " failures" ]]; then
    # Compared with nothing: the run counts frames and no failure, and the app finished itself without a crash.
    local finished=no activities crashes
    for _ in $(seq 1 20); do
      activities=$(run dumpsys activity activities)
      if [[ "$activities" != *"$activity"* ]]; then
        finished=yes
        break
      fi
      sleep 0.5
    done
    crashes=$(run logcat -d -b crash)
    device uninstall "$package" >/dev/null 2>&1 || true
    sed 's/^/    /' "$RESULTS/$app.logcat"
    if [[ "$finished" == yes && "$crashes" != *"$package"* ]] &&
      grep -Eq '^I/[^:]+: run [^:]+: [1-9][0-9]* frames, 0 failures' "$RESULTS/$app.logcat"; then
      echo "ran      $app"
      return 0
    fi
    echo "FAILED   $app (finished by itself: $finished)"
    return 1
  fi
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

if [[ -n "$features" ]]; then
  run pm list features | tr -d '\r' | sort >"$RESULTS/system-features.txt"
  if cmp -s fixtures/expected/system-features.txt "$RESULTS/system-features.txt"; then
    echo "same     system features ($(wc -l <"$RESULTS/system-features.txt" | tr -d ' '))"
    exit 0
  fi
  echo "DIFFERS  system features"
  diff -u fixtures/expected/system-features.txt "$RESULTS/system-features.txt" | sed 's/^/    /' || true
  exit 1
fi

if [[ -n "$app" ]]; then
  if [[ "$app" == present-gles || "$app" == present-vulkan ]]; then
    # An emulator this script started stops at the end, and boots with both as they were.
    [[ -n "$started" ]] || trap present_timestamp_back EXIT
    present_timestamp_on
  fi
  run_app
  exit $?
fi

if [[ -n "$art" ]]; then
  jar=build/art/test/$art.jar
  for needed in "$jar" ${art_library:+"$FIXTURES/$art_library"}; do
    if [[ ! -f "$needed" ]]; then
      echo "No $needed: run tools/build-art.sh and tools/build-fixtures.sh first." >&2
      exit 1
    fi
  done
  run "rm -rf $DEVICE_DIR/* $DEVICE_DIR/.[!.]*" >/dev/null
  if ! device push "$jar" ${art_library:+"$FIXTURES/$art_library"} "$DEVICE_DIR/" >/dev/null 2>&1; then
    echo "Cannot copy $art to the device." >&2
    exit 1
  fi
  status=0
  run "cd $DEVICE_DIR && dalvikvm64 -Xint -Djava.library.path=$DEVICE_DIR -cp $DEVICE_DIR/$art.jar Main" \
    >"$RESULTS/$art.out" 2>"$RESULTS/$art.err" || status=$?
  run "rm -rf $DEVICE_DIR/* $DEVICE_DIR/.[!.]*" >/dev/null
  if [[ $status -eq 0 ]] && cmp -s "fixtures/expected/$art.out" "$RESULTS/$art.out"; then
    echo "same     $art"
    tail -n 1 "$RESULTS/$art.err" | sed 's/^/    /'
    exit 0
  fi
  echo "DIFFERS  $art: exit status $status"
  diff -u "fixtures/expected/$art.out" "$RESULTS/$art.out" | sed 's/^/    /' || true
  tail -n 20 "$RESULTS/$art.err" | sed 's/^/    /'
  exit 1
fi

if [[ -n "$art_run_tests" ]]; then
  jdk=$(find build/tools/jdk-17 -mindepth 3 -maxdepth 3 -name Home -path '*/Contents/Home' 2>/dev/null | sort | tail -n 1)
  if [[ -z "$jdk" || ! -f build/art/java/all-classes.jar ]]; then
    echo "No JDK 17 in build/tools/jdk-17 or no boot classes in build/art/java: run tools/build-art.sh first." >&2
    exit 1
  fi
  jars=build/art-run-tests/jars
  /usr/bin/python3 runtime/art/run_tests.py jars "$jdk" build/sources/r8/r8.jar build/art/java build/sources "$jars" \
    --jobs 6
  results="$RESULTS/art-run-tests"
  rm -rf "$results"
  mkdir -p "$results"
  /usr/bin/python3 runtime/art/run_tests.py device-script build/sources "$DEVICE_DIR/art-run-tests" >"$results/run.sh"
  run "rm -rf $DEVICE_DIR/art-run-tests $DEVICE_DIR/run-test" >/dev/null
  if ! device push "$jars" "$DEVICE_DIR/art-run-tests/jars" >/dev/null 2>&1 ||
    ! device push "$results/run.sh" "$DEVICE_DIR/art-run-tests/run.sh" >/dev/null 2>&1; then
    echo "Cannot copy the run-tests to the device." >&2
    exit 1
  fi
  echo "Running $(grep -c '^ANDROID_DATA=' "$results/run.sh") run-tests on the device ..."
  run "sh $DEVICE_DIR/art-run-tests/run.sh" >"$results/run.log" 2>&1 || true
  device pull "$DEVICE_DIR/art-run-tests/results" "$results/" >/dev/null
  run "rm -rf $DEVICE_DIR/art-run-tests $DEVICE_DIR/run-test" >/dev/null
  /usr/bin/python3 runtime/art/run_tests.py reference build/sources "$results/results" fixtures/art-run-tests.txt \
    fixtures/expected/art-run-tests
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
