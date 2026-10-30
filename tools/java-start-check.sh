#!/usr/bin/env bash
# Checks ADR-0039 c5 in the Simulator or, with --device, on the iPhone (step c6): a Java app starts in game mode from
# the library, as a NativeActivity app does. app-start-test (ADR-0039's test 1), context-test (step c1's, started
# twice over the same data), asset-ndk-test (ADR-0041 decision 7's, whose JNI library System.loadLibrary loads),
# jni-lib-test (step c5.1's, which looks at its JNI library in nativeLibraryDir), jnigraphics-test (ADR-0041 a5's,
# whose JNI library links libjnigraphics, which the app's guest system has from build/hwui/lib, and reads and writes
# bitmaps through AndroidBitmap_* and decodes the APK's picture.png through AImageDecoder), surface-test
# (ADR-0043 c4 (d)'s, a GLSurfaceView whose surface is a layer of its own beside the window's), palette-test
# (ADR-0043 f)'s, whose app colors are Monet's, from the overlays of the guest's /data/resource-cache), binder-test
# (ADR-0043 d2b)'s, whose Binder, AIDL-style stub, IBinder and ResultReceivers are its own), strictmode-test (ADR-0039
# step c2d3's, StrictMode's defaults at the bind and Build.getSerial) and obb-test (ADR-0049 test 8's, which reads its
# main and its patch OBB, picked after it as a unit of its own, which the library gives to it by its name; on the iPhone
# the unit is copied into incoming/ with devicectl), splits-test and isolated-splits-test (ADR-0048 c's bundletool sets,
# picked as units too; splits-test's greeting on a device whose language is not English is the base's, as Android's
# would be for it),
# packs-test (ADR-0050 test 2's, a bundletool .apks of tools/build-packs-test.sh with a fast-follow and an on-demand
# pack, whose slices the library keeps and the start lays out in local_testing, read-only, with the meta-data
# local_testing_dir; its lines are the Mac's, fixtures/expected/packs-test-packs.logcat, with that set's sizes) and
# pad-test (ADR-0050 test 3's, the default set of tools/build-pad-test.sh, built with Google's Play libraries, whose
# asset-delivery serves both packs from those slices; skipped without the libraries, which only the user's go fetches)
# are imported and tapped in the library,
# and each one's lines must be Android 15's (fixtures/expected/<app>.logcat) as the Mac's harness compares
# them: app-start-test's System.out lines (framework/tests/app_process_run.py), which SessionCheck's guest-log request
# sends to tmp/session-check/guest-out, with ART's line in its guest log that names its collector, CMS (ADR-0033
# decision 5), and the lines of the others' own tags in their guest logs, of both of context-test's starts
# (framework/tests/java_app_run.py). An app process never ends by itself (step c2b): 2 s after
# the app's last line the script ends Minato, as tools/android-reference.sh stops the app on the AVD after that line,
# and it prints the time from the tap to that line. On the iPhone, java-test (ADR-0043 step c1) follows: twice the
# tap, its window's focus, Home (Settings comes to the front), the return to Minato and the app's own finish, as on
# the AVD, whose display is the iPhone 15 Pro Max's; it prints the time from the tap to its first onResume, ADR-0039
# decision 1's start time, from the app's own clock when it logs it. The Debug build answers through
# app/Sources/Session/SessionCheck.swift; on the iPhone devicectl sends the requests and fetches the events and files,
# as tools/session-check.sh does, the app must stay unlocked, and the times count the fetches' delay too. The test
# apps leave the library before the run, which a killed run may have left them in, and after it; in the Simulator the
# library is then as before. surface-test's sizes are the screen's: in the Simulator Android's 1290 x 2796 become the
# Simulator's, and test 4's Button, centered, keeps its size at that screen's center.
# activity-flow-test (ADR-0039 step b3.5b2-1's) is one of those apps too: its activities start one another in
# the app's one task, the session's window going to the top one, and the host's layer is not opaque under
# the windows of Second and Third, which have alpha, and opaque again each time Main has the window back.
# result-flow-test (step b3.5b2-2a's) is one too: its activities return results to the ones that started them.
# launch-mode-test (step b3.5b2-2b's) is one too: its starts follow the launch modes and the task's flags.
# crash-test (ADR-0039, the main thread's uncaught exception) is one too: Android's handlers end it after its "done",
# and the program must end with 137, the kill of itself, and Minato go back to the library without a notice.
# sqlite-test (the compatibility plan's gap 7) is one too: its own SQLite databases, whose files take the runtime's
# locks inside the app's sandbox on the iPhone.
# timezone-test (the compatibility plan's gap 21) is one too: the tzdata module's files in the app's /apex, and the
# zones that libcore, java.time and ICU read from them.
# package-installer-test (the Unity test APK resolve list, batch E) is one too: the package installer's sessions and the
# refusal of a new one, and the exit reasons.
# media-absent-test (the Unity test APK gap sort, category 2) is one too: android.media's classes for hardware Minato lacks.
# media-extractor-test (ADR-0060 e2d parts 2a and 2b) is one too: the NDK's AMediaExtractor and
# android.media.MediaExtractor on Minato's own mp4 files.
# media-codec-audio-test (ADR-0060 e2f) is one too: MediaCodec's AAC decoder on the host's AudioToolbox.
# views-test (ADR-0043's test 1: views, a TextView and a Button drawn in software) runs on the iPhone only, whose screen
# its views' bounds are (the Simulator's is another): this check plays no touches, so the app clicks its Button with
# performClick 5 s after its label's change, and its device line says so (the Mac's tools/views-check.sh and the AVD
# tap it).
# vk-ahb-test (ADR-0058 e1), decision 7) runs on the iPhone only, a NativeActivity app of tools/build-fixtures.sh whose
# Vulkan needs MoltenVK, which the Simulator lacks: VK_ANDROID_external_memory_android_hardware_buffer's import and
# export on the iPhone's IOSurfaces. media-codec-test and media-window-test (ADR-0060 e2c) run on the iPhone only too,
# against Android 15's lines, and media-bench-test, Minato's measurement at 1080p and 4K (its lines shown, Home twice).
# Usage: tools/java-start-check.sh [--device] [simulator or iPhone name or identifier]
# Optional environment:
#   APP        a built Minato.app (Debug) to install in place of tools/sim.sh's or, with --device, tools/deploy.sh's
#              build; on the iPhone it goes over the installed Minato, whose data stays, and the script prints how long
#              that took and what the certificate hand-over says
#   LOGS       a folder that keeps each start's guest log and stdout, <app>-<start>.log and .out, and a screenshot
#              <app>-<start>.png of a start that failed, which shows an alert's text
#   KEYS       set: only key-test runs (ADR-0068 step 7c2); the typing is the operator's, as simctl sends no keys:
#              the script waits for the app's "channel: ready", says what to type, and waits for its "done" (three key
#              ups). Type 2, b and Delete with the Mac keyboard into the Simulator's window; the iOS Simulator tool
#              reaches it too, but sends hardware keys for Delete, Return and the arrows only, and its text action is
#              a paste (Cmd+V). Up to "channel: ready" the log must be Android 15's; the typed keys are not compared
#              with Android's, since they may be any, but each must come from the hardware keyboard, device 1000, where
#              Android's adb input is the virtual keyboard, -1.
#   IME        set: only ime-test runs (ADR-0068 step 2c); the typing is the operator's on UIKit's keyboard, which must
#              show (in the Simulator I/O > Keyboard > Connect Hardware Keyboard off, on the iPhone no hardware
#              keyboard connected): the script waits for the app's "showSoftInput true" and the session's "keyboard
#              show", says what to type, and waits for the editor's action. Up to "showSoftInput true" the log must
#              be Android 15's; then come the host's records: h and i committed, the delete as
#              deleteSurroundingText 1,0, and Go as performEditorAction 2, leaving "h"
#   JAVA_TEST  set: java-test's round runs in the Simulator too, where its layout line shows the Simulator's screen
#   ONLY       the apps to start, by name and space separated, such as ONLY=java-test for a sitting with java-test
#              alone; java-test or a Godot APK among them runs in the Simulator too
#   GODOT_TEST no: test 2's Godot APKs of tools/build-godot.sh (ADR-0042 b)), godot-bench-vulkan and godot-bench-gles,
#              stay out; else they run too: their "godot-bench: " lines must be Android 15's
#              (fixtures/expected/godot-bench-*.logcat) and their run must have frames and no failures, as
#              tools/godot-java-check.sh checks them on the Mac, which also taps the bench for its touch line; this
#              check plays no touches, so it leaves that line out.
#              godot-bench-vulkan runs on the iPhone only: MoltenVK has no Simulator slice (ADR-0030, decision 8), and
#              Godot draws with GLES there, which its bench counts as a failure. On by default since 2026-10-07,
#              when both ran green on the iPhone (vulkan 528 and gles 525 frames, 0 failures) and gles in the
#              Simulator.
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
ON_DEVICE=no
if [[ "${1:-}" == --device ]]; then
  ON_DEVICE=yes
  shift
