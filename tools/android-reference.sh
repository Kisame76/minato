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
# after it has its first window and the focus, and started again, before that; window-formats-test (ADR-0043 b5, step
# f) is sent HOME a second after it shows RGB_565, and started again a second after its window goes; input-test gets
# the gestures of fixtures/apps/input-test/gestures.txt through the emulator's console once it logs "ready";
# rotation-test is turned to landscape once it logs "ready" and back to portrait a second after its first change
# (ADR-0038, test 3),
# through the window manager's user rotation, which the script restores afterwards. audio-live, which plays until it
# is quit, is sent HOME 3 s after it plays and started again, and quit with BACK 2 s after it plays again; no adb
# command of the emulator disconnects its stream (killing audioserver restores the stream without an error
# callback), so its error callback has no reference here (ADR-0036, after M3). present-gles and present-vulkan run
# with service.sf.present_timestamp at 1, as on a device with reliable present fences (ADR-0041, decision 11),
# through a root adbd; a running emulator gets the property and adbd back afterwards. config-test (ADR-0039, test 3)
# is a Java app from build/config-test (tools/build-java-test-app.sh), started through the framework's
# android.app.Activity, on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at density 480: the AVD
# apk-runner-35-iphone, the same image with that display, which the script creates and starts for it and
# view-tree-test (step c2c2) and animator-test (ADR-0041 a)) alone (the window manager forces at most three times
# apk-runner-35's 320 x 480 px on a side). text-test (ADR-0043 step d)) runs on activity-start-test's AVD with the
# emulator's input methods disabled, as Minato has none yet, and every exit enables them again. styled-text-test
# (ADR-0039 b3.4b), package-manager-test (b3.5a) and package-manager-legacy-test (b3.5b1) are Java apps of the same
# kind on apk-runner-35's display, on which their styled strings and PackageManager's answers do not depend. The package
# manager's two run on apk-runner-35-bare (ADR-0045 test 1), the same image without sensors, GPS, modem and cameras,
# whose hardware is closer to Minato's; its image still declares apk-runner-35's system features. services-test
# (ADR-0045, the start's local services), whose sensor service has no sensor on either, and input-service-test
# (ADR-0045, the input service) run there too. --features writes
# that AVD's `pm list features`, which must be fixtures/expected/system-features.txt (b3.5b1). context-test (ADR-0039
# step c1) is one more on apk-runner-35, stopped after its first "done" and started again, so that its second start
# reads what the first one wrote (java-test, ADR-0043 step c1, starts twice in the same way on activity-start-test's
# AVD, each start sent HOME and started again before the app finishes itself); looper-test (step c1), provider-test
# (step c2c1), app-calls-test (step c2d), vsync-test and gles-java-test (ADR-0041 a)), asset-ndk-test (ADR-0041
# decision 7), compat-test and its twin for targetSdk 35, compat-test-35 (step c2e), jni-lib-test (ADR-0039 step
# c5.1) and owner-rules-test (ADR-0039 decision 3, the owner rules for changes) are ones too, and app-start-test
# (test 1), whose lines are System.out's that start with "app-start-test: ". A test
# app's lines of logcat with its own tag (logcat -v tag) must be fixtures/expected/<app>.logcat; those with the tag
# <app>-device differ by device and are only shown.
# jnigraphics-test (ADR-0041 a5), whose JNI library links libjnigraphics, is one of those Java apps too, and so is
# obb-test (ADR-0049 test 8), whose OBB the script pushes into /sdcard/Android/obb/<package>/ before the start, where a
# user or a store puts one, and removes after the run.
# binder-test (ADR-0043 step d2b)), whose binders and ResultReceivers are the app's own, is one of those Java apps
# too.
# strictmode-test (ADR-0039 step c2d3) is one too.
# api-breadth-test (the compatibility plan's entry 22) is one too, and so is widgets-test (gap 5), whose widgets are
# laid out in px on the M4 plan's display.
# breadth-test (the framework breadth's behavior test) is one too.
# webview-camera-test (the Unity test APK probe's batch C) is one too, on apk-runner-35-bare, a device without cameras like
# Minato; its lines of webview-camera-test-device, which the AVD's WebView answers, are only shown.
# packs-test and its twin packs-test-own (ADR-0050 test 2) are ones too, without pack slices, as Android gives
# local_testing_dir only from a manifest. pad-test (ADR-0050 test 3), built with Google's Play libraries by
# tools/build-pad-test.sh, is a Java app with its own activity: bundletool's install-apks puts its local-testing set on
# apk-runner-35, the base with local_testing_dir and the packs' slices in local_testing of its external files folder.
# broadcast-test (ADR-0045 decision 5), whose activity sends its own broadcasts to its own receivers, is one too.
# crash-test (ADR-0039, the main thread's uncaught exception), whose own handler logs the exception that ends it, is
# one too.
# sqlite-test (the compatibility plan's gap 7), whose activity makes, changes and deletes its own SQLite database, is
# one too.
# timezone-test (the compatibility plan's gap 21), whose activity reads the time zone data, is one too.
# package-installer-test (the Unity test APK resolve list, batch E), whose activity asks the package installer for sessions
# and a new one, and reads the exit reasons, is one too.
# media-absent-test (the Unity test APK gap sort, category 2), whose activity asks android.media's DRM, codec, retriever and
# extractor classes, is one too; its lines of media-absent-test-device are only shown.
# drawables-test (ADR-0043 step d)), whose Button and CheckBox are sized in dp, is one of those Java apps too, on
# the M4 plan's display, and so is checkbox-test (ADR-0043 step e1)), whose CheckBoxes are laid out in px there.
# pointer-insets-test (ADR-0042 b, gaps 6 and 9) is one too, on that display, as its loaded icon's hotspot is in dp.
# palette-test (ADR-0043 step f)) is one too, on apk-runner-35: its system colors are those of the overlays that
# SystemUI makes from the image's default wallpaper (Monet).
# settings-test (ADR-0045, gap 13) is such an app too, whose values are the image's while no test writes settings:
# its reference is taken on apk-runner-35 wiped first. touch-test (ADR-0045 step 7b) is one too, whose events are made
# up, so that its lines do not depend on the device; since step 7b2 it has its own activity, on the display below.
# key-test (ADR-0045 step 7c) is one too, whose keys it injects into its own window, on that display.
# canvas-test (ADR-0043 test 2) is a Java app of the same kind on apk-runner-35, debuggable: the script takes the raw
# pixels it writes into its cache folder with run-as, gzipped, which must be fixtures/expected/canvas-test.rgba.gz.
# views-test (ADR-0043 test 1, step c3) runs on java-test's AVD, drawn in software with the system bars shown: a second
# after its "done", screencap takes the screen, whose rows between the bars' insets that the app logs under
# views-test-device must be fixtures/expected/views-test.rgba.gz, with the screen's size and the insets in
# fixtures/expected/views-test.rows (framework/tests/views_compare.py --crop): SystemUI draws over the rest.
# The benchmark's APKs bench-gles and bench-vulkan (ADR-0037,
# step c) come from build/graphics/bench (tools/build-bench.sh) and run with the emulator's defaults, as they find
# no bench.conf: their lines are only shown, and the line of their run must count frames and no failure. With
# BENCH_CONF they find its lines as their bench.conf (ADR-0066 step 4: an engine cap, cap=30 say).
# Test 2's Godot APKs godot-bench-vulkan and godot-bench-gles (ADR-0042 a) come from build/godot
# (tools/build-godot.sh) and run with the emulator's defaults too. Godot logs under its own tag, godot: bench.gd's
# lines that begin "godot-bench:" must be fixtures/expected/<app>.logcat, the rest are only shown, and the line of
# their run must count frames and no failure, after which the app has finished by itself. Once the bench has
# started, the script taps the screen's middle five times, 1 s apart, and bench.gd logs the first touch that reaches
# it, in percent of its viewport (Godot's input, ADR-0042 b)).
# A Java program of fixtures/java (ADR-0033 b) runs with --art as `dalvikvm64 -Xint` runs it from
# /data/local/tmp, its jar from build/art/test (tools/build-art.sh) and its JNI library, if it names one, from the
# fixtures' build, found through -Djava.library.path: its stdout must be fixtures/expected/<program>.out, and the
# last line of its stderr, which differs by device, is only shown. The program's lines of logcat from
# libnativeloader that name a namespace, where it loads a JNI library and which namespace it configures for a class
# loader, must be fixtures/expected/<program>.nativeloader where that file exists (ADR-0040 test 4).
# ART's run-tests (ADR-0033 test 3) run with --art-run-tests: the subset of runtime/art/run_tests.py, built into
# build/art-run-tests/jars, runs from one script on the device, each test as the guest runs it, with dalvikvm64; the
# results write fixtures/art-run-tests.txt and Android 15's own outputs in fixtures/expected/art-run-tests.
# Usage: tools/android-reference.sh [test ...]   (default: every test in fixtures/tests.txt)
#        tools/android-reference.sh --app <app>   (its APK built first: one older than its sources is refused)
#        tools/android-reference.sh --art <program> [<library>]
#        tools/android-reference.sh --art-run-tests
#        tools/android-reference.sh --features
# Results: build/android-reference/<test>.{out,err,logcat}, <app>.logcat and <app>-device.logcat,
#          <program>.nativeloader
# Optional environment:
#   ANDROID_HOME      SDK with emulator, platform-tools and the system image, default: ~/Library/Android/sdk
#   ANDROID_AVD_HOME  where the AVD lives, default: ~/.android/avd
#   FIXTURES_BUILD    the fixtures' build directory (tools/build-fixtures.sh), default: build/fixtures
#   ART_TEST_JARS     the Java programs' jars for --art, default: build/art/test
#   BENCH_BUILD       the bench's build directory (tools/build-bench.sh), default: build/graphics/bench
#   BENCH_CONF        bench.conf's lines for bench-gles and bench-vulkan, written into their internalDataPath
#   MINATO_DISPLAY    <width>x<height>@<scale> for an app whose AVD has the M4 plan's display (tools/check-display.sh):
#                     its AVD at that display, apk-runner-35-iphone-<width>x<height> or ...-gesture-<width>x<height>,
#                     results in build/android-reference/<width>x<height>, compared with <app>@<width>x<height>.*
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/check-display.sh

SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
AVD_HOME="${ANDROID_AVD_HOME:-$HOME/.android/avd}"
ADB="$SDK/platform-tools/adb"
EMULATOR="$SDK/emulator/emulator"
AVD=apk-runner-35
IMAGE=system-images/android-35/default/arm64-v8a/
FIXTURES="${FIXTURES_BUILD:-build/fixtures}/bin"
APKS="${FIXTURES_BUILD:-build/fixtures}/apks"
BENCH="${BENCH_BUILD:-build/graphics/bench}"
RESULTS=build/android-reference
DEVICE_DIR=/data/local/tmp