fi
if [[ "$ON_DEVICE" == yes ]]; then
  read -r DEVICE_ID _ _ _ < <(python3 tools/find-device.py "${1:-}") || true
  if [[ -z "${DEVICE_ID:-}" ]]; then
    echo "No paired iPhone found. Connect it by cable and trust this Mac." >&2
    exit 1
  fi
  python3 tools/require-device.py "$DEVICE_ID" || exit 1 # paired, by cable or over the local network
else
  read -r SIM_UDID _ < <(python3 tools/find-simulator.py "${1:-}") || true
  if [[ -z "${SIM_UDID:-}" ]]; then
    echo "No matching iPhone simulator found." >&2
    exit 1
  fi
fi
# Each devicectl call on the iPhone checks first that it is still reachable and stops the run when it is not;
# the message goes to the script's stderr, kept as 3, also where the call's output goes to /dev/null.
exec 3>&2
on_device() {
  python3 tools/require-device.py "$DEVICE_ID" 2>&3 || exit 1
  xcrun devicectl device "$@"
}

# The test apps, "<app> <package>", and where each one's last line appears.
APPS=("app-start-test io.github.kisame76.minato.test.appstart" "context-test io.github.kisame76.minato.test.context"
      "asset-ndk-test io.github.kisame76.minato.test.assetndk" "jni-lib-test io.github.kisame76.minato.test.jnilib"
      "jnigraphics-test io.github.kisame76.minato.test.jnigraphics"
      "surface-test io.github.kisame76.minato.test.surface" "palette-test io.github.kisame76.minato.test.palette"
      "binder-test io.github.kisame76.minato.test.binder" "strictmode-test io.github.kisame76.minato.test.strictmode"
      "activity-flow-test io.github.kisame76.minato.test.activityflow"
      "result-flow-test io.github.kisame76.minato.test.resultflow"
      "launch-mode-test io.github.kisame76.minato.test.launchmode" "crash-test io.github.kisame76.minato.test.crash"
      "sqlite-test io.github.kisame76.minato.test.sqlite"
      "timezone-test io.github.kisame76.minato.test.timezone"
      "package-installer-test io.github.kisame76.minato.test.packageinstaller"
      "media-absent-test io.github.kisame76.minato.test.mediaabsent"
      "media-extractor-test io.github.kisame76.minato.test.mediaextractor"
      "media-codec-audio-test io.github.kisame76.minato.test.mediacodecaudio")
JAVA_TEST=${JAVA_TEST:-}
ONLY=${ONLY:-}
[[ "$ON_DEVICE" == no ]] || JAVA_TEST=yes
[[ " $ONLY " != *" java-test "* ]] || JAVA_TEST=yes
# obb-test's OBB comes as a unit folder of its own, which this check makes in the Simulator only.
APPS+=("obb-test io.github.kisame76.minato.test.obb")
# packs-test comes as a bundletool .apks, which this check imports in the Simulator only, as it does obb-test's OBB.
[[ "$ON_DEVICE" == yes ]] || APPS+=("packs-test io.github.kisame76.minato.test.packs")
APPS+=("splits-test io.github.kisame76.minato.test.splits")
APPS+=("isolated-splits-test io.github.kisame76.minato.test.isolated")
# pad-test too, when Google's Play libraries are there to build it with (ADR-0050 c)).
if /usr/bin/python3 tools/fetch-play-libraries.py >/dev/null 2>&1; then
  APPS+=("pad-test io.github.kisame76.minato.test.pad")
else
  echo "skipped: pad-test: no Play libraries (tools/fetch-play-libraries.py --fetch, on the user's go)"
fi
[[ -z "$JAVA_TEST" ]] || APPS+=("java-test io.github.kisame76.minato.test.javatest")
GODOT_TEST=${GODOT_TEST-yes}
[[ "$GODOT_TEST" != no ]] || GODOT_TEST=""
[[ " $ONLY " != *" godot-bench-"* ]] || GODOT_TEST=yes
# The Vulkan APK on the iPhone only: the Simulator has no MoltenVK (ADR-0030, decision 8).
[[ -z "$GODOT_TEST" || "$ON_DEVICE" == no ]] || APPS+=("godot-bench-vulkan io.github.kisame76.minato.godotbench.vulkan")
[[ -z "$GODOT_TEST" ]] || APPS+=("godot-bench-gles io.github.kisame76.minato.godotbench.gles")
[[ "$ON_DEVICE" == no ]] || APPS+=("vk-ahb-test io.github.kisame76.minato.test.vkahb")
# ADR-0060 e2c's decoder apps on the iPhone only, on its hardware VideoToolbox (the Mac has checks of their own).
[[ "$ON_DEVICE" == no ]] || APPS+=("media-codec-test io.github.kisame76.minato.test.mediacodec")
[[ "$ON_DEVICE" == no ]] || APPS+=("media-bench-test io.github.kisame76.minato.test.mediabench")
[[ "$ON_DEVICE" == no ]] || APPS+=("media-window-test io.github.kisame76.minato.test.mediawindow")
# ADR-0043's test 1 on the iPhone only: its views' bounds are the iPhone 15 Pro Max's screen's, as the AVD's are.
[[ "$ON_DEVICE" == no ]] || APPS+=("views-test io.github.kisame76.minato.test.views")
# KEYS: key-test alone (ADR-0068 step 7c2), whose keys come from the operator's hands.
if [[ -n "${KEYS:-}" ]]; then
  APPS+=("key-test io.github.kisame76.minato.test.key")
  ONLY=key-test
fi
# IME: ime-test alone (ADR-0068 step 2c), whose text comes from the operator's taps on UIKit's keyboard.
if [[ -n "${IME:-}" ]]; then
  APPS+=("ime-test io.github.kisame76.minato.test.ime")
  ONLY=ime-test
fi
# Whether app $1 starts in this run: each one, or ONLY's, which must be the check's.
wanted() { [[ -z "${ONLY// /}" || " $ONLY " == *" $1 "* ]]; }
if [[ -n "${ONLY// /}" ]]; then
  names=" ${APPS[*]%% *} "
  for name in $ONLY; do
    if [[ "$names" != *" $name "* ]]; then
      echo "Not an app of this check: $name" >&2
      exit 1
    fi
  done
  chosen=()
  for app in "${APPS[@]}"; do
    if wanted "${app%% *}"; then chosen+=("$app"); fi
  done
  APPS=("${chosen[@]}")
fi
last_line() {
  case "$1" in
    app-start-test) echo "guest-out ^app-start-test: MessageDigest " ;; # app_process_run.LAST
    # Godot's run line, with its padded tag (framework/tests/godot_java_run.py's LAST).
    godot-bench-*) echo "guest-log ^I/godot *: run [^:]+: [0-9]+ frames, [0-9]+ failures$" ;;
    vk-ahb-test) echo "guest-log ^I/vk-ahb-test: android_main returns$" ;;   # a NativeActivity's end
    *) echo "guest-log ^I/$1: done$" ;;                                 # java_app_run's "<app>: done"
  esac
}
# The APK of app $1: the Godot APKs are tools/build-godot.sh's, which this check does not make; packs-test's set is
# tools/build-packs-test.sh's, pad-test's tools/build-pad-test.sh's.
apk() {
  case "$1" in
    godot-bench-*) echo "build/godot/$1.apk" ;;
    packs-test) echo "build/packs-test/packs-test.apks" ;;
    pad-test) echo "build/pad-test/pad-test.apks" ;;
    splits-test|isolated-splits-test) echo "build/$1/$1.apks" ;;
    vk-ahb-test) echo "build/fixtures/apks/$1.apk" ;;
    *) echo "build/$1/$1.apk" ;;
  esac
}
# Whether app $1 is one of this run's.
listed() { [[ " ${APPS[*]%% *} " == *" $1 "* ]]; }
for app in "${APPS[@]}"; do
  case "${app%% *}" in
    godot-bench-*)
      if [[ ! -f "$(apk "${app%% *}")" ]]; then
        echo "No $(apk "${app%% *}"): run tools/build-godot.sh first." >&2
        exit 1
      fi
      ;;
    packs-test) tools/build-packs-test.sh >/dev/null ;;
    pad-test) tools/build-pad-test.sh >/dev/null ;;
    splits-test|isolated-splits-test) tools/build-splits-test.sh "${app%% *}" >/dev/null ;;
    vk-ahb-test) tools/build-fixtures.sh >/dev/null ;;
    *) tools/build-java-test-app.sh "${app%% *}" >/dev/null ;;
  esac
done
now() { python3 -c 'import time; print(time.time())'; }
seconds() { python3 -c 'import sys; print(f"{float(sys.argv[2]) - float(sys.argv[1]):.1f}")' "$1" "$2"; }

WORK=$(mktemp -d)
if [[ "$ON_DEVICE" == yes ]]; then
  if [[ -n "${APP:-}" ]]; then
    # Over the installed Minato: never with --remove-existing-content, which would empty its container.
    started=$(now)
    on_device install app --device "$DEVICE_ID" -q "$APP" >/dev/null
    echo "note  installed over Minato in $(seconds "$started" "$(now)") s"
    # As tools/deploy.sh does after an install; only when the app does not have the certificate yet.
    tools/hand-over-certificate.sh "$DEVICE_ID" 2>&1 | sed 's/^/note  /' \
      || echo "note  the signing certificate was not handed over"
  else
    INSTALL_ONLY=1 tools/deploy.sh "$DEVICE_ID" >/dev/null
  fi
  # On the iPhone a copy of the files the check uses, which the helpers below fetch and send.
  CONTAINER="$WORK/container"
else
  if [[ -n "${APP:-}" ]]; then
    xcrun simctl boot "$SIM_UDID" 2>/dev/null || true # already booted is fine
    xcrun simctl install "$SIM_UDID" "$APP"
  else
    INSTALL_ONLY=1 tools/sim.sh --headless "$SIM_UDID" >/dev/null
  fi
  CONTAINER=$(xcrun simctl get_app_container "$SIM_UDID" "$BUNDLE_ID" data)
fi
SUPPORT_REL="Library/Application Support"
CHECK_REL="tmp/session-check"
SUPPORT="$CONTAINER/$SUPPORT_REL"
CHECK="$CONTAINER/$CHECK_REL"
EVENTS="$CHECK/events"
REQUEST="$SUPPORT/launch-request.json"
# The app that comes to the front for java-test's Home.
AWAY=com.apple.Preferences

devicectl_copy() {
  local direction=$1
  shift
  on_device copy "$direction" --device "$DEVICE_ID" --domain-type appDataContainer \
    --domain-identifier "$BUNDLE_ID" -q "$@" >/dev/null 2>&1
}
# Starts app $1, Minato when none, or brings it to the front when it runs.
launch() {
  if [[ "$ON_DEVICE" == yes ]]; then
    on_device process launch --device "$DEVICE_ID" -q "${1:-$BUNDLE_ID}" >/dev/null
  else
    xcrun simctl launch "$SIM_UDID" "${1:-$BUNDLE_ID}" >/dev/null
  fi
}
terminate() {
  if [[ "$ON_DEVICE" == yes ]]; then
    on_device info processes --device "$DEVICE_ID" -q -j "$WORK/processes.json" >/dev/null 2>&1 || return 0
    for pid in $(python3 -c 'import json,sys; print(" ".join(str(p["processIdentifier"]) for p in json.load(open(sys.argv[1]))["result"]["runningProcesses"] if p.get("executable", "").endswith("/Minato.app/Minato")))' "$WORK/processes.json"); do
      on_device process terminate --device "$DEVICE_ID" -q --kill --pid "$pid" >/dev/null 2>&1 || true
    done
  else
    xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  fi
}
# Every request of app/Sources/Session/SessionCheck.swift, and the events, as tools/session-check.sh lists them.
REQUESTS="way tap quit confirm orientation unanswered forget-hint crash start-failed tone fps no-guest guest-log extra-session turn-library turn-session audio-disconnect status-bar end events"
# On the iPhone: the requests as the script has them, events included, go into the app's folder. A request the script
# does not have goes as an empty file, which the app takes for none: devicectl cannot delete a file.
send_requests() {
  [[ "$ON_DEVICE" == yes ]] || return 0
  rm -rf "$WORK/sent"
  mkdir "$WORK/sent"
  for name in $REQUESTS; do
    if [[ -f "$CHECK/$name" ]]; then cp "$CHECK/$name" "$WORK/sent/"; else : > "$WORK/sent/$name"; fi
  done
  devicectl_copy to --source "$WORK/sent" --destination "$CHECK_REL"
}
# On the iPhone: the folder as the app has it, without the requests it took or emptied. False when there is none.
fetch_requests() {
  [[ "$ON_DEVICE" == yes ]] || { [[ -d "$CHECK" ]]; return; }
  rm -rf "$WORK/fetched"
  mkdir "$WORK/fetched"
  devicectl_copy from --source "$CHECK_REL" --destination "$WORK/fetched" || return 1
  find "$WORK/fetched" -type f -empty ! -name events -delete
  rm -rf "$CHECK"
  mv "$WORK/fetched" "$CHECK"
}
fetch_events() {
  [[ "$ON_DEVICE" == yes ]] || return 0
  if devicectl_copy from --source "$CHECK_REL/events" --destination "$WORK/events"; then mv "$WORK/events" "$EVENTS"; fi
}
# Whether the container has the file $1, a path in it; on the iPhone it is fetched.
fetch() {
  if [[ "$ON_DEVICE" == yes ]]; then
    mkdir -p "$(dirname "$CONTAINER/$1")"
    devicectl_copy from --source "$1" --destination "$CONTAINER/$1" || { rm -f "$CONTAINER/$1"; return 1; }
  else
    [[ -f "$CONTAINER/$1" ]]
  fi
}
# A screenshot into LOGS as $1.png, for a start that failed.
screenshot() {
  [[ -n "${LOGS:-}" ]] || return 0
  mkdir -p "$LOGS"
  if [[ "$ON_DEVICE" == yes ]]; then
    on_device capture screenshot --device "$DEVICE_ID" -q --destination "$LOGS/$1.png" >/dev/null 2>&1 || true
  else
    xcrun simctl io "$SIM_UDID" screenshot "$LOGS/$1.png" >/dev/null 2>&1 || true
  fi
}