for tool in "$ADB" "$EMULATOR" "$SDK/$IMAGE/system.img"; do
  if [[ ! -e "$tool" ]]; then
    echo "Missing $tool: install platform-tools, the emulator and $IMAGE into $SDK." >&2
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
  # The script builds no APK: a test app's APK older than a file of its sources would put old code on the device and
  # take the reference from it, so it stops before the emulator starts.
  # Only the source folders the app has, and find stops at the first newer file itself: under set -euo pipefail a
  # missing folder or a pipe into head would end the script without a word.
  sources=()
  for folder in "fixtures/java/$app" "fixtures/apps/$app"; do
    [[ ! -d "$folder" ]] || sources+=("$folder")
  done
  if [[ -z "${APK_SET:-}" && -f "build/$app/$app.apk" && ${#sources[@]} -gt 0 ]]; then
    newer=$(find "${sources[@]}" -type f -newer "build/$app/$app.apk" -print -quit)
    if [[ -n "$newer" ]]; then
      echo "build/$app/$app.apk is older than $newer: build it again first." >&2
      exit 1
    fi
  fi
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

# config-test's AVD has the M4 plan's display, and view-tree-test's, animator-test's, drawables-test's,
# checkbox-test's, pointer-insets-test's and widgets-test's, whose sizes in dp they turn into px at that density; every
# other test keeps apk-runner-35's, which its reference has.
avd_display=""
if [[ "$app" == config-test || "$app" == view-tree-test || "$app" == animator-test ||
  "$app" == drawables-test || "$app" == checkbox-test || "$app" == pointer-insets-test ||
  "$app" == widgets-test ]]; then
  AVD=apk-runner-35-iphone
  avd_display=$'hw.lcd.width=1290\nhw.lcd.height=2796\nhw.lcd.density=480\n'
fi
# activity-start-test's AVD (ADR-0039 step c2c3) has that display without navigation keys, as an iPhone has none, and
# gesture navigation, as our window manager answers that a navigation bar exists (step c2c2). start-stops-test
# (ADR-0042 b), whose window's insets controller depends on the bars, runs there too, java-test (ADR-0043 step c1),
# whose lifecycle the host drives as Minato's does, views-test (step c3), whose screen nothing but the bars covers
# there, text-test (step d)), which also waits for its window's focus, and surface-test (step c4), whose GLSurfaceView
# fills the window between the bars. touch-test (ADR-0045 step 7b2) runs there too: the script taps its window's
# middle and swipes through it with adb shell input, as apk-run's touch steps do in tools/touch-check.sh.
# activity-flow-test (ADR-0039 step b3.5b2-1), whose steps wait for its windows' focus, runs there too, and so does
# key-test (ADR-0045 step 7c), which injects its keys once its window has focus, gesture-flow-test
# (step b3.5b2-1b), whose swipe and tap the script plays as touch-test's, and result-flow-test (step b3.5b2-2a)
# and launch-mode-test (step b3.5b2-2b), whose steps wait for their windows' focus as activity-flow-test's.
# texture-view-test (ADR-0058 d)) runs there too: its two TextureViews' bounds in the window are Minato's there, and so
# does second-windows-test (ADR-0041 gap 4 (b)), whose dialogs' and popups' sizes in px are Minato's at that density,
# and toast-test (gap 4 (b4)), whose toast's size and place are, windows-return-test (gap 4 (b5a)), whose dialog's
# size is, and title-test, whose dialogs' sizes are.
if [[ "$app" == activity-start-test || "$app" == start-stops-test || "$app" == java-test ||
  "$app" == views-test* || "$app" == text-test || "$app" == surface-test || "$app" == touch-test ||
  "$app" == activity-flow-test || "$app" == key-test || "$app" == gesture-flow-test ||
  "$app" == result-flow-test || "$app" == launch-mode-test || "$app" == texture-view-test ||
  "$app" == second-windows-test || "$app" == toast-test || "$app" == windows-return-test ||
  "$app" == title-test || "$app" == ime-test || "$app" == ime-insets-test || "$app" == ime-insets-test-34 ||
  "$app" == ime-hide-test || "$app" == ime-hide-test-cb || "$app" == toolbar-test ||
  "$app" == action-bar-test || "$app" == action-bar-test-34 || "$app" == action-bar-menu-test ||
  "$app" == action-mode-standalone-test || "$app" == trim-memory-test || "$app" == selection-mode-test ||
  "$app" == insertion-mode-test || "$app" == menu-pause-test || "$app" == turn-test ||
  "$app" == orientation-request-test ||
  "$app" == ime-fallback-test || "$app" == enter-test || "$app" == enter-test-15 || "$app" == ime-extract-test ||
  "$app" == native-ime-test || "$app" == native-jni-ime-test || "$app" == back-test ||
  "$app" == back-test-callback ]]; then
  AVD=apk-runner-35-iphone-gesture
  avd_display=$'hw.lcd.width=1290\nhw.lcd.height=2796\nhw.lcd.density=480\nhw.mainKeys=no\n'
fi
# The package manager's tests and the system features: apk-runner-35-bare, without the hardware Minato reports as
# absent (ADR-0045 decision 4); the microphone stays.
if [[ -n "$features" || "$app" == package-manager-test || "$app" == package-manager-legacy-test \
    || "$app" == services-test || "$app" == input-service-test || "$app" == webview-camera-test ]]; then
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

# Another display (the device-size plan, step 1): an AVD of its own for an app whose AVD has the M4 plan's display;
# the others' references are the same at every display.
if [[ "$CHECK_DISPLAY" != "$CHECK_DEFAULT_DISPLAY" ]]; then
  if [[ "$avd_display" != *hw.lcd.width=1290* ]]; then
    echo "${app:-this run}'s AVD has no display of the M4 plan's, so its reference is the same at every display." >&2
    exit 1
  fi
  AVD="$AVD-${CHECK_WIDTH}x${CHECK_HEIGHT}"
  avd_display=${avd_display/hw.lcd.width=1290/hw.lcd.width=$CHECK_WIDTH}
  avd_display=${avd_display/hw.lcd.height=2796/hw.lcd.height=$CHECK_HEIGHT}
  avd_display=${avd_display/hw.lcd.density=480/hw.lcd.density=$CHECK_DENSITY}
  RESULTS="$RESULTS/${CHECK_WIDTH}x${CHECK_HEIGHT}"
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
case "$app" in aaudio-test | opensl-test | audio-live | sdl-audio-test) audio="" ;; esac
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
emulator_stop() {
  "$ADB" -s "$serial" shell sync >/dev/null 2>&1
  "$ADB" -s "$serial" emu kill >/dev/null 2>&1 || kill "$started" 2>/dev/null || true
}
if [[ -z "$serial" ]]; then
  [[ -f "$AVD_HOME/$AVD.ini" ]] || create_avd
  serial=emulator-5554
  echo "Starting the emulator ($AVD) ..."
  ANDROID_HOME="$SDK" ANDROID_SDK_ROOT="$SDK" ANDROID_AVD_HOME="$AVD_HOME" "$EMULATOR" -avd "$AVD" -port 5554 -no-window ${audio:+"$audio"} \
    -no-boot-anim -no-snapshot -gpu swiftshader_indirect >"$RESULTS/emulator.log" 2>&1 &
  started=$!
  trap emulator_stop EXIT
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
# The emulator makes a display's width even (the iPhone Duo's open 2007 x 2853 starts as 2008 x 2853): the window
# manager's override gives the display its size, which the AVD keeps.
if [[ "$CHECK_DISPLAY" != "$CHECK_DEFAULT_DISPLAY" ]]; then
  physical=$(run wm size | tr -d '\r' | sed -n -E 's/^Physical size: ([0-9]+x[0-9]+)$/\1/p')
  if [[ "$physical" != "${CHECK_WIDTH}x${CHECK_HEIGHT}" ]]; then
    run wm size "${CHECK_WIDTH}x${CHECK_HEIGHT}"
    sleep 2
  fi
fi

# Messages to the device's logger: in the runtime they go to stderr in logcat's "tag" format, on
# Android to logcat. An expected .err file therefore splits into logcat lines and stderr lines.
logcat_line='^[VDIWEFS]/[^:]+: '

# The app's lines of logcat with the tag $1.
app_lines() { run logcat -d -v tag -s "$1:V" | grep -v '^--------- ' || true; }
# Waits up to $4 seconds (30 without it) until the app's lines with the tag $1 contain the text $2 at least $3 times.
wait_count() {
  local seconds=${4:-30} count=0
  for _ in $(seq 1 $((seconds * 2))); do
    count=$(app_lines "$1" | grep -c -F "$2" || true)
    [[ "$count" -lt "$3" ]] || return 0
    sleep 0.5
  done
  echo "$1 did not log \"$2\" $3 times within $seconds s (got $count)." >&2
  return 1
}
# Waits up to $3 seconds (30 without it) until the app's lines with the tag $1 contain the text $2.
wait_for() {
  local seconds=${3:-30}
  for _ in $(seq 1 $((seconds * 2))); do
    app_lines "$1" | grep -q -F "$2" && return 0
    sleep 0.5
  done
  echo "$1 did not log \"$2\" within $seconds s." >&2
  run logcat -d -v tag | grep -F "$1" | tail -n 12 | sed 's/^/  /' >&2
  return 1
}

# BENCH_CONF's lines as bench.conf in the installed bench's internalDataPath, /data/user/0/<package>/files ($1), with
# the app's owner and label, written through a root adbd, as the App writes it into a session's data (ADR-0066 step 4).
bench_conf_in() {
  local files="/data/user/0/$1/files" rooted_here="" owner
  if [[ "$(run id -u | tr -d '\r')" != 0 ]]; then
    rooted_here=yes
    device root >/dev/null
    wait_for_adbd 0
  fi
  printf '%s\n' "$BENCH_CONF" >"$RESULTS/bench.conf"
  device push "$RESULTS/bench.conf" /data/local/tmp/bench.conf >/dev/null
  owner=$(run stat -c %u:%g "/data/user/0/$1" | tr -d '\r')
  run mkdir -p "$files"
  run mv /data/local/tmp/bench.conf "$files/bench.conf"
  run chown -R "$owner" "$files"
  run restorecon -R "$files"
  if [[ -n "$rooted_here" ]]; then
    device unroot >/dev/null
    wait_for_adbd shell
  fi
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

# ringtone-test (ADR-0060 e2e1) runs as on a device without sounds set, as Minato is: the settings' ringtone,
# notification sound and alarm are deleted and their cached copies (SettingsProvider's, in system_de) moved aside for
# the run, which takes a root adbd; every exit puts both back.
sounds_was="" sounds_rooted=""
SOUNDS="ringtone notification_sound alarm_alert"
SOUNDS_CACHE=/data/system_de/0/ringtones
sounds_off() {
  if [[ "$(run id -u | tr -d '\r')" != 0 ]]; then
    sounds_rooted=yes
    device root >/dev/null
    wait_for_adbd 0
  fi
  local name
  sounds_was=""
  for name in $SOUNDS; do
    sounds_was="$sounds_was $name=$(run settings get system "$name" | tr -d '\r')"
    run settings delete system "$name" >/dev/null
    run "[ ! -e $SOUNDS_CACHE/${name}_cache ] || mv $SOUNDS_CACHE/${name}_cache $SOUNDS_CACHE/${name}_cache.saved"
  done
}
# network-test's network: the emulator's Wi-Fi alone, its mobile data off until the exit (mobile_data_back), and 10 s
# for the default network to move to Wi-Fi.
mobile_data_off() {
  run svc wifi enable >/dev/null || true
  run svc data disable >/dev/null
  sleep 10
}

mobile_data_back() {
  run svc data enable >/dev/null || true
}
# cellular-network-test's network: the emulator's cellular network alone, its Wi-Fi off until the exit (wifi_back), and
# 10 s for the default network to move to it.
wifi_off() {
  run svc data enable >/dev/null || true
  run svc wifi disable >/dev/null
  sleep 10
}
wifi_back() {
  run svc wifi enable >/dev/null || true
}

sounds_back() {
  local entry name value cache
  for entry in $sounds_was; do
    name=${entry%%=*} value=${entry#*=}
    if [[ -z "$value" || "$value" == null ]]; then
      run settings delete system "$name" >/dev/null || true
    else
      run settings put system "$name" "$value" || true
    fi
    cache="$SOUNDS_CACHE/${name}_cache"
    run "[ ! -e $cache.saved ] || mv $cache.saved $cache" || true
  done
  if [[ -n "$sounds_rooted" ]]; then
    device unroot >/dev/null || true
    wait_for_adbd shell || true
  fi
}

# text-test runs on Android 15 without an input method, as Minato has none until the keyboard step (ADR-0043 step d),
# decision 5). Disabling the input methods is not enough, as the input method service enables a default one again
# when none is left, so the packages that hold them are disabled for the run. Both are settings, which outlive the
# emulator: every exit enables the packages and the input methods again and sets the default one back, before an
# emulator this script started stops. A run killed between the two leaves them off, and the next one would find none
# to give back: then it gives back the image's own, LatinIME, the AOSP image's only input method.
input_methods="" input_method_packages="" default_input_method=""
STOCK_INPUT_METHOD=com.android.inputmethod.latin/.LatinIME
input_methods_off() {
  input_methods=$(run ime list -s | tr -d '\r')
  default_input_method=$(run settings get secure default_input_method | tr -d '\r')
  input_method_packages=$(run ime list -a -s | tr -d '\r' | cut -d/ -f1 | sort -u)
  if [[ -z "$input_methods" ]]; then
    echo "No input method on for $app: an earlier run left them off, and $STOCK_INPUT_METHOD comes back afterwards"
    input_methods=$STOCK_INPUT_METHOD default_input_method=$STOCK_INPUT_METHOD
    input_method_packages=${input_method_packages:-${STOCK_INPUT_METHOD%%/*}}
  fi
  echo "Input methods off for $app: $input_methods (default $default_input_method), packages" $input_method_packages
  for package in $input_method_packages; do
    run pm disable-user --user 0 "$package" >/dev/null
  done
  # The service drops a disabled package's input methods when the package change reaches it.
  for _ in $(seq 1 20); do
    [[ -n "$(run ime list -a -s | tr -d '\r')" ]] || return 0
    sleep 0.5
  done
  echo "An input method stays." >&2
  return 1
}
input_methods_back() {
  for package in $input_method_packages; do
    run pm default-state --user 0 "$package" >/dev/null 2>&1 || true
  done
  for method in $input_methods; do
    run ime enable "$method" >/dev/null 2>&1 || true
  done
  if [[ -n "$default_input_method" && "$default_input_method" != null ]]; then
    run ime set "$default_input_method" >/dev/null 2>&1 || true
  fi
  echo "Input methods back: $(run ime list -s | tr -d '\r' | tr '\n' ' ')(default" \
    "$(run settings get secure default_input_method | tr -d '\r')); disabled packages:" \
    "$(run pm list packages -d | tr -d '\r' | sed 's/^package://' | tr '\n' ' ')"
}

ime_test_off() {
  run ime disable io.github.kisame76.minato.test.ime/.FixtureInputMethod >/dev/null 2>&1 || true
  run ime disable io.github.kisame76.minato.test.imeinsets/.FixtureInputMethod >/dev/null 2>&1 || true
  run ime disable io.github.kisame76.minato.test.imefallback/.FixtureInputMethod >/dev/null 2>&1 || true
  run ime disable io.github.kisame76.minato.test.nativeime/.FixtureInputMethod >/dev/null 2>&1 || true
  run ime disable io.github.kisame76.minato.test.nativejniime/.FixtureInputMethod >/dev/null 2>&1 || true
  run ime disable io.github.kisame76.minato.test.enter/.FixtureInputMethod >/dev/null 2>&1 || true
  run ime disable io.github.kisame76.minato.test.imeextract/.FixtureInputMethod >/dev/null 2>&1 || true
  # enter-test-15's input method once stayed the default after a run that failed: the app goes, and its method with it.
  device uninstall io.github.kisame76.minato.test.enter >/dev/null 2>&1 || true
}

# Test 2's Godot APKs (ADR-0042 a): their lines under Godot's tag, split into bench.gd's "godot-bench:" lines, which
# are compared, and the rest, which are only shown; the run's line and the app's own end, as for bench-gles.
run_godot() {
  local apk="build/godot/$app.apk" package="io.github.kisame76.minato.godotbench.${app#godot-bench-}"
  local launcher="$package/com.godot.game.GodotAppLauncher" finished=no crashes tag='^[A-Z]/godot *: '
  # Godot's template keeps its activity unexported and gives the launcher this alias. logcat -v tag pads the tag to 8
  # characters.
  if [[ ! -f "$apk" ]]; then
    echo "No $apk: run tools/build-godot.sh first." >&2
    return 1
  fi
  device uninstall "$package" >/dev/null 2>&1 || true
  device install "$apk" >/dev/null
  run logcat -c
  # Android shows its full-screen confirmation over an app that goes immersive, as Godot does, the first time, and
  # the confirmation takes the taps below: confirmed for the run, as by a user who dismissed it once, then restored.
  local confirmations
  confirmations=$(run settings get secure immersive_mode_confirmations | tr -d '\r')
  run settings put secure immersive_mode_confirmations confirmed
  run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$launcher" >/dev/null
  # Godot's input (ADR-0042 b)): taps in the screen's middle once the bench runs, of which bench.gd logs the first.
  # The middle of the display as it is turned now (dumpsys window's cur=), since Godot turns it to landscape and
  # input tap takes the turned display's coordinates; wm size gives the unturned one.
  if wait_for godot "godot-bench: start " 180; then
    local size
    size=$(run dumpsys window displays | tr -d '\r' |
      awk '!found && match($0, /cur=[0-9]+x[0-9]+/) { print substr($0, RSTART + 4, RLENGTH - 4); found = 1 }')
    for _ in 1 2 3 4 5; do
      run input tap $(( ${size%x*} / 2 )) $(( ${size#*x} / 2 ))
      sleep 1
    done
  fi
  if [[ -z "$confirmations" || "$confirmations" == null ]]; then
    run settings delete secure immersive_mode_confirmations >/dev/null
  else
    run settings put secure immersive_mode_confirmations "$confirmations"
  fi
  # The emulator draws on the CPU: the engine's start, its shader compiles and 10 s of frames. An app that hangs is
  # shown and removed, so that it does not draw beside the next one.
  if ! wait_for godot " failures" 180; then
    app_lines godot | sed 's/^/    /'
    device uninstall "$package" >/dev/null 2>&1 || true
    echo "FAILED   $app (no run line)"
    return 1
  fi
  sleep 1
  app_lines godot | grep -E "${tag}godot-bench: " >"$RESULTS/$app.logcat" || true
  app_lines godot | grep -v -E "${tag}godot-bench: " >"$RESULTS/$app-device.logcat" || true
  # Godot quits by ending its process ("Force quitting Godot instance"), and the window manager keeps the alias in its
  # orientation history, so the process is the sign.
  for _ in $(seq 1 20); do
    if [[ -z "$(run pidof "$package" | tr -d '\r' || true)" ]]; then
      finished=yes
      break
    fi
    sleep 0.5
  done
  crashes=$(run logcat -d -b crash)
  device uninstall "$package" >/dev/null 2>&1 || true
  sed 's/^/    /' "$RESULTS/$app-device.logcat"
  if [[ "$finished" != yes || "$crashes" == *"$package"* ]] ||
    ! grep -Eq "${tag}run [^:]+: [1-9][0-9]* frames, 0 failures" "$RESULTS/$app-device.logcat"; then
    echo "FAILED   $app (finished by itself: $finished)"
    return 1
  fi
  if cmp -s "fixtures/expected/$app.logcat" "$RESULTS/$app.logcat"; then
    echo "same     $app"
  else
    echo "DIFFERS  $app"
    diff -u "fixtures/expected/$app.logcat" "$RESULTS/$app.logcat" | sed 's/^/    /' || true
    return 1
  fi
}

run_app() {
  [[ "$app" != godot-bench-* ]] || { run_godot; return; }
  local apk="$APKS/$app.apk" manifest="fixtures/apps/$app/AndroidManifest.xml" build=tools/build-fixtures.sh
  local last="android_main returns" limit=30 package activity activity_class=android.app.NativeActivity mask=
  local tag="$app"
  # A tag whose lines are the app's too (a WebView's console messages, under chromium, without its other lines);
  # empty: none.
  local also_tag=
  # A benchmark's APK ends after the line of its run, which gives its frames and failures.
  case "$app" in
    bench-gles | bench-vulkan)
      apk="$BENCH/$app.apk" manifest="graphics/bench/android/$app/AndroidManifest.xml" build=tools/build-bench.sh
      last=" failures"
      ;;
    # A Java app (ADR-0039) with the framework's activity, which ends its log with "done"; its own certificate's
    # SHA-256 is marked (b3.5b1).
    config-test | styled-text-test | package-manager-test | package-manager-legacy-test | context-test | looper-test \
      | provider-test | view-tree-test | app-calls-test | vsync-test | animator-test | services-test | compat-test \
      | compat-test-35 | gles-java-test | input-service-test | asset-ndk-test | parcel-test | jni-lib-test \
      | owner-rules-test | settings-test | jnigraphics-test | drawables-test | checkbox-test | pointer-insets-test \
      | obb-test | palette-test | binder-test | strictmode-test | packs-test | packs-test-own | api-breadth-test \
      | contexts-test | traffic-test | widgets-test | breadth-test | priorities-test | json-test \
      | jobs-test | hardware-buffer-test | telephony-test | audiotrack-test | sdl-audiotrack-test | jni-fields-test \
      | soundpool-test | soundpool-codec-test \
      | storage-test | storage-space-test | location-test | gapscan-test | webview-camera-test | keystore-test \
      | sms-test | accounts-test \
      | pendingintent-test | alarm-test | view-fields-test | mime-test | cxx-binding-test | media-fields-test | media-format-test \
      | main-looper-test | reflect-test | sdk-types-test | ui-types-test | surface-texture-test | html-test | intent-query-test \
      | preferences-test | service-exceptions-test | network-security-test | network-security-config-test \
      | noted-app-op-test | app-services-test | image-reader-test | autocomplete-test | bound-services-test | package-answers-test \
      | image-writer-test | media-codec-test | media-codec-audio-test | media-codec-decoders-test | open-test \
      | sensors-java-test \
      | location-manager-test \
      | gms-absent-test \
      | location-manager-test-denied | location-manager-test-fine | location-manager-test-coarse \
      | device-profile-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      mask=yes
      ;;
    media-extractor-test)
      # Both files through the NDK's and Java's sources take about a minute on the emulator.
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      mask=yes limit=120
      ;;
    # media-codec-opus-test and media-extractor-ogg-test (ADR-0060 e2): about thirty Opus decodes each, surround ones
    # among them.
    media-codec-opus-test | media-extractor-ogg-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      mask=yes limit=180
      ;;
    # camera-characteristics-test and its twin camera-characteristics-test-granted (ADR-0073), from the same sources:
    # the twin gets CAMERA before the start and opens each camera; camera-legacy-test and camera-preview-test (step e)
    # get CAMERA too.
    camera-characteristics-test | camera-characteristics-test-granted | camera-legacy-test | camera-preview-test \
      | camera-picture-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      limit=120
      ;;
    # media-player-test (ADR-0060 e2e) and its twin without the INTERNET permission, from the same sources: a playback
    # and the http sources' waits take up to three minutes.
    media-player-test | media-player-test-offline | media-player-ogg-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      limit=240
      ;;
    # ringtone-test (ADR-0060 e2e1): RingtoneManager, Ringtone and MediaPlayer with the default sounds, without sounds
    # set (sounds_off).
    ringtone-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      limit=60
      ;;
    permission-request-test | permission-request-test-fine | permission-request-test-camera |\
    permission-request-test-notifications)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done
      activity_class=io.github.kisame76.minato.test.permreq.PermissionRequestActivity
      mask=yes
      ;;
    # visualizer-test and its twin visualizer-test-granted (the audio effects plan), from the same sources: the twin gets
    # RECORD_AUDIO before the start.
    visualizer-test | visualizer-test-granted)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      limit=60
      ;;
    # audiofx-test (the audio effects plan): the effects and their parameters on a track's session.
    audiofx-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      limit=60
      ;;
    # notify-test and its twin notify-test-granted (ADR-0074 decision 2), from the same sources: the twin gets
    # POST_NOTIFICATIONS before the start.
    notify-test | notify-test-granted)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      limit=60
      ;;
    # mic-test and its twin mic-test-granted (ADR-0064), from the same sources: the twin gets RECORD_AUDIO before the
    # start.
    mic-test | mic-test-granted)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      limit=60
      ;;
    # pad-test (ADR-0050 test 3): its activity asks for its two packs, each within 30 s, and ends with "done".
    pad-test)
      apk="build/$app/$app-local.apks" build=tools/build-pad-test.sh last=done activity_class=.PadActivity limit=90
      ;;
    # splits-test (ADR-0048 c)): a bundletool set that install-apks installs, whose Application logs and ends with "done".
    splits-test)
      apk="build/$app/$app.apks" build=tools/build-splits-test.sh last=done activity_class=android.app.Activity
      ;;
    # isolated-splits-test (ADR-0048 c)): the same shape with isolatedSplits, built by the same script.
    isolated-splits-test)
      apk="build/$app/$app.apks" build="tools/build-splits-test.sh isolated-splits-test" last=done
      activity_class=android.app.Activity
      ;;
    # broadcast-test (ADR-0045 decision 5): its activity's parts, each within 5 s, end with "done".
    broadcast-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.BroadcastActivity
      limit=90
      ;;
    # clipboard-test (the Unity test APK probe's blocker 11): its activity runs when the window has the focus and ends with "done".
    clipboard-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ClipboardActivity
      limit=90
      ;;
    # dialog-test (the Unity test APK probe's blocker 12): its activity runs when the window has the focus and ends with "done".
    dialog-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.DialogActivity
      limit=90
      ;;
    # second-windows-test (ADR-0041 gap 4 (b)): its activity runs when the window has the focus and ends with "done".
    second-windows-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.SecondWindowsActivity
      limit=150
      ;;
    # title-test (window titles): its activity runs when the window has the focus and ends with "done".
    title-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.TitleActivity
      limit=90
      ;;
    # glsurface-pause-test: HOME and the return drive its GLSurfaceView (below), and it ends with "done".
    glsurface-pause-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.GlPauseActivity
      limit=90
      ;;
    # windows-return-test (ADR-0041 gap 4 (b5a)): HOME and the return drive it (below), and it ends with "done".
    windows-return-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ReturnActivity
      limit=120
      ;;
    # toast-test (ADR-0041 gap 4 (b4)): its activity runs when the window has the focus and ends with "done".
    toast-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ToastActivity
      limit=120
      ;;
    # f2-types-test (framework breadth F2): its activity runs when the window has the focus and ends with "done".
    f2-types-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.F2Activity limit=90
      ;;
    # f4g-audio-patch-test (framework breadth F4g): its activity runs when the window has the focus and ends with "done".
    f4g-audio-patch-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.AudioPatchActivity
      limit=90
      ;;
    # f4f-media-route-test (framework breadth F4f): its activity runs when the window has the focus and ends with "done".
    f4f-media-route-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.RouteActivity limit=90
      ;;
    # f4e-camera-session-test (framework breadth F4e): its activity runs when the window has the focus and ends with
    # "done".
    f4e-camera-session-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.CameraSessionActivity
      limit=90
      ;;
    # f4d-cells-test (framework breadth F4d): its activity runs when the window has the focus and ends with "done".
    f4d-cells-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.CellsActivity limit=90
      ;;
    # its twin f4d-cells-test-fine has the package ...cells.fine but the same activity class.
    f4d-cells-test-fine)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done limit=90
      activity_class=io.github.kisame76.minato.test.cells.CellsActivity
      ;;
    # f4c-types-test (framework breadth F4c): its activity runs when the window has the focus and ends with "done".
    f4c-types-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.TypesActivity limit=90
      ;;
    # f4b-widgets-test (framework breadth F4b): its activity's parts end with "done".
    f4b-widgets-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.WidgetsActivity
      limit=90
      ;;
    # f4a-components-test (framework breadth F4a): its activity's parts end with "done".
    f4a-components-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ComponentsActivity
      limit=90
      ;;
    # date-time-widgets-test (framework breadth F3): its activity's parts end with "done".
    date-time-widgets-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.DateTimeActivity
      limit=90
      ;;
    # picker-test (ADR-0057 step a): its activity starts each file picker intent for a result and ends with "done"; the
    # script presses BACK a few seconds after each "start" line, so that every picker is cancelled.
    picker-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.PickerActivity
      limit=120
      ;;
    # documents-test (ADR-0057 step b): its activity opens a document and then two for a result and ends with "done"; the
    # script has known.txt and data.bin in Download and picks them in DocumentsUI by the texts on its screen
    # (tools/ui-pick.py).
    documents-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.DocumentsActivity
      limit=120
      ;;
    # documents-edit-test (ADR-0057, write-back): its activity opens known.txt, replaces its content through the URI and
    # reads it back, logs "done" and finishes itself, "paused to finish"; the script then reads the file in Download,
    # which holds the new text.
    documents-edit-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last="paused to finish"
      activity_class=.EditActivity
      limit=90
      ;;
    # system-services-test (gap 1's batch 3 of the compatibility plan): its activity asks the services that answered
    # null in Minato as the libraries do and ends with "done"; nothing to push or tap.
    # system-services-test-unpermitted is its twin without the VIBRATE permission, from the same sources.
    system-services-test | system-services-test-unpermitted)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done
      activity_class=io.github.kisame76.minato.test.systemservices.ServicesActivity
      limit=90
      ;;
    # battery-test (gap 1 of the compatibility plan): its activity reads the sticky ACTION_BATTERY_CHANGED and
    # BatteryManager, logs "ready" and then the battery's broadcasts while the script changes the battery in steps,
    # and ends with "done".
    battery-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.BatteryActivity
      limit=120
      ;;
    # sdk-extensions-test: its activity asks android.os.ext.SdkExtensions for the version of each extension and for all
    # of them, and logs what it answers or throws; ends with "done".
    sdk-extensions-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ExtensionsActivity
      limit=90
      ;;
    # turn-test (the turn of the display, a configuration change): PlainActivity, without android:configChanges, asks
    # for landscape and then for portrait and is relaunched at each; it starts ConfigActivity, which has the
    # configChanges and hears onConfigurationChanged and a relayout instead; each logs its callbacks, the
    # configuration and the decor's size. Ends with "done".
    turn-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.PlainActivity
      limit=120
      ;;
    # orientation-request-test (setRequestedOrientation): PlainActivity, without android:configChanges, asks for
    # landscape and then for portrait and is relaunched at each; it starts ConfigActivity, which has the configChanges
    # and hears onConfigurationChanged instead; each logs its callbacks, the configuration, the orientation it asked
    # for and the decor's size. Ends with "done"; the script turns nothing.
    orientation-request-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.PlainActivity
      limit=120
      ;;
    # photos-test (ADR-0057 step c): its activity picks one photo and then two with the photo picker and ends with
    # "done"; the script has red.jpg and blue.jpg in Pictures and in MediaStore, and taps the thumbnails by their
    # descriptions (tools/ui-pick.py), the first is red.jpg.
    photos-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.PhotosActivity
      limit=120
      ;;
    # media-window-test (ADR-0060 e2c3): its activity decodes into its SurfaceView's window, ends with "done".
    media-window-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.WindowActivity
      limit=120
      ;;
    # mediastore-test (ADR-0057 step e): its activity inserts, writes, finishes and deletes media in MediaStore and ends
    # with "done"; nothing to push or tap.
    mediastore-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.MediaStoreActivity
      limit=90
      ;;
    # saves-test (ADR-0057 step d): its activity saves note.txt with CREATE_DOCUMENT, opens the folder treetest with
    # OPEN_DOCUMENT_TREE, works with the provider's calls and ends with "done"; the script has treetest (a.txt, b.txt)
    # in Download and taps DocumentsUI by the texts on its screen (tools/ui-pick.py).
    saves-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.SavesActivity
      limit=120
      ;;
    # documents-tree-test (ADR-0057 step d): its activity opens Documents with OPEN_DOCUMENT_TREE, makes a file there
    # and ends with "done"; its second start, after a force-stop, reads the file through the kept grant.
    documents-tree-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done
      activity_class=.DocumentsTreeActivity limit=120
      ;;
    # alert-test (the Unity test APK probe's blocker 14): its activity runs when the window has the focus and ends with "done".
    alert-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.AlertActivity limit=90
      ;;
    # sqlite-test (the compatibility plan's gap 7): its activity's parts end with "done".
    sqlite-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.SqliteActivity
      ;;
    # timezone-test (the compatibility plan's gap 21): its activity's parts end with "done".
    timezone-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.TimezoneActivity
      ;;
    # package-installer-test (the Unity test APK resolve list, batch E): its activity's parts end with "done".
    package-installer-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.InstallerActivity
      ;;
    # media-absent-test (the Unity test APK gap sort, category 2): its activity's parts end with "done".
    media-absent-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.MediaActivity
      ;;
    # activity-start-test (ADR-0039 step c2c3): its own activity, behind the alias Launcher, ends with "done" once its
    # window has focus.
    activity-start-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.Launcher
      ;;
    # activity-flow-test (ADR-0039 step b3.5b2-1): its launcher, SplashActivity, starts the app's other activities in
    # turn, and its log ends with "done" once the last of them has finished.
    activity-flow-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.SplashActivity
      ;;
    # gesture-flow-test (ADR-0039 step b3.5b2-1b): its launcher, FirstActivity, starts Second at a swipe's DOWN, and its
    # log ends with "done" at the UP of a tap in Second's window.
    gesture-flow-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.FirstActivity
      ;;
    # result-flow-test (ADR-0039 step b3.5b2-2a): its launcher, MainActivity, starts the others for their results, and
    # its log ends with "done" at the destroy of Last, whose finishAndRemoveTask finishes it alone, as it is not the
    # task's root.
    result-flow-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.MainActivity
      ;;
    # launch-mode-test (ADR-0039 step b3.5b2-2b): its launcher, MainActivity, starts activities of every launch mode and
    # with the task's flags, and its log ends with "done" at the destroy of Clear, the last activity of its task.
    launch-mode-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.MainActivity
      ;;
    # crash-test (ADR-0039, the main thread's uncaught exception): its launcher, CrashActivity, throws on the main
    # thread, and its own default handler logs "done" before it hands the exception on to Android's, which ends the
    # process.
    crash-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.CrashActivity
      ;;
    # touch-test (ADR-0045 step 7b): its own launcher activity since step 7b2, TouchActivity, which ends with
    # "done" at the click of the swipe that the script plays.
    touch-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.TouchActivity
      ;;
    # key-test (ADR-0045 step 7c): its own launcher activity, KeyActivity, which ends with "done" once the keys it
    # injects have reached its EditText.
    key-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.KeyActivity
      ;;
    # gamepad-test (ADR-0068 decision 5, the controllers' step 1): its own launcher activity, which logs a virtual
    # game controller of `uinput` (fixtures/apps/gamepad-test/uinput.json) and ends with "done" once it is gone.
    gamepad-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.GamepadActivity
      ;;
    # gamepad-native-test (ADR-0068 decision 5, step 4): a NativeActivity (fixtures/apps), which logs the events of the
    # same virtual controller as gamepad-test and ends with "done" at the 18th motion event.
    gamepad-native-test)
      last=done
      ;;
    # mouse-test (ADR-0068 decision 5, the mouse step): its own launcher activity, which logs a virtual mouse of
    # `uinput` (fixtures/apps/mouse-test/uinput.json) and ends with "done" once it is gone.
    mouse-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.MouseActivity
      ;;
    # mouse-native-test (ADR-0068 decision 5, the mouse step): a NativeActivity (fixtures/apps), which logs the events
    # of the same virtual mouse as mouse-test and ends with "done" at the 46th motion event.
    mouse-native-test)
      last=done
      ;;
    # start-stops-test (ADR-0042 b): its own launcher activity, which ends with "done" once its window has focus.
    start-stops-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.StartStopsActivity
      ;;
    # java-test (ADR-0043 step c1): its activity, which HOME and the return drive, finishes itself and ends each of its
    # two starts with "done".
    java-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done
      activity_class=io.github.kisame76.minato.test.javatest.JavaActivity
      ;;
    # views-test (ADR-0043 step c3, test 1; views-test-default is its twin in the default theme, whose action bar the
    # screen shows too, and views-test-fonts adds a text in Hebrew, Arabic, Thai, Devanagari and emoji): its activity
    # draws in software, changes its label's text once it has the
    # focus, and logs "done" after the draw that follows the click of the script's tap on its Button; its screen follows
    # its log.
    views-test | views-test-default | views-test-fonts | views-test-bidi)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done
      activity_class=io.github.kisame76.minato.test.views.ViewsActivity
      ;;
    # texture-view-test (ADR-0058 d)): its activity, hardware accelerated as an app's is, logs "done" after the draw
    # that shows its plain TextureView's second frame; its screen where the views are follows its log.
    texture-view-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done
      activity_class=io.github.kisame76.minato.test.textureview.TextureViewActivity
      ;;
    # text-test (ADR-0043 step d)): its own launcher activity, which ends with "done" at its window's first focus.
    text-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.TextActivity
      ;;
    # ime-test (ADR-0068 step 2a): its own launcher activity, whose field the fixture input method types into; it ends
    # with "done" after the last key's release.
    ime-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ImeActivity
      ;;
    # ime-insets-test (ADR-0068 step 2d): an adjustResize window whose fixture keyboard has a fixed height; it shows and
    # hides the keyboard and logs the IME inset and the content's size after each, then "done".
    # ime-fallback-test (ADR-0069 step 3): a plain focused view, which the fixture input method types into through the
    # fallback connection; it ends with "done" after the editor action's key.
    ime-fallback-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.FallbackActivity
      ;;
    # webview-start-test (ADR-0056 step b1): its launcher activity makes a WebView as its content and logs what it
    # answers before any page loads; it ends with "done" at the view's first pre-draw.
    webview-start-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.WebViewStartActivity
      ;;
    # webview-test (ADR-0056 step b2): its launcher activity loads an asset page, a missing asset and a large page from
    # loadDataWithBaseURL, and logs what its clients hear; it ends with "done" after the third page's history.
    webview-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.WebViewActivity
      ;;
    # webview-touch-test (ADR-0056 c2, test 3): a translucent activity with a WebView over its window and a Button,
    # a view that is only focusable and a GONE one after it; the script's touches follow its device lines (above). It
    # ends with "done" once the page's field has the focus.
    webview-touch-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.TouchActivity
      ;;
    # webview-cookie-test (ADR-0056 c3): CookieManager before any WebView, then a page at an https base whose script
    # sets cookies, which CookieManager reads, an app's setCookie that the page reads, and removeAllCookies; it ends
    # with "done".
    webview-cookie-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.CookieActivity
      ;;
    # webview-console-test (ADR-0056 c3): an asset page's console messages, three times: with no WebChromeClient and
    # with one whose onConsoleMessage returns false the WebView logs them under chromium, which are the app's lines
    # too; with one that returns true it does not. It ends with "done".
    webview-console-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ConsoleActivity
      also_tag=chromium
      ;;
    # webview-bridge-test (ADR-0056 c3): objects added with addJavascriptInterface, which the page calls synchronously
    # (their @JavascriptInterface methods only), seen from the next load on and gone after a removal and a load. It
    # ends with "done". webview-bridge-test-16 is its twin of target SDK 16, whose page calls every public method.
    webview-bridge-test | webview-bridge-test-16)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.BridgeActivity
      ;;
    # network-test (ADR-0054 step d)): the active network, its NetworkInfo and capabilities, and the default network
    # callback's first calls, on the emulator's Wi-Fi: its mobile data, the default network otherwise, is off for the
    # run (mobile_data_off), as a Mac or an iPhone on Wi-Fi has it. It ends with "done".
    network-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.NetworkActivity
      ;;
    # network-request-test (ADR-0054 step d)): built NetworkRequests, which capabilities satisfy them, listens and
    # requests for the Internet and for cellular, one with a timeout, on the emulator's Wi-Fi as for network-test, with
    # no cellular network. It ends with "done".
    network-request-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.RequestActivity
      ;;
    # cellular-network-test (ADR-0054 step d)): the default network's legacy info with its subtype, its capabilities
    # and metering, on the emulator's cellular network alone: its Wi-Fi off for the run (wifi_off). It ends with
    # "done".
    cellular-network-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.CellularActivity
      ;;
    # restrict-background-test (ADR-0054 step d)): the restrict-background status, then its change broadcasts while
    # the script turns Data Saver on and, 2 s later, off (cmd netpolicy), as Minato follows the iPhone's Low Data
    # Mode. It ends with "done".
    restrict-background-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.RestrictActivity
      ;;
    # wifi-state-test (ADR-0054 step d)): the sticky CONNECTIVITY_ACTION and WIFI_STATE_CHANGED_ACTION at a receiver's
    # registering, and WifiManager's state and connection info without a location permission, on the emulator's Wi-Fi
    # as for network-test. It ends with "done".
    wifi-state-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.WifiStateActivity
      ;;
    # webview-late-test: a WebView made late, after what HTML5 app wrappers wait on first (a delay, an AsyncTask's copy
    # of the page to the app's files, Safe Browsing, the cookies), each with a limit of 5 s; the copied page as a
    # file:// URL is refused with file access off and runs with it on. It ends with "done".
    webview-late-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.LateActivity
      ;;
    # hang-test: its main thread sleeps 60 s after its first frame; it ends with "awake".
    hang-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=awake limit=120
      activity_class=.HangActivity
      ;;
    # ime-extract-test (ADR-0068, the host's text bar): one field and a password field, whose text and selection the
    # fixture input method asks for with getExtractedText (flags 0, no monitoring) and hears of while it types; the
    # method's lines under ime-extract-test-device are the numbers that Minato's extract requests must carry. It ends
    # with "done".
    ime-extract-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ExtractActivity
      ;;
    # enter-test (ADR-0068, the return key): seven fields that its fixture input method serves as LatinIME does, one
    # after another; it ends with "done" after the last field. enter-test-15 is its twin of target SDK 15.
    enter-test | enter-test-15)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.EnterActivity
      ;;
    ime-insets-test | ime-insets-test-34)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ResizeActivity
      ;;
    # ime-hide-test (ADR-0068, the keyboard reopen bug): two engine-style fields that track the soft keyboard, one by
    # onKeyPreIme(BACK) and one by the IME inset; ime-hide-test-cb is its twin that opted in to
    # OnBackInvokedCallback. Ends with "done" 16 seconds after the first focus.
    ime-hide-test | ime-hide-test-cb)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.HideActivity
      ;;
    toolbar-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ToolbarActivity
      ;;
    # action-bar-menu-test (the platform action bar's options menu, step 4): the MENU key opens the overflow, the arrow
    # and enter keys choose its first item, and after the app's invalidateOptionsMenu the MENU key opens and closes it,
    # then the BACK key finishes the action mode the app starts; ends with "done".
    # action-mode-standalone-test (step 5b): an activity without an action bar starts an action mode once it has the
    # focus, which its window's own bar shows, and finishes it; ends with "done".
    action-mode-standalone-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.ModeActivity
      ;;
    action-bar-menu-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.MenuActivity
      ;;
    # trim-memory-test (the memory-warning step): the callbacks of ComponentCallbacks2 that the Application, the
    # activity, a provider and a registered callback get when the system trims the app's memory, which the script
    # asks for with `am send-trim-memory` level by level, with HOME between the running and the background levels;
    # ends with "done" after level 80.
    trim-memory-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.TrimActivity
      ;;
    # selection-mode-test (the text selection's action mode): a long press on a word of an EditText, at the place the
    # app names, starts the selection's floating action mode, which the app's custom callback and its activity log;
    # ends with "done".
    selection-mode-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.SelectActivity
      ;;
    # menu-pause-test (ADR-0032 decision 7, the menu's pause): MainActivity's callbacks while a dialog themed
    # activity of another task comes over it and is finished with Back (a), while the screen goes off and on with the
    # keyguard showing and is unlocked (b), and at Home and back (c); the script writes a marker line before each part
    # and "done" at the end.
    menu-pause-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.MainActivity
      ;;
    # insertion-mode-test (the text cursor's insertion mode): a tap in the empty space after an EditText's text, a tap
    # on the insertion handle below the cursor and a long press in the empty space, at the places the app names; the
    # last two start the insertion's floating action mode, which the app's custom callback and its activity log; ends
    # with "done".
    insertion-mode-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.InsertActivity
      ;;
    # action-bar-test (the platform action bar, compatibility plan gap 5): a default-theme activity that uses
    # getActionBar(), an options menu with an overflow, hide and show, and an action mode; ends with "done".
    action-bar-test | action-bar-test-34)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.BarActivity
      ;;
    # back-test (ADR-0041 decision 10, the system's back): Root starts Second, and three BACKs pop Second's fragment,
    # finish Second and reach Root, which ends with "done". back-test-callback is its twin that opted in to
    # OnBackInvokedCallback.
    back-test | back-test-callback)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.RootActivity
      ;;
    # native-ime-test (ADR-0068 step 2e): a NativeActivity that asks for the keyboard, which Android 15 ignores, so its
    # fixture input method never starts; after its "soft input done" the script sends B and DEL as keys from outside,
    # and the DEL's release ends it with "done".
    native-ime-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done
      ;;
    # native-jni-ime-test (ADR-0069 step 1): a NativeActivity that opens the keyboard through JNI, which its fixture
    # input method types into; then, as for native-ime-test, B and DEL from outside, and "done".
    native-jni-ime-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done
      ;;
    # surface-test (ADR-0043 step c4): its own launcher activity, whose GLSurfaceView draws its fixed frames after the
    # window's first focus, then a Button over it, which ends with "done" at its first draw.
    surface-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=.SurfaceActivity
      ;;
    # app-start-test (ADR-0039 test 1) prints its lines to System.out from its Application class.
    app-start-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last="app-start-test: MessageDigest"
      activity_class=android.app.Activity tag=System.out
      ;;
    # canvas-test (ADR-0043 test 2): as those above, and its pixels follow its log.
    canvas-test)
      apk="build/$app/$app.apk" build="tools/build-java-test-app.sh $app" last=done activity_class=android.app.Activity
      ;;
    # present-test's apps draw 1200 frames, at 60 Hz on the emulator also in their 120 Hz half: about 45 s.
    present-gles | present-vulkan)
      limit=120
      ;;
  esac
  # APK_SET: a bundletool set (.apks) in place of the app's APK, installed as the device's splits (ADR-0048 c4:
  # APK_SET=build/activity-splits/activity-splits.apks tools/android-reference.sh --app activity-test).
  [[ -z "${APK_SET:-}" ]] || { apk=$APK_SET build=tools/build-activity-splits.sh; }
  if [[ ! -f "$apk" ]]; then
    echo "No $apk: run $build first." >&2
    return 1
  fi
  package=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "$manifest" | head -n 1)
  activity="$package/$activity_class"
  device uninstall "$package" >/dev/null 2>&1 || true
  if [[ "$app" == pad-test || "$app" == splits-test || "$app" == isolated-splits-test || -n "${APK_SET:-}" ]]; then
    # A local-testing set: install-apks installs the base and pushes the base's language splits and the packs' slices
    # into local_testing; it lists each push, so its output goes to a file, shown when it fails. splits-test's set is
    # installed as the device's splits.
    local java bundletool
    java=$(find build/tools/jdk-17 -mindepth 3 -maxdepth 3 -name Home -path '*/Contents/Home' 2>/dev/null | sort | tail -n 1)
    bundletool=$(/usr/bin/python3 tools/fetch-bundletool.py) || return 1
    "$java/bin/java" -jar "$bundletool" install-apks --apks="$apk" --adb="$ADB" --device-id="$serial" \
      >"$RESULTS/$app-install.log" 2>&1 || { sed 's/^/  /' "$RESULTS/$app-install.log" >&2; return 1; }
  elif [[ "$app" == enter-test-15 || "$app" == webview-bridge-test-16 ]]; then
    # Android 14 and later refuse a target SDK below 23 unless install bypasses the block.
    device install --bypass-low-target-sdk-block "$apk" >/dev/null
  else
    device install "$apk" >/dev/null
  fi
  # A ring for a long log (media-extractor-test's about 600 lines) beside the system's own lines.
  run logcat -G 16M >/dev/null 2>&1 || true
  run logcat -c
  if [[ "$app" == ime-test || "$app" == ime-insets-test || "$app" == ime-insets-test-34 ||
    "$app" == ime-hide-test || "$app" == ime-hide-test-cb ||
    "$app" == ime-fallback-test || "$app" == native-ime-test || "$app" == native-jni-ime-test ||
    "$app" == enter-test || "$app" == enter-test-15 || "$app" == ime-extract-test ]]; then
    # The fixture input method of the app just installed is the one that serves its field; the image's own comes back
    # with the exit trap.
    run ime enable "$package/.FixtureInputMethod" >/dev/null
    run ime set "$package/.FixtureInputMethod" >/dev/null
  fi
  if [[ "$AVD" == apk-runner-35-iphone-gesture* ]]; then
    # Gesture navigation's overlay, and the screen on past the keyguard, so that the window takes focus.
    run cmd overlay enable-exclusive --category com.android.internal.systemui.navbar.gestural >/dev/null
    run input keyevent KEYCODE_WAKEUP
    run wm dismiss-keyguard
  fi
  local rotation=""
  if [[ "$app" == rotation-test || "$app" == turn-test ]]; then
    # The activity leaves its orientation unspecified, so with the rotation locked it follows the user rotation.
    # "cmd window user-rotation" says "free" or "lock <rotation>", which it takes back as it is.
    rotation=$(run cmd window user-rotation | tr -d '\r')
    run cmd window user-rotation lock 0 >/dev/null
  fi
  if [[ "$app" == mic-test-granted || "$app" == audio-input-test-granted ||
        "$app" == visualizer-test-granted ]]; then
    run pm grant "$package" android.permission.RECORD_AUDIO
  fi
  if [[ "$app" == camera-characteristics-test-granted || "$app" == camera-legacy-test ||
        "$app" == camera-preview-test || "$app" == camera-picture-test ]]; then
    run pm grant "$package" android.permission.CAMERA
  fi
  if [[ "$app" == permission-request-test-notifications || "$app" == notify-test-granted ]]; then
    # ADR-0074 decision 1: POST_NOTIFICATIONS is granted before the start, as the -camera twin's CAMERA.
    run pm grant "$package" android.permission.POST_NOTIFICATIONS
  fi
  if [[ "$app" == permission-request-test-camera ]]; then
    # ADR-0073 decision 8: CAMERA is granted before the start, as the -fine twin's location.
    run pm grant "$package" android.permission.CAMERA
  fi
  if [[ "$app" == permission-request-test-fine ]]; then
    # ADR-0063: the twin's permissions are granted before the start, so that its requests need no dialog.
    run pm grant "$package" android.permission.ACCESS_FINE_LOCATION
    run pm grant "$package" android.permission.ACCESS_COARSE_LOCATION
  fi
  if [[ "$app" == location-manager-test* ]]; then
    # ADR-0063: what the twin's name says is granted before the start (-fine both location permissions, -coarse the
    # coarse one), and the emulator's GNSS is set to a position in Berlin, which its next fixes report.
    case "$app" in
      location-manager-test-fine)
        run pm grant "$package" android.permission.ACCESS_FINE_LOCATION
        run pm grant "$package" android.permission.ACCESS_COARSE_LOCATION
        ;;
      location-manager-test-coarse)
        run pm grant "$package" android.permission.ACCESS_COARSE_LOCATION
        ;;
    esac
    device emu geo fix 13.405 52.52 40 >/dev/null
  fi
  if [[ "$app" == f4d-cells-test-fine ]]; then
    # ADR-0063 decision 9: the fine location permission it requests is granted before the start, so that
    # getAllCellInfo answers its list where f4d-cells-test gets a SecurityException.
    run pm grant "$package" android.permission.ACCESS_FINE_LOCATION
  fi
  if [[ "$app" == obb-test ]]; then
    # ADR-0049 test 8: its main and patch OBBs in its OBB folder before the start, as a user or a store puts them there.
    run mkdir -p "/sdcard/Android/obb/${package:?}"
    device push "fixtures/apps/$app/main.1.$package.obb" "/sdcard/Android/obb/$package/" >/dev/null
    device push "fixtures/apps/$app/patch.1.$package.obb" "/sdcard/Android/obb/$package/" >/dev/null
  fi
  if [[ "$app" == saves-test ]]; then
    # ADR-0057 step d: the folder the user opens, with two files, in Download; note.txt is not there yet.
    run rm -rf /sdcard/Download/treetest /sdcard/Download/note.txt /sdcard/Download/renamed.txt
    device push "fixtures/apps/$app/files/treetest" /sdcard/Download/ >/dev/null
  fi
  if [[ "$app" == documents-tree-test ]]; then
    # ADR-0057 step d: no file of the test's in Documents yet, which DocumentsUI opens at (EXTRA_INITIAL_URI).
    run mkdir -p /sdcard/Documents
    run rm -f '/sdcard/Documents/documents-tree-test*.txt'
  fi
  if [[ "$app" == photos-test ]]; then
    # ADR-0057 step c: the two photos the picker shows, in Pictures and indexed by MediaStore as the scanner does.
    local photo
    for photo in red blue; do
      device push "fixtures/apps/$app/files/$photo.jpg" /sdcard/Pictures/ >/dev/null
      run content call --uri content://media/external/file --method scan_file --arg "/sdcard/Pictures/$photo.jpg" \
        >/dev/null
    done
  fi
  if [[ "$app" == documents-edit-test ]]; then
    # ADR-0057, write-back: known.txt in Download, picked and edited by the app.
    device push "fixtures/apps/$app/files/known.txt" /sdcard/Download/ >/dev/null
  fi
  if [[ "$app" == documents-test ]]; then
    # ADR-0057 step b: the two files the pickers pick, in Download, where a user or a browser puts them.
    device push "fixtures/apps/$app/files/known.txt" /sdcard/Download/ >/dev/null
    device push "fixtures/apps/$app/files/data.bin" /sdcard/Download/ >/dev/null
  fi
  if [[ "$app" == services-test ]]; then
    # Gap 1's batch 2: the notification service keeps an uninstalled app's channels in its policy file when the
    # emulator stops before it writes the removal, and gives them back at the next install, which gets the same uid;
    # the package's data cleared drops them, so that each run starts as a first install does.
    run pm clear "${package:?}" >/dev/null
  fi
  if [[ -n "${BENCH_CONF:-}" && ("$app" == bench-gles || "$app" == bench-vulkan) ]]; then
    bench_conf_in "$package"
  fi
  if [[ "$app" == battery-test ]]; then
    # The battery the run starts with, as apk-run's fixed one: 100 % charging on AC. BatteryService takes each value
    # as one update and stops reading the emulator's battery until the reset at the end.
    run dumpsys battery set ac 1 >/dev/null && run dumpsys battery set status 2 >/dev/null &&
      run dumpsys battery set level 100 >/dev/null || return 1
  fi
  run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
  if [[ "$app" == enter-test-15 || "$app" == webview-bridge-test-16 ]]; then
    # Android 15 asks to review the permissions of a fresh app of target SDK below 23 (ReviewPermissionsActivity),
    # which ends at the launcher, so the app starts again; then it may warn that the app was built for an older
    # Android (DeprecatedTargetSdkVersionDialog), which a tap on OK keeps away for the package from then on, also
    # across installs. Each has the focus: CONTINUE, and OK when it shows, as a user taps them; the app's window then
    # takes the focus and its run begins.
    if ! python3 tools/ui-pick.py "$ADB" "$serial" tap CONTINUE 20 ||
      ! run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \
        -n "$activity" >/dev/null; then
      run dumpsys window | grep -E "mCurrentFocus|mFocusedApp" >&2 || true
      run uiautomator dump /sdcard/enter-ui.xml >/dev/null 2>&1 &&
        run cat /sdcard/enter-ui.xml | grep -oE 'text="[^"]+"' >&2 || true
      return 1
    fi
    python3 tools/ui-pick.py "$ADB" "$serial" tap OK 5 >/dev/null 2>&1 || true
  fi
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
  if [[ "$app" == turn-test ]]; then
    # The display turns to landscape and back at each "ready for turn" of the app, which says it twice for each of its
    # two activities (the plain one, relaunched at each turn, and the one with configChanges).
    local turned=yes
    { wait_count "$app" "ready for turn 1" 1 && run cmd window user-rotation lock 1 >/dev/null &&
      wait_count "$app" "ready for turn 2" 1 && run cmd window user-rotation lock 0 >/dev/null &&
      wait_count "$app" "ready for turn 1" 2 && run cmd window user-rotation lock 1 >/dev/null &&
      wait_count "$app" "ready for turn 2" 2 && run cmd window user-rotation lock 0 >/dev/null; } || turned=no
    # shellcheck disable=SC2086
    run cmd window user-rotation $rotation >/dev/null
    [[ "$turned" == yes ]] || return 1
  fi
  if [[ "$app" == restrict-background-test ]]; then
    # Data Saver on once the app is ready, off 2 s later; off at the end in any case.
    local saved=yes
    { wait_for "$app" ready && sleep 1 && run cmd netpolicy set restrict-background true >/dev/null && sleep 2 &&
      run cmd netpolicy set restrict-background false >/dev/null; } || saved=no
    run cmd netpolicy set restrict-background false >/dev/null || true
    [[ "$saved" == yes ]] || return 1
  fi
  if [[ "$app" == battery-test ]]; then
    # The battery's steps, two seconds apart once the app is ready: to 50 %, unplugged (AC off, then discharging),
    # to 10 %, plugged in (AC on, then charging), to 50 % again; then BatteryService reads the emulator's battery
    # again.
    local stepped=yes
    { wait_for "$app" ready && sleep 2 && run dumpsys battery set level 50 >/dev/null && sleep 2 &&
      run dumpsys battery set ac 0 >/dev/null && sleep 1 && run dumpsys battery set status 3 >/dev/null && sleep 2 &&
      run dumpsys battery set level 10 >/dev/null && sleep 2 &&
      run dumpsys battery set ac 1 >/dev/null && sleep 1 && run dumpsys battery set status 2 >/dev/null && sleep 2 &&
      run dumpsys battery set level 50 >/dev/null; } || stepped=no
    run dumpsys battery reset >/dev/null
    [[ "$stepped" == yes ]] || return 1
  fi
  if [[ "$app" == touch-test ]]; then
    # ADR-0045 step 7b2: a tap in the window's middle, then a swipe to the right through it, each a second after
    # the line before it, as apk-run's lifecycle plays them in tools/touch-check.sh.
    { wait_for "$app" "pad: ready" && sleep 1 && run input tap 645 1398 &&
      wait_for "$app" "gesture: single tap confirmed" && sleep 1 &&
      run input swipe 300 1398 1000 1398 200; } || return 1
  fi
  if [[ "$app" == action-bar-menu-test ]]; then
    # The keys of tools/action-bar-menu-check.sh's lifecycle, a second and a half apart: MENU, DOWN, ENTER, then, after
    # the app's invalidation, MENU twice, and BACK once the action mode has started.
    { wait_for "$app" ready 90 && sleep 1.5 && run input keyevent KEYCODE_MENU && sleep 1.5 &&
      run input keyevent KEYCODE_DPAD_DOWN && sleep 1.5 && run input keyevent KEYCODE_ENTER &&
      wait_for "$app" "invalidateOptionsMenu" 20 && sleep 1.5 && run input keyevent KEYCODE_MENU && sleep 1.5 &&
      run input keyevent KEYCODE_MENU && wait_for "$app" "onActionModeStarted" 20 && sleep 1.5 &&
      run input keyevent KEYCODE_BACK; } || return 1
  fi
  if [[ "$app" == views-test* ]]; then
    # ADR-0043 test 1: a tap on the Button's middle, from its logged bounds in the window, which fills the screen, a
    # second after the label's change, as apk-run's lifecycle plays it in tools/views-check.sh.
    local bounds left top right bottom
    { wait_for "$app" "text changed" && sleep 1; } || return 1
    bounds=$(app_lines "$app" | tr -d '\r' | sed -n 's/.*: button bounds \([0-9 ]*\)$/\1/p' | tail -n 1)
    read -r left top right bottom <<<"$bounds"
    [[ -n "$bottom" ]] || { echo "views-test logged no button bounds." >&2; return 1; }
    run input tap $(((left + right) / 2)) $(((top + bottom) / 2)) || return 1
  fi
  if [[ "$app" == webview-touch-test ]]; then
    # ADR-0056 c2's test 3: from the app's device lines (the screen's pixels), a tap on the Button, a tap on the page
    # through the view that is only focusable, a swipe up on the page and, once the app has scrolled the page back to
    # its top, a tap on its field, each once the app has logged the one before it.
    local device button tap field swipe
    { wait_for "$app" ready 60 && sleep 1; } || return 1
    device=$(app_lines "$app-device" | tr -d '\r')
    button=$(sed -n 's/.*: button point //p' <<<"$device" | tail -n 1)
    tap=$(sed -n 's/.*: tap point //p' <<<"$device" | tail -n 1)
    field=$(sed -n 's/.*: field point //p' <<<"$device" | tail -n 1)
    swipe=$(sed -n 's/.*: swipe line //p' <<<"$device" | tail -n 1)
    [[ -n "$button" && -n "$tap" && -n "$field" && -n "$swipe" ]] || { echo "$app logged no points." >&2; return 1; }
    # shellcheck disable=SC2086 # each is "x y" (the swipe "x1 y1 x2 y2"), split on purpose
    { run input tap $button && wait_for "$app" "close tapped" && sleep 1 && run input tap $tap &&
      wait_for "$app" "title: page tapped" && sleep 1 && run input swipe $swipe 300 &&
      wait_for "$app" "scrolled back" && sleep 1 && run input tap $field; } || return 1
  fi
  if [[ "$app" == picker-test ]]; then
    # ADR-0057 step a: each picker is cancelled with BACK a few seconds after its start line, once it shows.
    local picked
    for picked in OPEN_DOCUMENT GET_CONTENT CREATE_DOCUMENT OPEN_DOCUMENT_TREE PICK_IMAGES; do
      wait_for "$app" "start $picked" && sleep 4 && run input keyevent KEYCODE_BACK || return 1
    done
  fi
  if [[ "$app" == documents-test ]]; then
    # ADR-0057 step b: into the internal storage's Download folder, where a tap picks known.txt; the second picker
    # allows several, so a long press selects known.txt, a tap data.bin, and Select takes both; the third, GET_CONTENT,
    # picks known.txt again. The files leave Download again.
    local pick="python3 tools/ui-pick.py $ADB $serial" picks=ok
    # Word splitting is wanted: $pick is a command.
    # shellcheck disable=SC2086
    { wait_for "$app" "start single" && $pick download && $pick tap known.txt &&
      wait_for "$app" "start multiple" && sleep 3 && $pick download && $pick hold known.txt && $pick tap data.bin &&
      $pick tap Select && wait_for "$app" "start get-content" && sleep 3 && $pick download && $pick tap known.txt &&
      wait_for "$app" done 60; } || picks=failed
    run rm -f /sdcard/Download/known.txt /sdcard/Download/data.bin
    [[ "$picks" == ok ]] || return 1
  fi
  if [[ "$app" == documents-edit-test ]]; then
    # ADR-0057, write-back: the one picker is DocumentsUI's, in Download; the file must hold the app's text afterwards.
    local pick="python3 tools/ui-pick.py $ADB $serial" picks=ok
    # Word splitting is wanted: $pick is a command.
    # shellcheck disable=SC2086
    { wait_for "$app" "start" && $pick download && $pick tap known.txt && wait_for "$app" done 60; } || picks=failed
    local after
    after=$(run cat /sdcard/Download/known.txt | tr -d '\r')
    echo "known.txt after the run: $after"
    [[ "$after" == "edited by app" ]] || picks=failed
    run rm -f /sdcard/Download/known.txt
    [[ "$picks" == ok ]] || return 1
  fi
  if [[ "$app" == saves-test ]]; then
    # ADR-0057 step d: the save dialog of DocumentsUI in Download takes the title as it is (its button reads SAVE);
    # the folder picker in Download opens treetest, USE THIS FOLDER, and the dialog that follows (ALLOW). What the app
    # made leaves Download again.
    local pick="python3 tools/ui-pick.py $ADB $serial" picks=ok
    # Word splitting is wanted: $pick is a command.
    # shellcheck disable=SC2086
    { wait_for "$app" "start create" && $pick download && $pick tap SAVE 20 &&
      wait_for "$app" "start tree" && sleep 3 && $pick download && $pick tap treetest 20 &&
      $pick tap "USE THIS FOLDER" 20 && $pick tap ALLOW 20 && wait_for "$app" done 60; } || picks=failed
    run rm -rf /sdcard/Download/treetest /sdcard/Download/note.txt /sdcard/Download/renamed.txt
    [[ "$picks" == ok ]] || return 1
  fi
  if [[ "$app" == documents-tree-test ]]; then
    # ADR-0057 step d: DocumentsUI opens at Documents; USE THIS FOLDER and the dialog that follows (ALLOW). Then a
    # force-stop and a second start, which reads the file through the kept grant and deletes it.
    local pick="python3 tools/ui-pick.py $ADB $serial" picks=ok ended=0
    # Word splitting is wanted: $pick is a command.
    # shellcheck disable=SC2086
    { wait_for "$app" "start tree" && sleep 3 && $pick tap "USE THIS FOLDER" 20 && $pick tap ALLOW 20 &&
      wait_for "$app" done 60; } || picks=failed
    if [[ "$picks" == ok ]]; then
      run am force-stop "$package"
      run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
      for _ in $(seq 1 60); do
        ended=$(app_lines "$app" | grep -c -E ": done.?$" || true)
        [[ "$ended" -lt 2 ]] || break
        sleep 0.5
      done
      [[ "$ended" -ge 2 ]] || picks=failed
    fi
    run rm -f '/sdcard/Documents/documents-tree-test*.txt'
    [[ "$picks" == ok ]] || return 1
  fi
  if [[ "$app" == photos-test ]]; then
    # ADR-0057 step c: the first thumbnail picks red.jpg; the second picker allows two: both thumbnails, then "Add".
    # The photos leave Pictures again, which MediaStore notices.
    local pick="python3 tools/ui-pick.py $ADB $serial" picks=ok
    # Word splitting is wanted: $pick is a command.
    # shellcheck disable=SC2086
    { wait_for "$app" "start single" && $pick tap "~Photo taken on" 20 0 &&
      wait_for "$app" "start two" && sleep 3 && $pick tap "~Photo taken on" 20 0 && $pick tap "~Photo taken on" 20 1 &&
      $pick tap "~Add" 20 && wait_for "$app" done 60; } || picks=failed
    run rm -f /sdcard/Pictures/red.jpg /sdcard/Pictures/blue.jpg
    [[ "$picks" == ok ]] || return 1
  fi
  if [[ "$app" == native-ime-test || "$app" == native-jni-ime-test ]]; then
    # ADR-0068 step 2e: KEYCODE_B and KEYCODE_DEL a second apart once the soft keyboard's part is done, as the host
    # sends a hardware keyboard's keys through Minato's input channel.
    { wait_for "$app" "soft input done" 20 && sleep 1 && run input keyevent 30 && sleep 1 && run input keyevent 67; } ||
      return 1
  fi
  if [[ "$app" == ime-hide-test || "$app" == ime-hide-test-cb ]]; then
    # The keyboard reopen bug: a tap on the first field shows the keyboard, BACK hides it, a tap on the first field
    # again; then BACK, and the same with the second field. The navigation bar's hide key sends the same BACK, with the
    # flags 0x48 besides the source 0x101 that `input keyboard` gives.
    { wait_for "$app" "ready: hide" && sleep 1 && run input tap 645 250 && sleep 2 &&
      run input keyboard keyevent KEYCODE_BACK && sleep 2 && run input tap 645 250 && sleep 2 &&
      run input keyboard keyevent KEYCODE_BACK && sleep 2 && run input tap 645 550 && sleep 2 &&
      run input keyboard keyevent KEYCODE_BACK && sleep 2 && run input tap 645 550; } || return 1
  fi
  if [[ "$app" == gamepad-test || "$app" == gamepad-native-test ]]; then
    # ADR-0068 decision 5: once the window says "ready: pad", `uinput` makes a game controller, plays its buttons,
    # sticks, triggers and hat, and ends, which removes the device.
    device push fixtures/apps/gamepad-test/uinput.json /data/local/tmp/gamepad-uinput.json >/dev/null || return 1
    { wait_for "$app" "ready: pad" && sleep 1 && run uinput /data/local/tmp/gamepad-uinput.json; } || return 1
    run rm -f /data/local/tmp/gamepad-uinput.json
  fi
  if [[ "$app" == mouse-test || "$app" == mouse-native-test ]]; then
    # ADR-0068 decision 5: once the window says "ready: mouse", `uinput` makes a mouse, plays its moves, buttons and
    # wheels, and ends, which removes the device.
    device push fixtures/apps/mouse-test/uinput.json /data/local/tmp/mouse-uinput.json >/dev/null || return 1
    { wait_for "$app" "ready: mouse" && sleep 1 && run uinput /data/local/tmp/mouse-uinput.json; } || return 1
    run rm -f /data/local/tmp/mouse-uinput.json
  fi
  if [[ "$app" == key-test ]]; then
    # ADR-0045 step 7c: the typed digit takes the display out of touch mode, which outlives the app; a tap in its
    # window once it is done brings the display back, so that the runs after it start in touch mode as before.
    # ADR-0045 step 7c2: three keys from outside the app, KEYCODE_2, KEYCODE_B and KEYCODE_DEL, a second apart once it
    # logs "channel: ready", as the host sends them through Minato's input channel.
    { wait_for "$app" "channel: ready" && sleep 1 && run input keyevent 9 && sleep 1 && run input keyevent 30 &&
      sleep 1 && run input keyevent 67 && wait_for "$app" done && run input tap 645 1398; } || return 1
  fi
  if [[ "$app" == back-test || "$app" == back-test-callback ]]; then
    # ADR-0041 decision 10: a back a second after Second's focus, after its fragment's pop and after Root's return, as
    # apk-run's back steps in tools/back-check.sh. back-test's is a swipe in from the left edge under gesture
    # navigation, whose compat callback makes the BACK keys in the app's process, as Minato's menu does: a key from
    # `input keyevent` has another source (0). back-test-callback's is that BACK key, which an opted-in app's window
    # gives to its top callback as Minato's keys; its last, after "done", reaches Root's default callback, which moves
    # the task to the back.
    local back=(input keyevent KEYCODE_BACK)
    [[ "$app" == back-test-callback ]] || back=(input swipe 10 1398 800 1398 400)
    { wait_for "$app-steps" "ready: second" && sleep 1 && run "${back[@]}" &&
      wait_for "$app" "second back stack 0" && sleep 1 && run "${back[@]}" &&
      wait_for "$app-steps" "ready: root" && sleep 1 && run "${back[@]}"; } || return 1
    if [[ "$app" == back-test-callback ]]; then
      { wait_for "$app" done && sleep 1 && run "${back[@]}"; } || return 1
    fi
  fi
  if [[ "$app" == second-windows-test ]]; then
    # ADR-0041 gap 4 (b2): a tap a second after each of the touches part's ready lines, at the place each names, as
    # tools/second-windows-check.sh's lifecycle taps: the dialog's OK, outside the next dialog, the target under a
    # popup that watches outside touches, in a focusable popup and on the target again.
    tap_when_ready() {
      wait_for "$app" "ready: $1, tap at " || return 1
      local point
      point=$(app_lines "$app" | sed -n "s/.*ready: $1, tap at \([0-9]*\),\([0-9]*\)\$/\1 \2/p" | tail -n 1)
      sleep 1 && run input tap $point
    }
    { tap_when_ready "dialog 1" && tap_when_ready "dialog 2" && tap_when_ready "popup 1" &&
      tap_when_ready "popup 2" && tap_when_ready "popup 2 again"; } || return 1
  fi
  if [[ "$app" == menu-pause-test ]]; then
    local package=io.github.kisame76.minato.test.menupause locked
    # One word for the device's shell: the text's spaces become underscores.
    marker() { run log -t "$app" "mark_${1// /_}"; }
    wait_for "$app" "ready" || return 1
    # The image's lock screen is off (set-disabled true); the swipe keyguard is on for this run only.
    run locksettings set-disabled false >/dev/null
    sleep 2
    marker "a a dialog themed activity of another task"
    run am start -n "$package/.OverActivity" >/dev/null
    wait_for "$app" "OverActivity onWindowFocusChanged true" || return 1
    sleep 1
    run input keyevent KEYCODE_BACK
    wait_for "$app" "OverActivity onDestroy" || return 1
    wait_count "$app" "MainActivity onWindowFocusChanged true" 2 || return 1
    sleep 2
    marker "b the screen off with the keyguard, then on"
    run input keyevent KEYCODE_SLEEP
    sleep 3
    run input keyevent KEYCODE_WAKEUP
    sleep 3
    marker "b unlocked"
    run wm dismiss-keyguard
    sleep 3
    marker "c Home and back"
    run input keyevent KEYCODE_HOME
    sleep 2
    # As the launcher starts it, which brings the stopped task back instead of making a second activity.
    run am start -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \
      -n "$package/.MainActivity" >/dev/null
    sleep 3
    run locksettings set-disabled true >/dev/null
    # The setting reaches the disk some seconds later; an emulator that stops sooner keeps the keyguard on.
    sleep 30
    run log -t "$app" done
  fi
  if [[ "$app" == insertion-mode-test ]]; then
    wait_for "$app" "ready: long press at " || return 1
    local tap handle
    tap=$(app_lines "$app" | sed -n "s/.*ready: tap at \([0-9]*\),\([0-9]*\)\$/\1 \2/p" | tail -n 1)
    handle=$(app_lines "$app" | sed -n "s/.*ready: handle at \([0-9]*\),\([0-9]*\)\$/\1 \2/p" | tail -n 1)
    sleep 1
    run input tap $tap
    sleep 1.5
    run input tap $handle
    sleep 1.5
    run input swipe $tap $tap 1000
  fi
  if [[ "$app" == selection-mode-test ]]; then
    wait_for "$app" "ready: long press at " || return 1
    point=$(app_lines "$app" | sed -n "s/.*ready: long press at \([0-9]*\),\([0-9]*\)\$/\1 \2/p" | tail -n 1)
    sleep 1
    run input swipe $point $point 1000
  fi
  if [[ "$app" == trim-memory-test ]]; then
    # The system refuses a background level for a foreground process, so the running levels go first, then HOME,
    # after which the system itself trims the hidden UI (level 20), before or after onStop, and then the background
    # levels.
    wait_for "$app" "ready" || return 1
    for level in RUNNING_MODERATE RUNNING_LOW RUNNING_CRITICAL; do
      sleep 1
      run am send-trim-memory io.github.kisame76.minato.test.trimmemory $level
    done
    sleep 1
    run input keyevent KEYCODE_HOME
    wait_for "$app-steps" "activity onStop" || return 1
    wait_for "$app" "activity onTrimMemory 20" || return 1
    for level in BACKGROUND MODERATE COMPLETE; do
      sleep 1
      run am send-trim-memory io.github.kisame76.minato.test.trimmemory $level
    done
  fi
  if [[ "$app" == gesture-flow-test ]]; then
    # Step b3.5b2-1b: a 3 s swipe down the window's middle from First's focus on, whose DOWN starts Second, so that the
    # finger is still down after First stops; then a tap in Second's window, a second after it is ready, as apk-run's
    # lifecycle plays them in tools/gesture-flow-check.sh.
    { wait_for "$app" "First ready" && sleep 1 && run input swipe 645 1000 645 2200 3000 &&
      wait_for "$app" "Second ready" && sleep 1 && run input tap 645 1398; } || return 1
  fi
  if [[ "$app" == activity-test ]]; then
    wait_for "$app" APP_CMD_GAINED_FOCUS || return 1
    sleep 1
    run input keyevent KEYCODE_HOME
    wait_for "$app" APP_CMD_SAVE_STATE || return 1
    sleep 1
    run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
  fi
  if [[ "$app" == window-formats-test ]]; then
    # ADR-0043 b5, step f: HOME a second after RGB_565 shows, and the return a second after its window goes, as
    # tools/window-contents-check.sh's round plays them; it shows RGB_565 again and finishes itself.
    wait_for "$app" "showing RGB_565" 30 || return 1
    sleep 1
    run input keyevent KEYCODE_HOME
    wait_for "$app" "the window goes" || return 1
    sleep 1
    run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
  fi
  if [[ "$app" == glsurface-pause-test ]]; then
    # HOME a second after its GLSurfaceView drew 30 frames ("ready"), and the return a second after its stop, as
    # apk-run's background and foreground follow the start in tools/glsurface-pause-check.sh.
    wait_for "$app" "I/$app: ready" || return 1
    sleep 1
    run input keyevent KEYCODE_HOME
    wait_for "$app" "I/$app: onStop" || return 1
    sleep 1
    run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
  fi
  if [[ "$app" == windows-return-test ]]; then
    # ADR-0041 gap 4 (b5a): HOME a second after the translucent activity's "ready", and the return a second after the
    # launcher activity's stop, as apk-run's background and foreground follow the start in
    # tools/windows-return-check.sh.
    wait_for "$app" "I/$app: ready" || return 1
    sleep 1
    run input keyevent KEYCODE_HOME
    wait_for "$app" "activity onStop" || return 1
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
  if [[ "$app" == java-test ]]; then
    # Each start as Minato's host plays it (apk-run --lifecycle start,background,foreground): HOME a second after the
    # first focus, the return a second after the state is saved, and the app's own finish a second after its second
    # focus; then a force-stop and a second start the same way, which reads what the first one wrote.
    local start
    for start in 1 2; do
      wait_count "$app" "focus true" $((start * 2 - 1)) || return 1
      sleep 1
      run input keyevent KEYCODE_HOME
      wait_count "$app" onSaveInstanceState "$start" || return 1
      sleep 1
      run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
      wait_count "$app" ": done" "$start" || return 1
      if [[ "$start" == 1 ]]; then
        run am force-stop "$package"
        run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
      fi
    done
  fi
  if [[ "$app" == context-test ]]; then
    wait_for "$app" done || return 1
    run am force-stop "$package"
    run am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$activity" >/dev/null
    local ended=0
    for _ in $(seq 1 60); do
      ended=$(app_lines "$app" | grep -c -E ": done.?$" || true)
      [[ "$ended" -lt 2 ]] || break
      sleep 0.5
    done
    [[ "$ended" -ge 2 ]] || { echo "$app did not end its second start within 30 s." >&2; return 1; }
  fi
  wait_for "$tag" "$last" "$limit" || return 1
  sleep 1
  if [[ -n "$also_tag" ]]; then
    run logcat -d -v tag -s "$app:V" "$also_tag:V" | grep -v '^--------- ' |
      awk -v tag="$also_tag" '$0 !~ "^[VDIWEF]/" tag ": " || /: \[INFO:CONSOLE\(/' >"$RESULTS/$app.logcat" || true
  elif [[ "$tag" == "$app" ]]; then
    app_lines "$app" >"$RESULTS/$app.logcat"
  else
    app_lines "$tag" | grep -F ": $app: " >"$RESULTS/$app.logcat" || true
  fi
  app_lines "$app-device" >"$RESULTS/$app-device.logcat"
  # native-ime-test's and native-jni-ime-test's fixture input methods log their calls and their answers under a tag of
  # their own, and the app's input method manager what it said about the view it was asked to show the keyboard for:
  # shown only.
  if [[ "$app" == native-ime-test || "$app" == native-jni-ime-test ]]; then
    app_lines "$app-reference" >"$RESULTS/$app-reference.logcat"
    app_lines InputMethodManager | grep -i soft >>"$RESULTS/$app-reference.logcat" || true
  fi
  [[ "$app" != obb-test ]] || run rm -rf "/sdcard/Android/obb/${package:?}"
  if [[ "$app" == canvas-test ]]; then
    # The raw pixels it wrote into java.io.tmpdir, its cache folder, before the uninstall takes them.
    device exec-out run-as "$package" cat cache/canvas-test.rgba | gzip -n -9 >"$RESULTS/$app.rgba.gz"
  fi
  if [[ "$app" == views-test* ]]; then
    # The screen a second after "done", while the app shows it, between the bars.
    device exec-out screencap >"$RESULTS/$app.screencap"
    python3 framework/tests/views_compare.py --crop "$RESULTS/$app.screencap" "$RESULTS/$app-device.logcat" \
      "$RESULTS/$app.rgba.gz" "$RESULTS/$app.rows"
  fi
  if [[ "$app" == webview-start-test ]]; then
    # The screen a second after "done", while the empty WebView fills it between the bars: the color it shows, which
    # its software draw (webview-start-test-device) need not be.
    device exec-out screencap >"$RESULTS/$app.screencap"
    python3 -c 'import struct, sys
data = open(sys.argv[1], "rb").read()
width, height = struct.unpack("<II", data[:8])
offset = 16 + 4 * (width * (height // 2) + width // 2)
print("  screen center (RGBA): " + data[offset:offset + 4].hex())' "$RESULTS/$app.screencap"
  fi
  if [[ "$app" == texture-view-test ]]; then
    # The screen a second after "done", while the app shows it, where the views are.
    device exec-out screencap >"$RESULTS/$app.screencap"
    python3 graphics/tests/texture_view_compare.py --crop "$RESULTS/$app.screencap" "$RESULTS/$app.logcat" \
      "$RESULTS/$app.rgba.gz"
  fi
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
  if [[ "$app" == canvas-test || "$app" == views-test* || "$app" == texture-view-test ]] &&
    ! cmp -s "$(expected_at_display "$app" rgba.gz)" "$RESULTS/$app.rgba.gz"; then
    echo "DIFFERS  $app's pixels: $RESULTS/$app.rgba.gz"
    return 1
  fi
  if [[ "$app" == views-test* ]] && ! cmp -s "$(expected_at_display "$app" rows)" "$RESULTS/$app.rows"; then
    echo "DIFFERS  $app's rows: $RESULTS/$app.rows"
    return 1
  fi
  if cmp -s "$(expected_at_display "$app" logcat)" "$RESULTS/$app.logcat"; then
    echo "same     $app"
  else
    echo "DIFFERS  $app"
    diff -u "$(expected_at_display "$app" logcat)" "$RESULTS/$app.logcat" | sed 's/^/    /' || true
    return 1
  fi
  sed 's/^/    /' "$RESULTS/$app-device.logcat"
  [[ "$app" != native-ime-test && "$app" != native-jni-ime-test ]] || sed 's/^/    /' "$RESULTS/$app-reference.logcat"
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
  if [[ "$app" == ringtone-test ]]; then
    trap 'sounds_back; [[ -z "$started" ]] || emulator_stop' EXIT
    sounds_off
  fi
  if [[ "$app" == network-test || "$app" == network-request-test || "$app" == wifi-state-test ]]; then
    trap 'mobile_data_back; [[ -z "$started" ]] || emulator_stop' EXIT
    mobile_data_off
  fi
  if [[ "$app" == cellular-network-test ]]; then
    trap 'wifi_back; [[ -z "$started" ]] || emulator_stop' EXIT
    wifi_off
  fi
  # key-test (ADR-0045 step 7c) runs without an input method too, as Minato has none: its keys reach the EditText
  # as a hardware keyboard's do; so does second-windows-test, whose windows' order an input method's window changed.
  if [[ "$app" == text-test || "$app" == key-test || "$app" == second-windows-test ]]; then
    trap 'input_methods_back; [[ -z "$started" ]] || emulator_stop' EXIT
    input_methods_off
  fi
  # ime-test (ADR-0068 step 2a) types through the fixture input method it installs, and native-ime-test (step 2e)
  # installs one too: the image's input methods and its default are put back at every exit, as for text-test, and the
  # fixture's own is switched off.
  if [[ "$app" == ime-test || "$app" == ime-insets-test || "$app" == ime-insets-test-34 ||
    "$app" == ime-hide-test || "$app" == ime-hide-test-cb ||
    "$app" == ime-fallback-test || "$app" == native-ime-test || "$app" == native-jni-ime-test ||
    "$app" == enter-test || "$app" == enter-test-15 || "$app" == ime-extract-test ]]; then
    trap 'ime_test_off; input_methods_back; [[ -z "$started" ]] || emulator_stop' EXIT
    input_methods=$(run ime list -s | tr -d '\r')
    default_input_method=$(run settings get secure default_input_method | tr -d '\r')
    # An interrupted text-test or key-test leaves the image's input method package disabled, and input_methods_back
    # here has no package to enable again: the run would go through without noticing, so it stops.
    left_off=$(run pm list packages -d | tr -d '\r' | sed 's/^package://' | grep -i inputmethod || true)
    if [[ -n "$left_off" || -z "$input_methods" ]]; then
      echo "An earlier run left the input method off (disabled: ${left_off:-none}, enabled: none or" \
        "${input_methods:-none}). Put it back first: pm default-state --user 0 ${STOCK_INPUT_METHOD%%/*}," \
        "ime enable $STOCK_INPUT_METHOD, ime set $STOCK_INPUT_METHOD." >&2
      exit 1
    fi
  fi
  run_app
  exit $?
fi

if [[ -n "$art" ]]; then
  jar=${ART_TEST_JARS:-build/art/test}/$art.jar
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
  # The program's process id, written before dalvikvm64 replaces the shell, picks its own lines out of logcat.
  run logcat -c
  run "cd $DEVICE_DIR && echo \$\$ >$DEVICE_DIR/.pid && exec dalvikvm64 -Xint -Djava.library.path=$DEVICE_DIR \
-cp $DEVICE_DIR/$art.jar Main" >"$RESULTS/$art.out" 2>"$RESULTS/$art.err" || status=$?
  pid=$(run cat "$DEVICE_DIR/.pid" | tr -d '\r') || pid=""
  run logcat -d -v tag --pid="$pid" -s nativeloader:V |
    grep -E '^[VDIWEF]/nativeloader: (Load .* ns |Configuring )' >"$RESULTS/$art.nativeloader" || true
  run "rm -rf $DEVICE_DIR/* $DEVICE_DIR/.[!.]*" >/dev/null
  namespaces=same
  if [[ -f "fixtures/expected/$art.nativeloader" ]] &&
    ! cmp -s "fixtures/expected/$art.nativeloader" "$RESULTS/$art.nativeloader"; then
    namespaces=""
  fi
  if [[ $status -eq 0 && -n "$namespaces" ]] && cmp -s "fixtures/expected/$art.out" "$RESULTS/$art.out"; then
    echo "same     $art"
    tail -n 1 "$RESULTS/$art.err" | sed 's/^/    /'
    exit 0
  fi
  echo "DIFFERS  $art: exit status $status"
  diff -u "fixtures/expected/$art.out" "$RESULTS/$art.out" | sed 's/^/    /' || true
  if [[ -z "$namespaces" ]]; then
    diff -u "fixtures/expected/$art.nativeloader" "$RESULTS/$art.nativeloader" | sed 's/^/    /' || true
  fi
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