# The test apps go through the library, as session-check.sh removes its own, then what is left of them. Fails while
# the index still names one.
remove_test_apps() {
  terminate
  # obb-test's OBB unit (pick_obb) and packs-test's and pad-test's sets (pick_set), which a run that ended before the
  # library took them leaves, and the launch would import.
  [[ "$ON_DEVICE" == yes ]] || rm -rf "${SUPPORT:?}/incoming/obb-test-obb" "${SUPPORT:?}/incoming/obb-test-obb.partial" \
    "${SUPPORT:?}/incoming/packs-test-apks" "${SUPPORT:?}/incoming/packs-test-apks.partial" \
    "${SUPPORT:?}/incoming/pad-test-apks" "${SUPPORT:?}/incoming/pad-test-apks.partial"
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  for app in "${APPS[@]}"; do echo "${app#* }"; done > "$CHECK/end"
  send_requests
  launch || true
  for _ in $(seq 1 50); do fetch_requests || break; sleep 0.2; done
  terminate
  rm -rf "$CHECK"
  if [[ "$ON_DEVICE" == no ]]; then
    rm -f "$REQUEST"
    for app in "${APPS[@]}"; do
      rm -rf "${SUPPORT:?}/apps/${app#* }" "${SUPPORT:?}/data/${app#* }" "${CONTAINER:?}/Library/Caches/guest/${app#* }"
    done
  fi
  fetch "$SUPPORT_REL/library.json" || true
  python3 - "$SUPPORT/library.json" "${APPS[@]#* }" <<'EOF'
import json
import sys
try:
    ids = {entry["id"] for entry in json.load(open(sys.argv[1]))["entries"]}
except FileNotFoundError:
    sys.exit(0)
left = [package for package in sys.argv[2:] if package in ids]
if left:
    print(f"still in the library: {' '.join(left)}")
    sys.exit(1)
EOF
}

# A killed run leaves its apps in the library. Saved with them, the index would bring them back at the end without
# their files, and an import of the same APK keeps such an entry, whose tap fails.
mkdir -p "$SUPPORT/incoming"
remove_test_apps
had_data=no
if [[ "$ON_DEVICE" == no ]]; then
  cp "$SUPPORT/library.json" "$WORK/library.json.saved" 2>/dev/null || true
  [[ -d "$SUPPORT/data" ]] && had_data=yes
fi

restore() {
  remove_test_apps || true
  if [[ "$ON_DEVICE" == no ]]; then
    if [[ "$had_data" == no ]]; then rmdir "$SUPPORT/data" 2>/dev/null || true; fi
    if [[ -f "$WORK/library.json.saved" ]]; then cp "$WORK/library.json.saved" "$SUPPORT/library.json"; else rm -f "$SUPPORT/library.json"; fi
  fi
  if [[ -n "${LOGS:-}" ]]; then
    mkdir -p "$LOGS"
    cp "$WORK"/*.log "$WORK"/*.out "$LOGS/" 2>/dev/null || true
  fi
  rm -rf "${WORK:?}"
}
trap restore EXIT

failures=0
ok() { echo "ok    $1"; }
fail() {
  echo "FAIL  $1"
  failures=$((failures + 1))
}

# The events the app appended, "<seconds> <pid> <event>"; `seen` counts the lines already taken. Waits up to $2
# seconds for the next event that matches the extended regex $1 and puts it into EVENT, as session-check.sh does.
seen=0
EVENT=""
await_event() {
  local pattern=$1 end=$((SECONDS + ${2:-20})) found
  while true; do
    fetch_events
    found=$(tail -n +$((seen + 1)) "$EVENTS" 2>/dev/null | grep -n -E -m 1 "^[0-9.]+ [0-9]+ ($pattern)" || true)
    if [[ -n "$found" ]]; then
      seen=$((seen + ${found%%:*}))
      EVENT=${found#*:}
      return 0
    fi
    (( SECONDS < end )) || break
    sleep 0.2
  done
  EVENT=""
  return 1
}
# Field $1 of app $2's entry.
field() {
  fetch "$SUPPORT_REL/library.json" || true
  python3 -c 'import json,sys; e={e["id"]:e for e in json.load(open(sys.argv[1]))["entries"]}.get(sys.argv[2]); print(None if e is None else e.get(sys.argv[3]))' \
    "$SUPPORT/library.json" "$2" "$1" 2>/dev/null || echo None
}

# Waits up to $3 seconds for a line of the start's file $1, guest-log or guest-out, that matches the extended regex
# $2, fetched on the iPhone, and sets SEEN to the time it was first seen. Fails at once when the start failed or the
# program ended without it. The events come first: a program that ends right after its last line, as java-test after
# "done", has that line in the file fetched after them.
SEEN=""
await_line() {
  local file=$1 pattern=$2 end=$((SECONDS + $3)) ended
  while (( SECONDS < end )); do
    fetch_events
    ended=no
    if tail -n +$((seen + 1)) "$EVENTS" | grep -E '^[0-9.]+ [0-9]+ (could not start|program ended) ' >/dev/null; then
      ended=yes
    fi
    fetch "$CHECK_REL/$file" || true
    if grep -q -E "$pattern" "$CHECK/$file" 2>/dev/null; then
      SEEN=$(now)
      return 0
    fi
    [[ "$ended" == no ]] || return 1
    sleep 0.2
  done
  return 1
}

# The tap on package $2, the $3rd start of app $1: up to game mode with the guest log on, whose files the start then
# has; TAP is the tap's time, on the app's clock. False when it did not get there.
TAP=""
begin_start() {
  local app=$1 package=$2 round=$3
  echo "== $app, start $round"
  terminate
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  [[ "$ON_DEVICE" == yes ]] || rm -f "$REQUEST"
  : > "$WORK/$app-$round.log"
  : > "$WORK/$app-$round.out"
  : > "$EVENTS"
  seen=0
  echo "$package" > "$CHECK/tap"
  echo yes > "$CHECK/guest-log"
  send_requests
  launch
  if ! await_event "decision .*" 60; then
    fail "  no tap in the library"
    screenshot "$app-$round"
    return 1
  fi
  TAP=${EVENT%% *}
  # The game mode process truncates the guest log's files when it takes the request.
  if ! await_event "process gameMode $package.*" 60 || ! await_event "guest log on" 60; then
    fail "  no game mode after the tap"
    sed 's/^/      | /' "$EVENTS"
    screenshot "$app-$round"
    return 1
  fi
}
# The start's files, kept as $WORK/$1-$2.log and .out, after Minato's end.
end_start() {
  terminate
  for file in guest-log guest-out; do fetch "$CHECK_REL/$file" || true; done
  cp "$CHECK/guest-log" "$WORK/$1-$2.log" 2>/dev/null || : > "$WORK/$1-$2.log"
  cp "$CHECK/guest-out" "$WORK/$1-$2.out" 2>/dev/null || : > "$WORK/$1-$2.out"
}
failed_line() {
  fail "  $1"
  screenshot "$2"
  sed 's/^/      | /' "$EVENTS"
  tail -n 40 "$CHECK/guest-log" 2>/dev/null | sed 's/^/      | /' || true
}

# One start of app $1 with package $2, the $3rd: the tap, game mode, the app's lines up to its last one, and Minato's
# end 2 s later.
run_start() {
  local app=$1 package=$2 round=$3 file pattern
  read -r file pattern <<< "$(last_line "$app")"
  if begin_start "$app" "$package" "$round"; then
    if await_line "$file" "$pattern" 300; then
      ok "  its last line, $(seconds "$TAP" "$SEEN") s after the tap"
      sleep 2
    else
      failed_line "no last line" "$app-$round"
    fi
  fi
  end_start "$app" "$round"
}

# java-test's $1st start, as on the AVD (ADR-0043 step c1): the tap, its window's focus, Home, the return and its own
# finish. The time to its first onResume comes from the app's own "onResume at" line when it has one.
run_lifecycle() {
  local round=$1 package="" resumed at app
  # java-test's package by its name: the Godot APKs come after it in APPS.
  for app in "${APPS[@]}"; do
    [[ "${app%% *}" != java-test ]] || package=${app#* }
  done
  if begin_start java-test "$package" "$round"; then
    if ! await_line guest-log "^I/java-test: onResume$" 300; then
      failed_line "no onResume" "java-test-$round"
    else
      resumed=$SEEN
      # The app's clock, which is the tap's too, in the line it logs right after.
      at=""
      await_line guest-log "^I/java-test-device: onResume at " 10 \
        && at=$(sed -n -E 's/^I\/java-test-device: onResume at ([0-9]+)$/\1/p' "$CHECK/guest-log" | head -n 1)
      if [[ -n "$at" ]]; then
        ok "  onResume $(python3 -c 'import sys; print(f"{float(sys.argv[2]) / 1000 - float(sys.argv[1]):.2f}")' "$TAP" "$at") s after the tap, by the app's clock"
      else
        ok "  onResume seen $(seconds "$TAP" "$resumed") s after the tap"
      fi
      if ! await_line guest-log "^I/java-test: focus true$" 60; then
        failed_line "no focus" "java-test-$round"
      else
        launch "$AWAY"
        if ! await_line guest-log "^I/java-test: onSaveInstanceState$" 60; then
          failed_line "no onStop after Home" "java-test-$round"
        else
          launch
          if await_line guest-log "^I/java-test: done$" 60; then
            ok "  done, after Home and the return, seen $(seconds "$TAP" "$SEEN") s after the tap"
            sleep 2
          else
            failed_line "no done after the return" "java-test-$round"
          fi
        fi
      fi
    fi
  fi
  end_start java-test "$round"
}

# media-bench-test's start (ADR-0060 e2c): its measurement lines, and twice Home a few seconds into a background run
# and the return 8 s later, while it decodes 4K into an RGBA and then a YUV reader; its lines are shown, as they are
# Minato's measurement, which has no Android reference.
run_media_bench() {
  local package=io.github.kisame76.minato.test.mediabench kind
  if begin_start media-bench-test "$package" 1; then
    for kind in rgba yuv; do
      if ! await_line guest-log "^I/media-bench-test: background $kind: decoding$" 300; then
        failed_line "no background $kind run" "media-bench-test-1"
        break
      fi
      sleep 4
      launch "$AWAY"
      sleep 8
      launch
      ok "  Home and the return during the background $kind run"
    done
    if await_line guest-log "^I/media-bench-test: done$" 120; then
      ok "  done, $(seconds "$TAP" "$SEEN") s after the tap"
      sleep 2
    else
      failed_line "no done" "media-bench-test-1"
    fi
  fi
  end_start media-bench-test 1
  grep -E "^I/media-bench-test: " "$WORK/media-bench-test-1.log" | sed 's/^/note  /'
  if grep -q -E "^I/media-bench-test: (failed|.*no codec)" "$WORK/media-bench-test-1.log" ||
     grep -q -E "^I/media-bench-test: background .*images in the last 3 s no" "$WORK/media-bench-test-1.log"; then
    failures=$((failures + 1))
  fi
}

# Import the apps; the Simulator signs a ready app's code ad hoc at its first tap.
terminate
rm -rf "$CHECK"
mkdir -p "$CHECK"
[[ "$ON_DEVICE" == yes ]] || rm -f "$REQUEST"
: > "$EVENTS"
for app in "${APPS[@]}"; do
  # packs-test's and pad-test's sets come as unit folders of their own (pick_set), as the library takes only APKs
  # lying flat here.
  case "${app%% *}" in packs-test|pad-test|splits-test|isolated-splits-test) continue ;; esac
  if [[ "$ON_DEVICE" == yes ]]; then
    devicectl_copy to --source "$(apk "${app%% *}")" --destination "$SUPPORT_REL/incoming/${app%% *}.apk"
  else
    cp "$(apk "${app%% *}")" "$SUPPORT/incoming/"
  fi
done
# obb-test's main and patch OBB (ADR-0049 test 8): a unit of its own after the APKs, a folder that appears whole, as
# "+" leaves a pick; the library gives it to obb-test, which it imports first, by its name. remove_test_apps took any
# left over.
pick_obb() {
  if [[ "$ON_DEVICE" == yes ]]; then
    rm -rf "$WORK/unit-obb" && mkdir -p "$WORK/unit-obb"
    cp "fixtures/apps/obb-test/main.1.$1.obb" "fixtures/apps/obb-test/patch.1.$1.obb" "$WORK/unit-obb/"
    devicectl_copy to --source "$WORK/unit-obb" --destination "$SUPPORT_REL/incoming/obb-test-obb"
    return
  fi
  local unit="${SUPPORT:?}/incoming/obb-test-obb"
  mkdir "$unit.partial"
  cp "fixtures/apps/obb-test/main.1.$1.obb" "fixtures/apps/obb-test/patch.1.$1.obb" "$unit.partial/"
  mv "$unit.partial" "$unit"
}
if wanted obb-test; then pick_obb io.github.kisame76.minato.test.obb; fi
# packs-test's and pad-test's sets (ADR-0050 tests 2 and 3), each a unit folder that appears whole, as "+" leaves a
# pick.
pick_set() {
  if [[ "$ON_DEVICE" == yes ]]; then
    rm -rf "$WORK/unit-$1" && mkdir -p "$WORK/unit-$1"
    cp "$(apk "$1")" "$WORK/unit-$1/"
    devicectl_copy to --source "$WORK/unit-$1" --destination "$SUPPORT_REL/incoming/$1-apks"
    return
  fi
  local unit="${SUPPORT:?}/incoming/$1-apks"
  mkdir "$unit.partial"
  cp "$(apk "$1")" "$unit.partial/"
  mv "$unit.partial" "$unit"
}
if wanted packs-test && [[ "$ON_DEVICE" == no ]]; then pick_set packs-test; fi
if listed pad-test; then pick_set pad-test; fi
if listed splits-test; then pick_set splits-test; fi
if listed isolated-splits-test; then pick_set isolated-splits-test; fi
send_requests
launch
for app in "${APPS[@]}"; do
  package=${app#* }
  end=$((SECONDS + 60))
  while (( SECONDS < end )); do
    [[ "$(field state "$package")" == "{'ready': {}}" ]] && break
    sleep 0.2
  done
  state=$(field state "$package")
  if [[ "$state" == "{'ready': {}}" ]]; then ok "$package is ready"; else fail "$package is not ready: $state"; fi
done

if wanted app-start-test; then
  run_start app-start-test io.github.kisame76.minato.test.appstart 1
  python3 - "$WORK/app-start-test-1.out" "$WORK/app-start-test-1.log" fixtures/expected/app-start-test.logcat <<'EOF' \
    || failures=$((failures + 1))
import sys
sys.path.insert(0, "framework/tests")
from app_process_run import COLLECTOR, collector_type, expected_lines, our_lines

out, log, expected = sys.argv[1:]
got, wanted = our_lines(open(out, encoding="utf-8", errors="replace").read()), expected_lines(expected)
lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
# ART's line at every start names its collector: app_process's CMS, as on the Mac (ADR-0033 decision 5).
collector = collector_type("\n".join(lines))
if got == wanted and collector == COLLECTOR:
    print(f"ok      app-start-test printed what Android 15's app process shows, {len(got)} lines, under {COLLECTOR}")
    sys.exit(0)
if collector != COLLECTOR:
    print(f"FAIL    app-start-test's ART runs {collector or 'a collector its log does not name'}, not {COLLECTOR}")
if got != wanted:
    print("FAIL    app-start-test's lines differ from Android 15's:")
    for line in wanted:
        if line not in got:
            print(f"      | only Android's: {line}")
    for line in got:
        if line not in wanted:
            print(f"      | only ours:      {line}")
print("      | its guest log, last 40 lines:")
for line in lines[-40:]:
    print(f"      | {line}")
sys.exit(1)
EOF
fi

if wanted context-test; then
  run_start context-test io.github.kisame76.minato.test.context 1
  run_start context-test io.github.kisame76.minato.test.context 2
  python3 - "$WORK/context-test-1.log" "$WORK/context-test-2.log" fixtures/expected/context-test.logcat <<'EOF' \
    || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

first, second, expected = sys.argv[1:]
# One log of the two starts, as java_app_run.py --restart counts them.
log = "".join(open(path, encoding="utf-8", errors="replace").read() for path in (first, second))
compare_log("context-test", subprocess.CompletedProcess([], 0, "", log), expected, {})
EOF
fi

# Their Applications load their JNI libraries; jni-lib-test also looks at its own in nativeLibraryDir, the code path's
# lib/arm64, where the library puts clones of the version's originals. $3 is the expected lines, by default
# fixtures/expected/<app>.logcat.
run_one() {
  wanted "$1" || return 0
  run_start "$1" "$2" 1
  local expected="${3:-fixtures/expected/$1.logcat}"
  if [[ "${4:-}" == locale ]] && grep -q "^I/$1: greeting: hello from the base$" "$WORK/$1-1.log"; then
    sed "s/^\(I\/$1: greeting: hello from the \)English split$/\1base/" "$expected" > "$WORK/$1-expected.logcat"
    expected="$WORK/$1-expected.logcat"
  fi
  python3 - "$1" "$WORK/$1-1.log" "$expected" <<'EOF' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

app, log, expected = sys.argv[1:]
compare_log(app, subprocess.CompletedProcess([], 0, "", open(log, encoding="utf-8", errors="replace").read()), expected,
            {})
EOF
}
run_one asset-ndk-test io.github.kisame76.minato.test.assetndk
run_one jni-lib-test io.github.kisame76.minato.test.jnilib
run_one jnigraphics-test io.github.kisame76.minato.test.jnigraphics
# Its colors come from Monet's overlays, which every app's Resources take from /data/resource-cache (ADR-0043 f)).
run_one palette-test io.github.kisame76.minato.test.palette
# The app's own binders, which ART checks against IBinder's types at their first use (ADR-0043 d2b)).
run_one binder-test io.github.kisame76.minato.test.binder
# StrictMode's defaults from the bind, and getSerial's answer from the local device identifiers service (step c2d3).
run_one strictmode-test io.github.kisame76.minato.test.strictmode
# The app's own starts in its one task (step b3.5b2-1). The host's layer starts opaque and loses it under Second's and
# Third's windows, whose formats have alpha; it gets it back each time Main takes the session's window again and
# reports its own format. Both have alpha, as an opaque Third's new window would report what Main's take-back must.
if wanted activity-flow-test; then
  run_one activity-flow-test io.github.kisame76.minato.test.activityflow
  fetch_events
  opacity=$(sed -n -E 's/^[0-9.]+ [0-9]+ layer opaque ([01])$/\1/p' "$EVENTS" | tr -d '\n')
  if [[ "$opacity" == 10101 ]]; then
    ok "  the layer: opaque, not under Second's and Third's windows, opaque again under Main's after each"
  else
    fail "  the layer's opacity went ${opacity:-nowhere}, not 1, 0, 1, 0, 1"
  fi
fi
# Results between the app's activities (step b3.5b2-2a), with finishActivity, finishAffinity and finishAndRemoveTask.
run_one result-flow-test io.github.kisame76.minato.test.resultflow
# Launch modes and the task's flags (step b3.5b2-2b): singleTop, CLEAR_TOP, REORDER_TO_FRONT, singleTask, noHistory,
# navigateUpTo and CLEAR_TASK.
run_one launch-mode-test io.github.kisame76.minato.test.launchmode
# SQLite databases (the compatibility plan's gap 7): SQLiteOpenHelper on the main thread, cursors, WAL and the files.
run_one sqlite-test io.github.kisame76.minato.test.sqlite
# Time zones (the compatibility plan's gap 21): the tzdata module's files, and libcore's, java.time's and ICU's zones;
# the device's zone is only shown.
run_one timezone-test io.github.kisame76.minato.test.timezone
# The package installer for an app without install rights, and the exit reasons (the Unity test APK resolve list, batch E).
run_one package-installer-test io.github.kisame76.minato.test.packageinstaller
# android.media's DRM, codec list, retriever, extractor, image reader and TV input manager without the hardware.
run_one media-absent-test io.github.kisame76.minato.test.mediaabsent
run_one media-extractor-test io.github.kisame76.minato.test.mediaextractor
run_one media-codec-audio-test io.github.kisame76.minato.test.mediacodecaudio
# ADR-0043's test 1: views, a TextView and a Button drawn in software. This check plays no touches, so the Button's
# click comes from the app's performClick, 5 s after its label's change, which its device line names; the Mac's
# tools/views-check.sh and the AVD tap it.
[[ "$ON_DEVICE" == no ]] || run_one views-test io.github.kisame76.minato.test.views
# On the iPhone only, as its APPS entry (the Simulator has no MoltenVK).
[[ "$ON_DEVICE" == no ]] || run_one vk-ahb-test io.github.kisame76.minato.test.vkahb
[[ "$ON_DEVICE" == no ]] || run_one media-codec-test io.github.kisame76.minato.test.mediacodec
[[ "$ON_DEVICE" == no ]] || ! wanted media-bench-test || run_media_bench
[[ "$ON_DEVICE" == no ]] || run_one media-window-test io.github.kisame76.minato.test.mediawindow
# An exception that no thread catches (ADR-0039, the main thread's uncaught exception): it leaves ActivityThread.main,
# RuntimeInit's handlers log FATAL EXCEPTION after "Shutting down VM" and kill the process, so the program ends with
# 137, which needs no notice (ADR-0028, decision 5), and Minato starts the library.
if wanted crash-test; then
  if begin_start crash-test io.github.kisame76.minato.test.crash 1; then
    if ! await_line guest-log "^I/crash-test: done$" 300; then
      failed_line "no last line" crash-test-1
    elif ! await_event "program ended .*" 30; then
      failed_line "no end of the program after its last line" crash-test-1
    else
      [[ "${EVENT#* * }" == "program ended 137" ]] && ok "  the program ended with 137, the kill of itself" \
        || fail "  the program did not end with 137: ${EVENT#* * }"
      if await_event "process library( [0-9.]+)?$" 60; then
        ok "  back in the library without a notice"
      else
        failed_line "not back in the library without a notice" crash-test-1
      fi
    fi
  fi
  end_start crash-test 1
  python3 - crash-test "$WORK/crash-test-1.log" fixtures/expected/crash-test.logcat <<'PY' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log, in_order_found

app, log, expected = sys.argv[1:]
text = open(log, encoding="utf-8", errors="replace").read()
wanted = ["D/AndroidRuntime: Shutting down VM", "E/AndroidRuntime: FATAL EXCEPTION: main",
          "E/AndroidRuntime: Process: io.github.kisame76.minato.test.crash, PID: ",
          "E/AndroidRuntime: java.lang.IllegalStateException: thrown on the main thread",
          "I/crash-test: uncaught on main: java.lang.IllegalStateException: thrown on the main thread",
          "I/Process : Sending signal. PID: "]
if in_order_found(text.splitlines(), wanted) < len(wanted):
    print("FAIL: the guest log lacks Android's lines around the exception, in their order")
    sys.exit(1)
compare_log(app, subprocess.CompletedProcess([], 0, "", text), expected, {})
PY
fi
if wanted obb-test; then
  # Its OBB must be in its OBB folder before the tap.
  obb_rel="$SUPPORT_REL/data/io.github.kisame76.minato.test.obb/obb/main.1.io.github.kisame76.minato.test.obb.obb"
  obb="$CONTAINER/$obb_rel"
  for _ in $(seq 1 100); do fetch "$obb_rel" && break; sleep 0.2; done
  if [[ -f "$obb" ]]; then ok "obb-test's OBB is in its OBB folder"; else fail "no OBB in obb-test's OBB folder"; fi
  run_one obb-test io.github.kisame76.minato.test.obb
fi
if wanted packs-test && [[ "$ON_DEVICE" == no ]]; then
  # Its fast-follow and on-demand pack slices, which the library keeps from its set (ADR-0049 f) and the start lays out
  # read-only in local_testing of its external files folder, with the meta-data local_testing_dir (ADR-0050 decision 2):
  # the Mac's lines with the sizes of the set's slices.
  python3 - "$(apk packs-test)" fixtures/expected/packs-test-packs.logcat "$WORK/packs-test-apks.logcat" <<'EOF'
import re
import sys
import zipfile

apks, mac, out = sys.argv[1:]
with zipfile.ZipFile(apks) as archive:
    sizes = {name.rsplit("/", 1)[1]: archive.getinfo(name).file_size
             for name in archive.namelist() if name.startswith("asset-slices/")}
lines = []
for line in open(mac).read().splitlines():
    match = re.match(r"(.*: local_testing (\S+) size )\d+( .*)", line)
    lines.append(f"{match.group(1)}{sizes.get(match.group(2), 0)}{match.group(3)}" if match else line)
open(out, "w").write("\n".join(lines) + "\n")
EOF
  run_one packs-test io.github.kisame76.minato.test.packs "$WORK/packs-test-apks.logcat"
fi
# Google's asset-delivery serves both packs from the slices the start lays out, through its local-testing path
# (ADR-0050 test 3): Android 15's lines, as install-apks lays them out there.
if listed pad-test; then
  run_one pad-test io.github.kisame76.minato.test.pad
fi
if listed splits-test; then
  # The AVD's language is English, so Android's greeting comes from the set's English split. An iPhone or a Mac in
  # another language (German here) takes the base's string, as Android does for it (the set holds the English split,
  # not that language's): its expected line is then the base's. A named limit: an English device whose English split
  # failed to apply would pass too.
  run_one splits-test io.github.kisame76.minato.test.splits fixtures/expected/splits-test.logcat locale
fi
if listed isolated-splits-test; then run_one isolated-splits-test io.github.kisame76.minato.test.isolated; fi

if [[ -n "$JAVA_TEST" ]] && wanted java-test; then
  run_lifecycle 1
  run_lifecycle 2
  python3 - "$WORK/java-test-1.log" "$WORK/java-test-2.log" fixtures/expected/java-test.logcat "$WORK" <<'EOF' \
    || failures=$((failures + 1))
import os
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

first, second, expected, work = sys.argv[1:]
log = "".join(open(path, encoding="utf-8", errors="replace").read() for path in (first, second))
# At Home the focus goes before onPause, ADR-0032 decision 7's named departure (App's c6 host fixes): iOS takes the
# focus at willDeactivate, before it tells of the background. Android's other lines and their order stay.
wanted = open(expected, encoding="utf-8").read().splitlines()
for i in range(len(wanted) - 1):
    if wanted[i:i + 2] == ["I/java-test: onPause, finishing false", "I/java-test: focus false"]:
        wanted[i:i + 2] = wanted[i + 1], wanted[i]
ours = os.path.join(work, "java-test.logcat")
with open(ours, "w", encoding="utf-8") as handle:
    handle.write("\n".join(wanted) + "\n")
print("note    at Home focus false comes before onPause, ADR-0032 decision 7's named departure")
# compare_log names missing and extra lines; the same lines in another order it only fails.
got = [line for line in log.splitlines() if line.startswith("I/java-test: ")]
first_difference = next((i for i, (a, b) in enumerate(zip(got, wanted)) if a != b), None)
if first_difference is not None and sorted(got) == sorted(wanted):
    print(f"      | the order differs at line {first_difference + 1}: ours {got[first_difference]!r}, "
          f"expected {wanted[first_difference]!r}")
compare_log("java-test", subprocess.CompletedProcess([], 0, "", log), ours, {})
EOF
fi

if wanted surface-test; then
  run_start surface-test io.github.kisame76.minato.test.surface 1
  python3 - "$WORK/surface-test-1.log" fixtures/expected/surface-test.logcat "$WORK" <<'EOF' \
    || failures=$((failures + 1))
import os
import re
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

path, expected, work = sys.argv[1:]
log = open(path, encoding="utf-8", errors="replace").read()
wanted = open(expected, encoding="utf-8").read()
# Android's sizes are the AVD's screen, the iPhone 15 Pro Max's; on another screen the view's line names its size.
screen = re.search(r"^I/surface-test: view: at 0, 0, (\d+) x (\d+)$", log, re.M)
if screen and screen.groups() != ("1290", "2796"):
    print(f"note    the screen is {screen[1]} x {screen[2]} pixels, which take the place of Android's 1290 x 2796")
    wanted = wanted.replace("1290 x 2796", f"{screen[1]} x {screen[2]}")
    # Test 4's Button, centered over the GLSurfaceView: its size stays, its place is the center of this screen.
    button = re.search(r"^I/surface-test: button: added at \d+, \d+, (\d+) x (\d+)$", wanted, re.M)
    if button:
        width, height = int(button[1]), int(button[2])
        x, y = (int(screen[1]) - width) // 2, (int(screen[2]) - height) // 2
        print(f"note    the Button is {width} x {height}, centered on this screen at {x}, {y}")
        wanted = wanted.replace(button[0], f"I/surface-test: button: added at {x}, {y}, {width} x {height}")
ours = os.path.join(work, "surface-test.logcat")
with open(ours, "w", encoding="utf-8") as handle:
    handle.write(wanted)
compare_log("surface-test", subprocess.CompletedProcess([], 0, "", log), ours, {})
EOF
fi

# Test 2's Godot APKs (ADR-0042 b)): their "godot-bench: " lines with Godot's padded tag must be Android 15's, and their
# run line must have frames and no failures, as tools/godot-java-check.sh checks them on the Mac.
for flavor in vulkan gles; do
  [[ -n "$GODOT_TEST" ]] && wanted "godot-bench-$flavor" || continue
  [[ "$flavor" == gles || "$ON_DEVICE" == yes ]] || continue
  run_start "godot-bench-$flavor" "io.github.kisame76.minato.godotbench.$flavor" 1
  python3 - "$WORK/godot-bench-$flavor-1.log" "fixtures/expected/godot-bench-$flavor.logcat" "godot-bench-$flavor" \
    <<'EOF' || failures=$((failures + 1))
import re
import sys

log, expected, app = sys.argv[1:]
tag = r"^[VDIWEF]/godot *: "
lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
godot = [line for line in lines if re.match(tag, line)]
got = [line for line in godot if re.match(tag + "godot-bench: ", line)]
wanted = open(expected, encoding="utf-8").read().splitlines()
# No touches are played here: the bench's touch line is the Mac's (tools/godot-java-check.sh), on both sides.
touch = re.compile(tag + "godot-bench: first touch ")
got = [line for line in got if not touch.match(line)]
wanted = [line for line in wanted if not touch.match(line)]
ran = any(re.match(tag + r"run [^:]+: [1-9][0-9]* frames, 0 failures$", line) for line in godot)
if got == wanted and ran:
    print(f"ok      {app} logged what Android 15 logs, {len(got)} lines, and its run had no failures")
    for line in godot:
        if line not in got:
            print(f"        device: {line}")
    sys.exit(0)
print(f"FAIL    {app}" + ("" if ran else ': no line "run <name>: N frames, 0 failures"'))
for line in wanted:
    if line not in got:
        print(f"      | only Android's: {line}")
for line in got:
    if line not in wanted:
        print(f"      | only ours:      {line}")
print("      | its guest log, last 40 lines:")
for line in lines[-40:]:
    print(f"      | {line}")
sys.exit(1)
EOF
done

# KEYS: key-test (ADR-0068 step 7c2) starts, logs "channel: ready" after the keys it injects itself, and takes the three
# keys the operator types, 2, b and Delete, from the Simulator's hardware keyboard path; then "done".
if [[ -n "${KEYS:-}" ]]; then
  if begin_start key-test io.github.kisame76.minato.test.key 1; then
    if await_line guest-log '^I/key-test: channel: ready$' 300; then
      echo "type    now, in the Simulator's window: 2, then b, then Delete (the Mac keyboard path)"
      if await_line guest-log '^I/key-test: done$' 300; then
        ok "  the three keys arrived, $(seconds "$SEEN" "$(now)") s ago"
        sleep 2
      else
        failed_line "no keys within 300 s" key-test-1
      fi
    else
      failed_line 'no "channel: ready"' key-test-1
    fi
  fi
  end_start key-test 1
  python3 - "$WORK/key-test-1.log" fixtures/expected/key-test.logcat "$WORK" <<'EOF' || failures=$((failures + 1))
import re
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

path, expected, work = sys.argv[1:]
lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
ready = next((i for i, line in enumerate(lines) if line.endswith("key-test: channel: ready")), len(lines))
# Up to "channel: ready" the log must be Android 15's: the map and the keys the app injects itself.
failed = 0
try:
    compare_log("key-test", subprocess.CompletedProcess([], 0, "", "\n".join(lines) + "\n"), expected, {},
                until="channel: ready")
except SystemExit as exit_:
    failed = exit_.code or 1
# After it come the keys the operator typed, whichever they are: each one of the Simulator's hardware keyboard, which
# the app sees as the host's device 1000 (Android's virtual keyboard is -1), as a down and an up, and the app's "done".
typed = lines[ready + 1:]
edits = [line for line in typed if re.search(r"key-test: edit: (down|up) ", line)]
wrong = [line for line in edits if not line.endswith(", device 1000")]
channel = [line for line in typed if re.search(r"key-test: channel KEYCODE_", line)]
if len(channel) >= 3 and edits and not wrong and any(line.endswith("key-test: done") for line in typed):
    print(f"ok      {len(edits)} key lines from the hardware keyboard, device 1000, and {len(channel)} typed results")
    for line in typed:
        print(f"        typed: {line}")
else:
    print("FAIL    the typed keys: expected 3 or more key results, all from device 1000, then done")
    for line in typed:
        print(f"      | {line}")
    failed = 1
sys.exit(failed)
EOF
fi

# IME: ime-test (ADR-0068 step 2c) starts with its field focused and asks for the keyboard; UIKit's must come, and the
# operator types h, i, Delete and Go on it. The app's own timeout ends its run after 10 s, but every call is logged.
if [[ -n "${IME:-}" ]]; then
  if begin_start ime-test io.github.kisame76.minato.test.ime 1; then
    if await_line guest-log '^I/ime-test: showSoftInput true$' 300; then
      if await_event 'keyboard show\(' 20; then
        ok "  UIKit's keyboard shows"
        echo "type    now, on the on-screen keyboard: h, i, Delete, then Go"
        if await_line guest-log '^I/ime-test: call performEditorAction 2 ' 300; then
          ok "  the editor's action arrived, $(seconds "$SEEN" "$(now)") s ago"
          sleep 2
        else
          failed_line "no Go within 300 s" ime-test-1
        fi
      else
        failed_line "no keyboard within 20 s" ime-test-1
      fi
    else
      failed_line 'no "showSoftInput true"' ime-test-1
    fi
  fi
  end_start ime-test 1
  python3 - "$WORK/ime-test-1.log" fixtures/expected/ime-test.logcat <<'PY' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

path, expected = sys.argv[1:]
lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
# Up to "showSoftInput true" the log must be Android 15's: the editor and the manager's answers.
failed = 0
try:
    compare_log("ime-test", subprocess.CompletedProcess([], 0, "", "\n".join(lines) + "\n"), expected, {},
                until="showSoftInput true")
except SystemExit as exit_:
    failed = exit_.code or 1
calls = [line.split("ime-test: ", 1)[1] for line in lines if "ime-test: call " in line]
wanted = ['call commitText "h" 1 -> true', 'call commitText "i" 1 -> true', "call deleteSurroundingText 1,0 -> true",
          "call performEditorAction 2 -> true"]
if calls == wanted:
    print("ok      the host's records reached the connection: " + ", ".join(call[5:].split(" ->")[0] for call in calls))
else:
    print("FAIL    the connection's calls: expected " + "; ".join(wanted))
    for line in lines:
        print(f"      | {line}")
    failed = 1
sys.exit(failed)
PY
fi

echo "java start check: $failures failed"
exit $((failures > 0))
