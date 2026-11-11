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
# asset-delivery serves both packs from those slices; skipped without the libraries, which are fetched separately)
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
# media-codec-audio-test (ADR-0060 e2f) is one too: MediaCodec's AAC decoder on the host's AudioToolbox; and
# media-codec-decoders-test (e2f.2): its MP3, FLAC and raw decoders; media-codec-opus-test (e2): its Opus decoder;
# and media-extractor-ogg-test (e2): MediaExtractor on Ogg Opus.
# webview-test (ADR-0056 c2) is one too: its WebView's pages on graphics/host's WKWebView, in the session's view below
# the window's layer, which then is not opaque.
# views-test (ADR-0043's test 1: views, a TextView and a Button drawn in software) runs on the iPhone only, whose screen
# its views' bounds are (the Simulator's is another): this check plays no touches, so the app clicks its Button with
# performClick 5 s after its label's change, and its device line says so (the Mac's tools/views-check.sh and the AVD
# tap it).
# vk-ahb-test (ADR-0058 e1), decision 7) runs on the iPhone only, a NativeActivity app of tools/build-fixtures.sh whose
# Vulkan needs MoltenVK, which the Simulator lacks: VK_ANDROID_external_memory_android_hardware_buffer's import and
# export on the iPhone's IOSurfaces. gles-procs-test runs on the iPhone only too, a NativeActivity app of
# tools/build-fixtures.sh: which GLES, EGL and Vulkan functions an app finds by dlsym, eglGetProcAddress and
# vk*ProcAddr, against Android 15's measurement (graphics/tests/gles_procs_android.txt); engine-start-test too, a game
# engine's start past those lookups (contexts from ES 3.2 down, a shared context, frame
# timestamps, a frame pacer's fence wait and display timing; the Mac's tools/engine-start-check.sh).
# media-codec-test and media-window-test (ADR-0060 e2c) run on the iPhone only too, against Android 15's lines, and
# media-bench-test, Minato's measurement at 1080p and 4K (its lines shown, Home twice).
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
#   NOTIFY_REAL set: with notify-tap-test, its five rounds of the user's tap (the MinatoUITests runner drives
#              SpringBoard): on the iPhone on the entry in Notification Center, in the Simulator on the banner of the
#              notification handed to iOS again (Debug's notification-repost), since the Simulator treats Notification
#              Center as the Lock Screen; each round says which it tapped. The iPhone must be unlocked. Needs a
#              single-screen iPhone: on the iPhone Duo the runner's SpringBoard is the cover screen's, and its Home
#              press and edge drags miss the main screen.
#   IME        set: only ime-test runs (ADR-0068 step 2c); the typing is the operator's on UIKit's keyboard, which must
#              show (in the Simulator I/O > Keyboard > Connect Hardware Keyboard off, on the iPhone no hardware
#              keyboard connected): the script waits for the app's "showSoftInput true" and the session's "keyboard
#              show", says what to type, and waits for the editor's action. Up to "showSoftInput true" the log must
#              be Android 15's; then come the host's records: h and i committed, the delete as
#              deleteSurroundingText 1,0, and Go as performEditorAction 2, leaving "h"
#   WEBVIEW_TOUCH set: only webview-touch-test runs (ADR-0056 c2, test 3); the touches are the operator's, as simctl
#              sends none: once the app is ready the script prints its points in the window's pixels (the Simulator's
#              points times the screen's scale) and the four touches to make, and waits for its "done". Its lines
#              must be Android 15's, whose touches tools/android-reference.sh plays from the same points.
#   JAVA_TEST  set: java-test's round runs in the Simulator too, where its layout line shows the Simulator's screen
#   EDGE_SWIPE_SIM set: the edge deferral round (edge-back-test-landscape, two swipes up from the bottom edge) runs in
#              the Simulator too, where iOS gives the first swipe in landscape to the system for every app
#   EDGE_CONTROL set: after it, the same swipes over Minato's bare control view controller (EdgeControl), a note
#              whether the control kept the first swipe (Minato's cause) or lost it too (iOS's)
#   ONLY       the apps to start, by name and space separated, such as ONLY=java-test for a sitting with java-test
#              alone; java-test or a Godot APK among them runs in the Simulator too
#   MINUTES    how long renpy-memory-test plays and is sampled, 15 by default; the app runs only with
#              ONLY=renpy-memory-test. The game (tools/renpy-memory.sh's, built with BUILD_ONLY) shows distinct
#              full-screen backgrounds one after another as a visual novel does; the Debug build writes the process's
#              physical footprint every 10 s and every iOS memory warning to tmp/session-check/footprint-log. The
#              check prints "csv,footprint_min_mb,footprint_max_mb,footprint_end_mb,memory_warnings,on_low_memory"
#              (tools/renpy_footprint.py); a run with no footprint sample is a note, not a failure.
#   RENPY_MEMORY_APK  an APK of the same package that renpy-memory-test plays instead of tools/renpy-memory.sh's
#              (Minato's own Ren'Py builds only, such as the ATL frame sequence of the texture parking's round)
#   PIXELS     a folder: only the pixel round of ADR-0043 b5's step f runs (tools/window-contents-check.sh), with
#              views-test, dialog-test, second-windows-test, activity-flow-test and window-formats-test (ONLY= picks),
#              each started with SessionCheck's window-crc. At each checkpoint the windows' dump (window-dump) and a
#              screenshot go into <folder>/<app>/<checkpoint>/ (dump/, screen.png, compare.txt), and
#              tools/window_pixels.py compares them: after the start (the Java apps' "done", second-windows-test's
#              first dialog, which waits for a tap; window-formats-test once for each format it shows), then after
#              Home and the return, once the menu's Continue came ("menu
#              continued", Android's order: onStart, onResume, the window). The start's guest log is kept as
#              <folder>/<app>/guest-log, window-formats-test's compared with Android 15's, and SessionCheck's
#              footprint log as <folder>/<app>/footprint-log. PIXELS_DRAWABLES=1 draws the windows by drawables
#              (window-by-drawables, a Debug switch), for the memory comparison; PIXELS_CONTENTS=1 shows them by
#              contents in the Simulator, the iPhone's way (window-by-contents, a Debug switch).
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
  read -r DEVICE_ID DEVICE_UDID _ _ < <(python3 tools/find-device.py "${1:-}") || true
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
      "media-codec-audio-test io.github.kisame76.minato.test.mediacodecaudio"
      "media-codec-decoders-test io.github.kisame76.minato.test.mediacodecdecoders"
      "media-codec-opus-test io.github.kisame76.minato.test.mediacodecopus"
      "media-extractor-ogg-test io.github.kisame76.minato.test.mediaextractorogg"
      "webview-test io.github.kisame76.minato.test.webview"
      "documents-tree-test io.github.kisame76.minato.test.documentstree"
      "storage-space-test io.github.kisame76.minato.test.storagespace"
      "network-test io.github.kisame76.minato.test.network"
      "network-request-test io.github.kisame76.minato.test.networkrequest"
      "network-security-test io.github.kisame76.minato.test.networksecurity"
      "network-security-config-test io.github.kisame76.minato.test.networksecurityconfig"
      "wifi-state-test io.github.kisame76.minato.test.wifistate"
      "lan-test io.github.kisame76.minato.test.lan"
      "net-io-test io.github.kisame76.minato.test.netio"
      "orientation-request-test io.github.kisame76.minato.test.orientationrequest"
      "gamepad-test io.github.kisame76.minato.test.gamepad"
      "permission-request-test-fine io.github.kisame76.minato.test.permreqfine"
      "location-manager-test-fine io.github.kisame76.minato.test.locmanfine"
      "location-manager-test-coarse io.github.kisame76.minato.test.locmancoarse"
      "permission-request-test-camera io.github.kisame76.minato.test.permreqcamera"
      "permission-request-test-notifications io.github.kisame76.minato.test.permreqnotifications"
      "notify-test io.github.kisame76.minato.test.notify"
      "notify-test-granted io.github.kisame76.minato.test.notifygranted"
      "notify-tap-test io.github.kisame76.minato.test.notifytap"
      "ime-engines-test io.github.kisame76.minato.test.imeengines"
      "ime-pan-test io.github.kisame76.minato.test.imepan" "ime-pan-test-34 io.github.kisame76.minato.test.imepan34"
      "native-jni-ime-test io.github.kisame76.minato.test.nativejniime"
      "ime-surrounding-test-delete io.github.kisame76.minato.test.imesurroundingdelete"
      "ime-surrounding-test-return io.github.kisame76.minato.test.imesurroundingreturn"
      "ime-surrounding-test-hide io.github.kisame76.minato.test.imesurroundinghide"
      "install-flow-test io.github.kisame76.minato.test.installflow"
      "install-session-flow-test io.github.kisame76.minato.test.installsessionflow"
      "install-set-flow-test io.github.kisame76.minato.test.installsetflow"
      "starting-window-test io.github.kisame76.minato.test.startingwindow"
      "ime-outside-test io.github.kisame76.minato.test.imeoutside"
      "edge-back-test-landscape io.github.kisame76.minato.test.edgeback"
      "uninstall-test io.github.kisame76.minato.test.uninstall"
      "uninstall-noperm-test io.github.kisame76.minato.test.uninstallnoperm"
      "vsync-test io.github.kisame76.minato.test.vsync")
JAVA_TEST=${JAVA_TEST:-}
ONLY=${ONLY:-}
[[ "$ON_DEVICE" == no ]] || JAVA_TEST=yes
[[ " $ONLY " != *" java-test "* ]] || JAVA_TEST=yes
# obb-test's OBB comes as a unit folder of its own, which this check makes in the Simulator only.
APPS+=("obb-test io.github.kisame76.minato.test.obb")
# packs-test comes as a bundletool .apks, which this check imports in the Simulator only, as it does obb-test's OBB.
[[ "$ON_DEVICE" == yes ]] || APPS+=("packs-test io.github.kisame76.minato.test.packs")
# obb-large-test comes as an .xapk with an OBB of more than 2 GiB (tools/make-large-xapk.py), a unit of its own that the
# library inflates into the app: opt-in (LARGE_XAPK=1), as it needs 2.2 GB of room in the app's data folder, which the
# run removes again with the app.
[[ -z "${LARGE_XAPK:-}" ]] || APPS+=("obb-large-test io.github.kisame76.minato.test.obblarge")
APPS+=("splits-test io.github.kisame76.minato.test.splits")
# Ren'Py's splashscreen that calls a screen at once (ADR-0055), from tools/renpy-splash-build.sh, which this check
# does not make: Interface start comes early, which game mode's turn to landscape must not catch without a window.
[[ ! -f build/renpy-splash-test/renpy-splash-test.apk ]] ||
  APPS+=("renpy-splash-test io.github.kisame76.minato.test.renpy")
# The same game that stays on screen (tools/renpy-splash-build.sh stay), for the resume round (RESUME) only.
[[ -z "${RESUME:-}" || ! -f build/renpy-splash-test/renpy-stay-test.apk ]] ||
  APPS+=("renpy-stay-test io.github.kisame76.minato.test.renpy")
APPS+=("isolated-splits-test io.github.kisame76.minato.test.isolated")
# Minato's Ren'Py memory game (tools/renpy-memory.sh), only with ONLY=renpy-memory-test: it plays for MINUTES.
[[ " $ONLY " != *" renpy-memory-test "* ]] ||
  APPS+=("renpy-memory-test io.github.kisame76.minato.test.renpymemory")
# workmanager-test too, when the WorkManager libraries are there to build it with
# (tools/fetch-workmanager-libraries.py).
if /usr/bin/python3 tools/fetch-workmanager-libraries.py >/dev/null 2>&1; then
  APPS+=("workmanager-test io.github.kisame76.minato.test.workmanager")
else
  echo "skipped: workmanager-test: no WorkManager libraries (tools/fetch-workmanager-libraries.py --fetch, on request)"
fi
# pad-test too, when Google's Play libraries are there to build it with (ADR-0050 c)).
if /usr/bin/python3 tools/fetch-play-libraries.py >/dev/null 2>&1; then
  APPS+=("pad-test io.github.kisame76.minato.test.pad")
else
  echo "skipped: pad-test: no Play libraries (tools/fetch-play-libraries.py --fetch, on request)"
fi
[[ -z "$JAVA_TEST" ]] || APPS+=("java-test io.github.kisame76.minato.test.javatest")
GODOT_TEST=${GODOT_TEST-yes}
[[ "$GODOT_TEST" != no ]] || GODOT_TEST=""
[[ " $ONLY " != *" godot-bench-"* ]] || GODOT_TEST=yes
# The Vulkan APK on the iPhone only: the Simulator has no MoltenVK (ADR-0030, decision 8).
[[ -z "$GODOT_TEST" || "$ON_DEVICE" == no ]] || APPS+=("godot-bench-vulkan io.github.kisame76.minato.godotbench.vulkan")
[[ -z "$GODOT_TEST" ]] || APPS+=("godot-bench-gles io.github.kisame76.minato.godotbench.gles")
[[ "$ON_DEVICE" == no ]] || APPS+=("vk-ahb-test io.github.kisame76.minato.test.vkahb")
[[ "$ON_DEVICE" == no ]] || APPS+=("gles-procs-test io.github.kisame76.minato.test.glesprocs")
# The frame rate limit's hold of unpaced presents (ADR-0078 decision 4): EGL in the Simulator too, Vulkan on the
# iPhone only (the Simulator has no MoltenVK).
APPS+=("present-limit-gles io.github.kisame76.minato.test.presentlimit.gles")
[[ "$ON_DEVICE" == no ]] || APPS+=("present-limit-vulkan io.github.kisame76.minato.test.presentlimit.vulkan")
[[ "$ON_DEVICE" == no ]] || APPS+=("engine-start-test io.github.kisame76.minato.test.enginestart")
APPS+=("program-binary-test io.github.kisame76.minato.test.programbinary")
APPS+=("hardware-bitmap-test io.github.kisame76.minato.test.hardwarebitmap")
APPS+=("graphics-classes-test io.github.kisame76.minato.test.graphicsclasses")
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
# WEBVIEW_TOUCH: webview-touch-test alone (ADR-0056 c2), whose touches are the operator's.
if [[ -n "${WEBVIEW_TOUCH:-}" ]]; then
  APPS+=("webview-touch-test io.github.kisame76.minato.test.webviewtouch")
  ONLY=webview-touch-test
fi
# HANG: hang-test alone, whose main thread sleeps 60 s after its first frame; the menu and Quit come 12 s after the
# start (SessionCheck's "quit"), and the library must be back within 15 s of the Quit, the hung guest ended.
if [[ -n "${HANG:-}" ]]; then
  APPS+=("hang-test io.github.kisame76.minato.test.hang")
  ONLY=hang-test
fi
# RESUME: the user's bug of a black screen after leaving Minato and coming back. RESUME=1 (ONLY= picks the apps; the
# default is those of godot-bench-gles, godot-bench-vulkan, renpy-splash-test, surface-test and views-test this run has)
# starts each one, sends Minato to the background (AWAY) for RESUME_WAITS seconds (default "15 60"; iOS suspends a
# background app), brings it back and checks, with SessionCheck's ping and menu requests: the host's main thread
# answers, the guest's windows present new frames, the screenshot is not black, the menu opens. RESUME_SETTLE (8) is
# the seconds an app runs before the first background (3 and a single 15 s trip for Godot's bench, which ends by itself
# after about 14 s). Only that round runs. A touch is checked by hand (`motion` events).
# COMPILE: the measures of M6 c's E6 (tools/compile-measure.sh): compute-app-test and godot-bench-gles, each started
# plain, left by Quit so that the library asks for compile mode, compiled by dex2oat in a process of its own, signed at
# the next library start and started again with its compiled code. It prints a "csv," line for each (the columns are
# tools/compile-measure.sh's); the Debug build's events carry dex2oat's peak footprint and the library's sizes and
# signing time (app/Sources/Session/CompileMode.swift, LibraryModel.swift).
if [[ -n "${COMPILE:-}" ]]; then
  for app in "compute-app-test io.github.kisame76.minato.test.computeapp" \
             "godot-bench-gles io.github.kisame76.minato.godotbench.gles"; do
    [[ " ${APPS[*]%% *} " == *" ${app%% *} "* ]] || APPS+=("$app")
  done
  # COMPILE=1 runs both; COMPILE="compute-app-test" (or godot-bench-gles) only that one.
  if [[ "$COMPILE" == 1 ]]; then ONLY="compute-app-test godot-bench-gles"; else ONLY=$COMPILE; fi
fi
# IME: ime-test alone (ADR-0068 step 2c), whose text comes from the operator's taps on UIKit's keyboard.
if [[ -n "${IME:-}" ]]; then
  APPS+=("ime-test io.github.kisame76.minato.test.ime")
  ONLY=ime-test
fi
# RESUME=1 without ONLY: the apps that draw, as far as this run has them (Vulkan needs a device, views-test too).
if [[ -n "${RESUME:-}" && -z "${ONLY// /}" ]]; then
  for name in godot-bench-gles godot-bench-vulkan renpy-stay-test surface-test views-test; do
    [[ " ${APPS[*]%% *} " != *" $name "* ]] || ONLY+=" $name"
  done
fi
# PIXELS: the pixel round's apps (ADR-0043 b5, step f), ONLY's or all five.
if [[ -n "${PIXELS:-}" ]]; then
  for app in "views-test io.github.kisame76.minato.test.views" "dialog-test io.github.kisame76.minato.test.dialog" \
             "second-windows-test io.github.kisame76.minato.test.secondwindows" \
             "activity-flow-test io.github.kisame76.minato.test.activityflow" \
             "window-formats-test io.github.kisame76.minato.test.windowformats" \
             "views-test-landscape io.github.kisame76.minato.test.viewslandscape" \
             "static-window-test io.github.kisame76.minato.test.staticwindow" \
             "egl-probe-decor-test io.github.kisame76.minato.test.eglprobedecor"; do
    [[ " ${APPS[*]%% *} " == *" ${app%% *} "* ]] || APPS+=("$app")
  done
  [[ -n "${ONLY// /}" ]] ||
    ONLY="views-test dialog-test second-windows-test activity-flow-test window-formats-test views-test-landscape"
fi
# Whether app $1 starts in this run: each one, or ONLY's, which must be the check's.
# notify-test-granted and notify-tap-test need iOS's answer for Minato's notifications, granted: the host passes a
# stored grant on only while iOS gives it. In the Simulator only permission-request-test-notifications' round gets
# that answer (its runner answers iOS's prompt), so ONLY takes it along; a Simulator or a Minato install that never
# answered would fail them with "posted: enabled false".
if [[ "$ON_DEVICE" == no && -n "${ONLY// /}" && " $ONLY " != *" permission-request-test-notifications "* ]] &&
    [[ " $ONLY " == *" notify-tap-test "* || " $ONLY " == *" notify-test-granted "* ]]; then
  ONLY="permission-request-test-notifications $ONLY"
  echo "note    ONLY takes permission-request-test-notifications along, for iOS's notifications answer"
fi
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
    gles-procs-test) echo "guest-log ^I/gles-procs-test: android_main returns$" ;;
    window-formats-test) echo "guest-log ^I/window-formats-test: android_main returns$" ;;
    orientation-request-test) echo "guest-log ^I/orientation-request-test: config [0-9]+ done$" ;;
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
    vk-ahb-test|window-formats-test|gles-procs-test|present-limit-*) echo "build/fixtures/apks/$1.apk" ;;
    renpy-splash-test|renpy-stay-test) echo "build/renpy-splash-test/$1.apk" ;;
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
    workmanager-test) tools/build-workmanager-test.sh >/dev/null ;;
    pad-test) tools/build-pad-test.sh >/dev/null ;;
    splits-test|isolated-splits-test) tools/build-splits-test.sh "${app%% *}" >/dev/null ;;
    vk-ahb-test|window-formats-test|gles-procs-test|present-limit-*) tools/build-fixtures.sh >/dev/null ;;
    renpy-splash-test|renpy-stay-test) ;;
    renpy-memory-test)
      if [[ -n "${RENPY_MEMORY_APK:-}" ]]; then
        mkdir -p build/renpy-memory-test
        cp "$RENPY_MEMORY_APK" build/renpy-memory-test/renpy-memory-test.apk
      else
        BUILD_ONLY=1 tools/renpy-memory.sh
      fi
      ;;
    # Their assets are splits-test's set (build/splits-test/set/).
    install-set-test|install-set-flow-test)
      tools/build-splits-test.sh splits-test >/dev/null
      tools/build-java-test-app.sh "${app%% *}" >/dev/null
      ;;
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

# How many lines of file $2 match the extended regex $1, 0 when the file is missing (a fetch that failed): grep -c
# prints nothing then, which is a syntax error in (( )).
count_lines() {
  local n
  n=$(grep -c -E "$1" "$2" 2>/dev/null) || true
  echo "${n:-0}"
}
# A line with the time of day, for the rounds that look for what stalls the iPhone's copies: STAMPS=yes, which the
# NOTIFY_REAL rounds set, or the caller's for the whole run. A copy of 10 s or more has its line always. The lines go
# to the script's own stderr (3), never into what a caller captures (the ready check parses field's output), and name
# a local file by its path in the work folder only.
STAMPS=${STAMPS:-}
stamp() { [[ -z "$STAMPS" ]] || echo "note    $(date +%H:%M:%S) $*" >&3; }
copy_stamp() {
  if [[ -n "$STAMPS" ]] || (( SECONDS - $2 >= 10 )); then
    echo "note    copy $1 $3-$(date +%H:%M:%S) ($((SECONDS - $2)) s) attempt $4: $5" >&3
  fi
}
# The tail of devicectl's own log for a copy that hit the deadline and was killed, so a device run shows
# where its file service stalled, not only that it did. Device identifiers are masked; the copy only ever
# touches our own session-check files, so those paths may stay. Called only on a hang, which goes to a FAIL.
copy_trace_tail() {
  [[ -s "$1" ]] || return 0
  local uuid='[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}'
  local ecid='[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}'
  local masked
  masked=$(tail -n 6 "$1" | sed -E "s/$uuid/<id>/g; s/$ecid/<id>/g")
  [[ -n "${DEVICE_ID:-}" ]] && masked=${masked//$DEVICE_ID/<device>}
  [[ -n "${DEVICE_UDID:-}" ]] && masked=${masked//$DEVICE_UDID/<device>}
  [[ -n "$masked" ]] || return 0
  echo "note    devicectl's last log before the killed copy, where its file service stalled:" >&3
  sed 's/^/note    | /' <<< "$masked" >&3
}
devicectl_copy() {
  local direction=$1
  shift
  # A lost device aborts the whole check, as on_device does; keep that guard before the timed copy.
  python3 tools/require-device.py "$DEVICE_ID" 2>&3 || exit 1
  # devicectl has hung for minutes on the phone (seen twice in a row), which would make the check wait and then go
  # red for the wrong reason, so the copy runs with a deadline the way the tools bound things without a GNU timeout:
  # in the background, killed if it outlasts the deadline. One retry; if it hangs both times, a red names the cause.
  local limit=${DEVICECTL_COPY_TIMEOUT_S:-90} attempt hung=no status shown=${2:-}
  shown=${shown#"$WORK"/}
  # devicectl's own log of this copy, named so restore()'s *.log/*.out sweep into LOGS skips it; read back on
  # a hang (which then goes to a FAIL) to show where the file service stalled, device identifiers masked out.
  local trace="$WORK/devicectl-copy-trace"
  for attempt in 1 2; do
    local from started=$SECONDS
    from=$(date +%H:%M:%S)
    : > "$trace"
    xcrun devicectl device copy "$direction" --device "$DEVICE_ID" --domain-type appDataContainer \
      --domain-identifier "$BUNDLE_ID" --verbose --log-output "$trace" "$@" >/dev/null 2>&1 &
    local pid=$!
    local end=$((SECONDS + limit))
    while kill -0 "$pid" 2>/dev/null && (( SECONDS < end )); do sleep 0.2; done
    if kill -0 "$pid" 2>/dev/null; then
      pkill -P "$pid" 2>/dev/null  # a child devicectl, if xcrun forked one, so the hung copy does not outlive us
      kill "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      hung=yes
      copy_stamp "$direction ${shown:-}" "$started" "$from" "$attempt" "hung, killed"
      copy_trace_tail "$trace"
    else
      wait "$pid"
      status=$?
      copy_stamp "$direction ${shown:-}" "$started" "$from" "$attempt" "exit $status"
      [[ $status -eq 0 ]] && return 0
      hung=no
    fi
  done
  [[ $hung == yes ]] && fail "devicectl copy $direction hung: no response within ${limit}s, retried once"
  return 1
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
REQUESTS="way tap quit confirm orientation unanswered forget-hint crash start-failed tone fps no-guest guest-log extra-session turn-library turn-session audio-disconnect status-bar no-compile ping menu continue window-crc window-dump window-by-drawables window-by-contents window-trace footprint-log pick-allow install-allow install-deny keyboard-views frame-rate-limit end events"
# The notification taps of the notify-tap-test rounds.
REQUESTS+=" notification-tap notification-cold-tap notification-foreground-tap notification-response-save"
REQUESTS+=" notification-repost"
# On the iPhone: the requests as the script has them go into the app's folder. A request the script does not have goes
# as an empty file, which the app takes for none: devicectl cannot delete a file. The events go only after the script
# emptied them for a new start (reset_events): sent at any other time, the copy the script holds would replace the
# iPhone's file and drop the lines that came after it was fetched.
EVENTS_RESET=""
reset_events() {
  : > "$EVENTS"
  seen=0
  EVENTS_RESET=yes
  EVENTS_SHORTER=""
}
send_requests() {
  [[ "$ON_DEVICE" == yes ]] || return 0
  rm -rf "$WORK/sent"
  mkdir "$WORK/sent"
  for name in $REQUESTS; do
    [[ "$name" != events || -n "$EVENTS_RESET" ]] || continue
    if [[ -f "$CHECK/$name" ]]; then cp "$CHECK/$name" "$WORK/sent/"; else : > "$WORK/sent/$name"; fi
  done
  devicectl_copy to --source "$WORK/sent" --destination "$CHECK_REL" || return 1
  EVENTS_RESET=""
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
# Between two resets the iPhone's file only grows, so a copy that comes back shorter than the one held (pass 4's
# ime-surrounding-test-hide: no events at all under its FAIL) is not taken, with one line per start.
EVENTS_SHORTER=""
fetch_events() {
  [[ "$ON_DEVICE" == yes ]] || return 0
  rm -f "$WORK/events"
  devicectl_copy from --source "$CHECK_REL/events" --destination "$WORK/events" || return 0
  local held got
  held=$(wc -c < "$EVENTS" 2>/dev/null || echo 0)
  got=$(wc -c < "$WORK/events" 2>/dev/null || echo 0)
  if (( got < held )); then
    [[ -n "$EVENTS_SHORTER" ]] ||
      echo "note    the iPhone's events came back shorter ($((got)) < $((held)) bytes): the longer kept" >&3
    EVENTS_SHORTER=yes
    return 0
  fi
  mv "$WORK/events" "$EVENTS" 2>/dev/null || true
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
    # A device with two displays (the iPhone Duo) wants one named: the primary's is 1.
    xcrun simctl io "$SIM_UDID" screenshot "$LOGS/$1.png" >/dev/null 2>&1 ||
      xcrun simctl io "$SIM_UDID" screenshot --display=1 "$LOGS/$1.png" >/dev/null 2>&1 || true
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
  { for app in "${APPS[@]}"; do echo "${app#* }"; done
    # The set install-set-flow-test hands over is splits-test's app, which the library took from it.
    ! wanted install-set-flow-test || echo io.github.kisame76.minato.test.splits; } > "$CHECK/end"
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
    # The app's events of the session check ("<seconds since 1970> <pid> <event>"), which the guest log's
    # "app_process: start-times" line (the same clock) is read against.
    if [[ -f "$EVENTS" ]]; then cp "$EVENTS" "$LOGS/events"; fi
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
  # The guest log is the app's, not dex2oat's: no compile mode after a session, but in the COMPILE round.
  [[ -n "${COMPILE:-}" ]] || echo yes > "$CHECK/no-compile"
  # The menu that waits over the app at a return from the background chooses Continue after 1 s (the menu's pause).
  echo yes > "$CHECK/continue"
  [[ "$ON_DEVICE" == yes ]] || rm -f "$REQUEST"
  : > "$WORK/$app-$round.log"
  : > "$WORK/$app-$round.out"
  reset_events
  echo "$package" > "$CHECK/tap"
  echo yes > "$CHECK/guest-log"
  [[ "$app" != renpy-memory-test ]] || echo yes > "$CHECK/footprint-log"
  # A save's or a folder's alert answers Allow by itself, as the user's tap does (ADR-0057 step d).
  [[ "$app" != documents-tree-test ]] || echo yes > "$CHECK/pick-allow"
  # An app installer's alerts answer Allow and Install by themselves, as the user's taps do.
  [[ "$app" != install-flow-test ]] || echo yes > "$CHECK/install-allow"
  # The session flow's third start (the user declines the install) has the install alert answer Cancel.
  if [[ "$app" == install-session-flow-test ]]; then
    [[ $round == 3 ]] && echo yes > "$CHECK/install-deny" || echo yes > "$CHECK/install-allow"
  fi
  # The set flow's second start (the user declines the set) the same.
  if [[ "$app" == install-set-flow-test ]]; then
    [[ $round == 2 ]] && echo yes > "$CHECK/install-deny" || echo yes > "$CHECK/install-allow"
  fi
  # An app's request to remove an app (ADR-0077): the first start's alerts decline, the second's confirm.
  if [[ "$app" == uninstall-test ]]; then
    [[ $round == 1 ]] && echo yes > "$CHECK/install-deny" || echo yes > "$CHECK/install-allow"
  fi
  # The frame rate limit round's step, stored as Settings' choice at each launch (FRAME_RATE_LIMIT, set by the round).
  [[ -z "${FRAME_RATE_LIMIT:-}" ]] || echo "$FRAME_RATE_LIMIT" > "$CHECK/frame-rate-limit"
  # Only the virtual controller of the pad request reaches gamepad-test, not one the Simulator or the iPhone has.
  [[ "$app" != gamepad-test ]] || echo yes > "$CHECK/pad-only"
  # Minato's permission alert answers Allow by itself, as the user's tap does (ADR-0063 decision 2).
  case "$app" in
    permission-request-test-fine | permission-request-test-camera | permission-request-test-notifications)
      echo yes > "$CHECK/permission-allow" ;;
  esac
  # The pixel round (PIXELS): window.m's CRC lines in the guest log and the footprint, with PIXELS_DRAWABLES by the
  # drawables' pass, with PIXELS_CONTENTS by contents in the Simulator.
  if [[ -n "${PIXELS:-}" ]]; then
    echo yes > "$CHECK/window-crc"
    echo yes > "$CHECK/footprint-log"
    [[ -z "${PIXELS_DRAWABLES:-}" ]] || echo yes > "$CHECK/window-by-drawables"
    [[ -z "${PIXELS_CONTENTS:-}" ]] || echo yes > "$CHECK/window-by-contents"
    # PIXELS_TRACE: the window trace (graphics/host, Debug), shown()'s decision and who is above the buffers' sublayer
    # per post in the guest log, to name the second producer of the black-screen bug on the device.
    [[ -z "${PIXELS_TRACE:-}" ]] || echo yes > "$CHECK/window-trace"
  fi
  [[ -z "${QUIT_AFTER:-}" ]] || echo "$QUIT_AFTER" > "$CHECK/quit"
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
    sed 's/^/      | /' "$EVENTS" 2>/dev/null || true
    screenshot "$app-$round"
    return 1
  fi
}
# The start's files, kept as $WORK/$1-$2.log and .out, after Minato's end.
end_start() {
  local file
  terminate
  for file in guest-log guest-out; do fetch "$CHECK_REL/$file" || true; done
  cp "$CHECK/guest-log" "$WORK/$1-$2.log" 2>/dev/null || : > "$WORK/$1-$2.log"
  cp "$CHECK/guest-out" "$WORK/$1-$2.out" 2>/dev/null || : > "$WORK/$1-$2.out"
}
failed_line() {
  fail "  $1"
  screenshot "$2"
  sed 's/^/      | /' "$EVENTS" 2>/dev/null || true
  tail -n 40 "$CHECK/guest-log" 2>/dev/null | sed 's/^/      | /' || true
}
# The keyboard bar's views on screen in Minato's windows (SessionCheck's keyboard-views), asked for $1 seconds from
# now, into VIEWS: "<class> <x>,<y> <width>x<height>" each, "none", or "no answer"; FOCUS: yes while the session's
# text view holds the keyboard, else no.
VIEWS=""
FOCUS=""
keyboard_views() {
  sleep "$1"
  request keyboard-views
  if await_event "keyboard views .* focus (yes|no)" 10; then
    VIEWS=${EVENT#*keyboard views }
    FOCUS=${VIEWS##* focus }
    VIEWS=${VIEWS% focus *}
  else
    VIEWS="no answer"
    FOCUS=""
  fi
}
# After the step $1 (start $2 for failed_line), asked $3 seconds later (2 by default): without the focus nothing of
# the bar may be on screen (the red of device-regressions entry 2). Whether the keyboard stayed is UIKit's or the
# step's choice, not the bug: both are ok lines.
bar_with_focus() {
  keyboard_views "${3:-2}"
  if [[ "$FOCUS" == no && "$VIEWS" != none ]]; then
    failed_line "$1: the bar's views are on screen without the keyboard: $VIEWS" "$2"
  elif [[ "$FOCUS" == yes && "$VIEWS" == *KeyboardBarView* ]]; then
    ok "  $1: the keyboard is up with its bar"
  elif [[ "$FOCUS" == yes ]]; then
    ok "  $1: the keyboard is up, without a bar for this field"
  elif [[ "$FOCUS" == no ]]; then
    ok "  $1: the keyboard and its bar went together"
  else
    failed_line "$1: no answer to keyboard-views" "$2"
  fi
}
# The bar's views once the keyboard went ($1 the label, $2 the app and start for failed_line): none may stay.
no_keyboard_views() {
  keyboard_views 1.5
  if [[ "$VIEWS" == none ]]; then ok "  $1: nothing of the bar stayed on screen"; else
    failed_line "$1: the bar's views stayed on screen: $VIEWS" "$2"; fi
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
            # Game mode's 1 Hz memory picture (memory_watermark.h) in the session log, seconds after the start.
            local feet
            feet=$(grep -c '^I/apkrt *: mem foot=' "$CHECK/guest-log" 2>/dev/null || true)
            if (( ${feet:-0} >= 2 )); then
              ok "  the memory picture: $feet \"mem foot=\" lines in the session log"
            else
              fail "  the memory picture: ${feet:-0} \"mem foot=\" lines in the session log, not 2 or more"
            fi
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
  grep -E "^I/media-bench-test: " "$WORK/media-bench-test-1.log" | sed 's/^/note  /' || true
  if grep -q -E "^I/media-bench-test: (failed|.*no codec)" "$WORK/media-bench-test-1.log" ||
     grep -q -E "^I/media-bench-test: background .*images in the last 3 s no" "$WORK/media-bench-test-1.log"; then
    failures=$((failures + 1))
  fi
}

# A request of SessionCheck while the app runs: $1 is the file's name, on the iPhone sent into the container.
request() {
  if [[ "$ON_DEVICE" == yes ]]; then
    rm -rf "$WORK/one"
    mkdir "$WORK/one"
    echo yes > "$WORK/one/$1"
    devicectl_copy to --source "$WORK/one" --destination "$CHECK_REL"
  else
    echo yes > "$CHECK/$1"
  fi
}
# The share of the screen's sampled points $1 (a PNG) that are not black, in percent; -1 when it cannot be read.
lit_percent() {
  python3 - "$1" <<'PY'
import struct
import subprocess
import sys
import tempfile
import zlib

# sips scales the screenshot down, so that the decoding below stays small.
small = tempfile.mktemp(suffix=".png")
try:
    subprocess.run(["sips", "-Z", "96", sys.argv[1], "--out", small], check=True, capture_output=True)
    data = open(small, "rb").read()
    position, chunks, header = 8, [], None
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        body = data[position + 8:position + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            chunks.append(body)
        position += 12 + length
    width, height, depth, color = header[:4]
    channels = {2: 3, 6: 4}[color]
    assert depth == 8
    raw = zlib.decompress(b"".join(chunks))
    stride, previous, lit, total = width * channels, bytearray(width * channels), 0, 0
    for row in range(height):
        kind = raw[row * (stride + 1)]
        line = bytearray(raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                guess = left + up - corner
                near = min((abs(guess - left), left), (abs(guess - up), up), (abs(guess - corner), corner))[1]
                line[i] = (line[i] + near) & 255
        for x in range(width):
            pixel = line[x * channels:x * channels + 3]
            total += 1
            lit += max(pixel) > 24
        previous = line
    print(round(100 * lit / total))
except Exception:
    print(-1)
PY
}
# The checks after a return from the background (the resume round): the second ping, new frames for an app that moves,
# a screenshot that is not black, and the menu's answer. $1 the app, $2 the label of the case.
check_return() {
  local app=$1 wait_for=$2 presents_after presents_now lit
    presents_after=$(sed -E 's/.*presents ([0-9]+) .*/\1/' <<< "$EVENT")
    # The windows are made again at the return, so the counts before and after the background are not comparable:
    # the frames of the app after the return are those between this ping and the next.
    sleep 3
    request ping
    EVENT=""
    await_event "pong presents [0-9]+ scene .*" 15 || true
    screenshot "$app-resume-$wait_for"
    lit=-1
    [[ -z "${LOGS:-}" ]] || lit=$(lit_percent "$LOGS/$app-resume-$wait_for.png")
    if [[ -z "$EVENT" ]]; then
      failed_line "after $wait_for: the second ping got no answer" "$app-resume-$wait_for"
    elif [[ "$app" == surface-test || "$app" == views-test ]]; then
      echo "note  after $wait_for s: $app draws a still picture, so new frames say nothing"
    elif (( $(sed -E 's/.*presents ([0-9]+) .*/\1/' <<< "$EVENT") > presents_after )); then
      presents_now=$(sed -E 's/.*presents ([0-9]+) .*/\1/' <<< "$EVENT")
      ok "  after $wait_for: new frames ($presents_after to $presents_now)"
    else
      failed_line "after $wait_for: no new frames in 3 s ($presents_after)" "$app-resume-$wait_for"
    fi
    if (( lit < 0 )); then
      echo "note  after $wait_for s: no screenshot to judge"
    elif (( lit >= 2 )); then
      ok "  after $wait_for: the screen is not black ($lit % lit)"
    else
      failed_line "after $wait_for: the screen is black ($lit % lit)" "$app-resume-$wait_for"
    fi
    request menu
    if await_event "menu opened" 15; then
      ok "  after $wait_for: the menu answers"
    else
      failed_line "after $wait_for: no answer of the menu" "$app-resume-$wait_for"
    fi
}
# The resume round of app $1 (package $2), the user's bug of a black screen after leaving Minato and coming back: the
# app runs, Minato goes to the background for 15 s and again for 60 s (iOS suspends a background app), and after each
# return the host's main thread answers (ping), the guest's windows present new frames, the screen is not black, and the
# menu's request gets its answer. A touch is checked by hand: `motion` events in tmp/session-check/events.
# The guest may end by itself in the meantime (a run of the bench); that is no failure.
run_resume() {
  local app=$1 package=$2 wait_for presents_before presents_after lit
  if ! begin_start "$app" "$package" 1; then end_start "$app" 1; return; fi
  # Godot's bench ends by itself after about 14 s of frames, so it gets one short background trip.
  local settle=${RESUME_SETTLE:-8} waits=${RESUME_WAITS:-15 60}
  case "$app" in godot-bench-*) settle=${RESUME_SETTLE:-3} waits=${RESUME_WAITS:-15} ;; esac
  sleep "$settle"
  request ping
  if ! await_event "pong presents [0-9]+ scene .*" 15; then
    failed_line "no pong before the background: the host's main thread does not answer" "$app-resume"
    fetch_events
    cp "$EVENTS" "$WORK/$app-resume-events.out" 2>/dev/null || true
    end_start "$app" 1
    return
  fi
  presents_before=$(sed -E 's/.*presents ([0-9]+) .*/\1/' <<< "$EVENT")
  ok "  running: $presents_before presents"
  # shellcheck disable=SC2086
  for wait_for in $waits; do
    launch "$AWAY"
    sleep "$wait_for"
    launch
    sleep 3
    request ping
    if ! await_event "pong presents [0-9]+ scene .*" 20; then
      if await_event "(process |session ended|program ended).*" 1; then
        ok "  after $wait_for s in the background: the app ended by itself"
      else
        failed_line "after $wait_for s in the background no pong: the host's main thread is stuck" "$app-resume"
      fi
      break
    fi
    check_return "$app" "$wait_for s in the background"
    sleep 2
  done
  fetch_events
  cp "$EVENTS" "$WORK/$app-resume-events.out" 2>/dev/null || true
  end_start "$app" 1
}

# More cases of the resume round (RESUME_CASES=1, after run_resume's): `continue` is requested, so that every return
# closes the menu that waits over the app after 1 s, as the user's Continue does. For app $1 (package $2): (1) the
# three-finger menu open when Minato goes to the background for 15 s; (2) two short trips in a row (3 s away, 2 s
# back, twice); (3) a trip 1 s after the start. Each one checks as run_resume does.
ping_again() {
  request ping
  EVENT=""
  await_event "pong presents [0-9]+ scene .*" 20 || true
}
run_resume_cases() {
  local app=$1 package=$2 trip
  if ! begin_start "$app" "$package" 1; then end_start "$app" 1; return; fi
  sleep "${RESUME_SETTLE:-8}"
  request continue
  ping_again
  if [[ -z "$EVENT" ]]; then
    failed_line "no pong before the cases: the host's main thread does not answer" "$app-resume-cases"
    end_start "$app" 1
    return
  fi
  request menu-hold
  await_event "menu held" 15 || failed_line "the menu did not open to be held" "$app-resume-cases"
  launch "$AWAY"
  sleep 15
  launch
  sleep 4
  # The menu that was open stays open over the paused app (Continue starts it again): the menu request closes it.
  request menu
  await_event "menu opened" 15 || true
  sleep 4
  ping_again
  check_return "$app" "the three-finger menu open in the background"
  for trip in 1 2; do
    launch "$AWAY"
    sleep 3
    launch
    sleep 2
  done
  ping_again
  check_return "$app" "two short trips in a row"
  fetch_events
  cp "$EVENTS" "$WORK/$app-resume-cases-events.out" 2>/dev/null || true
  end_start "$app" 1
  if ! begin_start "$app" "$package" 1; then end_start "$app" 1; return; fi
  sleep 1
  request continue
  launch "$AWAY"
  sleep 5
  launch
  sleep "${RESUME_EARLY_WAIT:-20}"
  ping_again
  check_return "$app" "a trip 1 s after the start"
  fetch_events
  cp "$EVENTS" "$WORK/$app-resume-cases-early-events.out" 2>/dev/null || true
  end_start "$app" 1
}

# Import the apps; the Simulator signs a ready app's code ad hoc at its first tap.
terminate
rm -rf "$CHECK"
mkdir -p "$CHECK"
[[ "$ON_DEVICE" == yes ]] || rm -f "$REQUEST"
reset_events
for app in "${APPS[@]}"; do
  # packs-test's and pad-test's sets come as unit folders of their own (pick_set), as the library takes only APKs
  # lying flat here.
  case "${app%% *}" in packs-test|pad-test|splits-test|isolated-splits-test|obb-large-test) continue ;; esac
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
# obb-large-test's .xapk (an APK and a main OBB of 2 GiB and 128 MiB, a few MiB packed), a unit folder that appears whole.
pick_large_xapk() {
  rm -rf "$WORK/unit-obb-large" && mkdir -p "$WORK/unit-obb-large"
  python3 tools/make-large-xapk.py "$(apk obb-large-test)" "$WORK/unit-obb-large/obb-large-test.xapk"
  LARGE_XAPK_START=$SECONDS
  if [[ "$ON_DEVICE" == yes ]]; then
    devicectl_copy to --source "$WORK/unit-obb-large" --destination "$SUPPORT_REL/incoming/obb-large-test-xapk"
    return
  fi
  local unit="${SUPPORT:?}/incoming/obb-large-test-xapk"
  mkdir "$unit.partial"
  cp "$WORK/unit-obb-large/obb-large-test.xapk" "$unit.partial/"
  mv "$unit.partial" "$unit"
}
if listed obb-large-test; then pick_large_xapk; fi
send_requests
launch
for app in "${APPS[@]}"; do
  package=${app#* }
  end=$((SECONDS + 60))
  # The inflation of obb-large-test's 2.2 GB OBB takes longer than the other apps' imports.
  [[ "${app%% *}" != obb-large-test ]] || end=$((SECONDS + 600))
  while (( SECONDS < end )); do
    [[ "$(field state "$package")" == "{'ready': {}}" ]] && break
    sleep 0.2
  done
  state=$(field state "$package")
  if [[ "$state" == "{'ready': {}}" ]]; then ok "$package is ready"; else fail "$package is not ready: $state"; fi
  # No entry at all after the first app's minute: Minato imported nothing, as when an iOS alert over the library
  # holds it up; the run stops here rather than wait a minute for each app.
  if [[ "$app" == "${APPS[0]}" && "$state" != "{'ready': {}}" ]] && python3 -c 'import json,sys
try: entries = json.load(open(sys.argv[1])).get("entries")
except (OSError, ValueError): entries = None
sys.exit(1 if entries else 0)' "$SUPPORT/library.json"; then
    screenshot not-ready
    fail "Minato imported nothing within 60 s: an iOS alert may be open over the library${LOGS:+, see the screenshot}"
    [[ -z "${LOGS:-}" ]] || echo "      $LOGS/not-ready.png"
    terminate
    echo "$failures failed"
    exit 1
  fi
done

# The pixel round (PIXELS, ADR-0043 b5's step f). A screenshot into $1, of the iPhone's screen or the Simulator's
# first display.
capture() {
  if [[ "$ON_DEVICE" == yes ]]; then
    on_device capture screenshot --device "$DEVICE_ID" -q --destination "$1" >/dev/null 2>&1
  else
    xcrun simctl io "$SIM_UDID" screenshot "$1" >/dev/null 2>&1 ||
      xcrun simctl io "$SIM_UDID" screenshot --display=1 "$1" >/dev/null 2>&1
  fi
}
# Checkpoint $2 of app $1: the screen and the windows' dump, compared (tools/window_pixels.py); with $3, a window of
# the dump must have that format.
pixels_checkpoint() {
  local app=$1 name=$2 dir="$PIXELS/$1/$2"
  rm -rf "${dir:?}"
  mkdir -p "$dir"
  request window-dump
  capture "$dir/screen.png" || { fail "  $name: no screenshot"; return; }
  if ! await_event "window dump -?[0-9]+" 15; then
    failed_line "$name: no window dump" "$app-$name"
    return
  fi
  if (( ${EVENT##* } <= 0 )); then
    fail "  $name: the dump wrote ${EVENT##* } windows"
    return
  fi
  if [[ "$ON_DEVICE" == yes ]]; then
    devicectl_copy from --source "$CHECK_REL/window-dump-files" --destination "$dir/dump" || {
      fail "  $name: the dump could not be fetched"
      return
    }
  else
    cp -R "$CHECK/window-dump-files" "$dir/dump"
  fi
  # On an opaque layer, a NativeActivity's, RGBA_8888 shows on black too.
  if [[ -n "${3:-}" ]] && ! grep -q -E " ${3% on black}( on black)? post " "$dir"/dump/window-*.txt; then
    fail "  $name: no window of the dump is $3"
  fi
  if python3 tools/window_pixels.py screen "$dir/dump" "$dir/screen.png" >"$dir/compare.txt" 2>&1; then
    ok "  $name: the screen shows each window's buffer"
  else
    fail "  $name: the screen differs from the windows' buffers"
  fi
  sed 's/^/      | /' "$dir/compare.txt" 2>/dev/null || true
}
# Home and the return: back once the menu's Continue came, which starts the app again as Android does.
pixels_return() {
  launch "$AWAY"
  sleep 3
  launch
  await_event "menu continued" 30 || { fail "  no Continue after the return"; return 1; }
}
# Waits up to $2 seconds for a second line of the guest log that matches the extended regex $1.
await_second() {
  local end=$((SECONDS + $2))
  while (( SECONDS < end )); do
    fetch "$CHECK_REL/guest-log" || true
    (( $(count_lines "$1" "$CHECK/guest-log") >= 2 )) && return 0
    sleep 0.2
  done
  return 1
}
# App $1 (package $2) through the pixel round.
run_pixels() {
  local app=$1 package=$2
  if begin_start "$app" "$package" 1; then
    if [[ "$app" == window-formats-test ]]; then
      local format
      for format in RGBA_8888 RGBX_8888 RGB_565; do
        if ! await_line guest-log "^I/window-formats-test: showing $format$" 60; then
          failed_line "no \"showing $format\"" "$app-$format"
          break
        fi
        case "$format" in RGBA_8888) pixels_checkpoint "$app" "$format" "$format" ;;
          *) pixels_checkpoint "$app" "$format" "$format on black" ;;
        esac
      done
      if await_line guest-log "^I/window-formats-test: waiting for the window to go$" 30 && pixels_return; then
        # RGB_565 shown again, on the window of the return.
        if await_second "^I/window-formats-test: showing RGB_565$" 30; then
          pixels_checkpoint "$app" return "RGB_565 on black"
        else
          failed_line "no RGB_565 again after the return" "$app-return"
        fi
        await_line guest-log "^I/window-formats-test: android_main returns$" 30 || failed_line "no end" "$app-end"
      fi
    elif [[ "$app" == second-windows-test ]]; then
      # Without the Mac host's taps it stops at its first dialog, dimmed over the activity: the two windows'
      # checkpoint, then Home and the return.
      if await_line guest-log "^I/second-windows-test: ready: dialog 1, " 120; then
        pixels_checkpoint "$app" start
        if pixels_return; then
          sleep 2
          pixels_checkpoint "$app" return
        fi
      else
        # Without the session's window handler (RuntimeSession.swift, ADR-0041 gap 4) a dialog's window request
        # answers -EOPNOTSUPP, which ViewRootImpl takes in silence, and the dialog never shows.
        failed_line "no \"ready: dialog 1\": the dialog's window never came (the session's window handler, gap 4)" \
          "$app-start"
      fi
    elif await_line guest-log "^I/$app: done$" 300; then
      sleep 2
      pixels_checkpoint "$app" start
      if pixels_return; then
        sleep 2
        pixels_checkpoint "$app" return
      fi
    else
      failed_line "no done" "$app-start"
    fi
    fetch "$CHECK_REL/footprint-log" || true
    mkdir -p "$PIXELS/$app"
    cp "$CHECK/footprint-log" "$PIXELS/$app/footprint-log" 2>/dev/null || true
  fi
  end_start "$app" 1
  mkdir -p "$PIXELS/$app"
  cp "$WORK/$app-1.log" "$PIXELS/$app/guest-log"
  # window-formats-test's lines, Home and the return included, must be Android 15's (run_one's comparison).
  if [[ "$app" == window-formats-test ]]; then
    python3 - "$app" "$WORK/$app-1.log" "fixtures/expected/$app.logcat" <<'PY' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

app, log, expected = sys.argv[1:]
compare_log(app, subprocess.CompletedProcess([], 0, "", open(log, encoding="utf-8", errors="replace").read()), expected,
            {})
PY
  fi
}
if [[ -n "${PIXELS:-}" ]]; then
  for app in "${APPS[@]}"; do
    run_pixels "${app%% *}" "${app#* }"
  done
  echo "java start check: $failures failed"
  exit $((failures > 0))
fi

# RESUME=1 runs only the resume round, for the apps ONLY names (default: the four that draw in the Simulator).
if [[ -n "${RESUME:-}" ]]; then
  for app in "${APPS[@]}"; do
    if [[ -n "${RESUME_CASES:-}" ]]; then
      run_resume_cases "${app%% *}" "${app#* }"
    else
      run_resume "${app%% *}" "${app#* }"
    fi
  done
  echo "java start check: $failures failed"
  exit $((failures > 0))
fi

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

# workmanager-test (androidx.work 2.12.0): launch 1 enqueues unique delayed work, and launch 2, after Minato ended and
# the process with it, finds it ENQUEUED in WorkManager's Room database in the app's data folder. Both starts' lines
# must be Android 15's (fixtures/expected/workmanager-test.logcat, 50 lines: "kept work none", then "kept work 1,
# ENQUEUED" and "2 tagged"). Room's SQLite in Minato's sandbox paths is what the Mac cannot show.
if wanted workmanager-test; then
  run_start workmanager-test io.github.kisame76.minato.test.workmanager 1
  run_start workmanager-test io.github.kisame76.minato.test.workmanager 2
  python3 - "$WORK/workmanager-test-1.log" "$WORK/workmanager-test-2.log" \
    fixtures/expected/workmanager-test.logcat <<'EOF' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

first, second, expected = sys.argv[1:]
log = "".join(open(path, encoding="utf-8", errors="replace").read() for path in (first, second))
compare_log("workmanager-test", subprocess.CompletedProcess([], 0, "", log), expected, {})
EOF
fi

# ADR-0057 step d: documents-tree-test opens Documents, allowed in Minato's alert, and makes a file there; its second
# start reads it through the grant it kept. Both starts' lines must be Android 15's.
if wanted documents-tree-test; then
  run_start documents-tree-test io.github.kisame76.minato.test.documentstree 1
  if grep -q " pick allowed$" "$EVENTS"; then
    ok "  the folder's alert came and was allowed"
  else
    fail "  no folder alert allowed: $(grep -o "pick alert .*" "$EVENTS" | head -n 1)"
  fi
  run_start documents-tree-test io.github.kisame76.minato.test.documentstree 2
  python3 - "$WORK/documents-tree-test-1.log" "$WORK/documents-tree-test-2.log" \
    fixtures/expected/documents-tree-test.logcat <<'EOF' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

first, second, expected = sys.argv[1:]
log = "".join(open(path, encoding="utf-8", errors="replace").read() for path in (first, second))
compare_log("documents-tree-test", subprocess.CompletedProcess([], 0, "", log), expected, {})
EOF
fi

# orientation-request-test: each activity asks for landscape, then portrait, with setRequestedOrientation, which the
# session turns for (APKRT_REQUEST_ORIENTATION). Its lines must be Android 15's, whose sizes are the AVD's screen, the
# iPhone 15 Pro Max's: on another screen its portrait sizes in dp and pixels take their place, and the landscape's
# must be them swapped.
if wanted orientation-request-test; then
  run_start orientation-request-test io.github.kisame76.minato.test.orientationrequest 1
  python3 - "$WORK/orientation-request-test-1.log" fixtures/expected/orientation-request-test.logcat "$WORK" <<'EOF' \
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
tag = "I/orientation-request-test: "
dp = re.search(rf"^{tag}plain 1 onCreate, saved none, orientation 1, (\d+) x (\d+) dp", log, re.M)
pixels = re.search(rf"^{tag}plain 3 in portrait: decor (\d+) x (\d+),", log, re.M)
if dp and pixels and (dp.groups(), pixels.groups()) != (("430", "932"), ("1290", "2796")):
    (w, h), (pw, ph) = dp.groups(), pixels.groups()
    print(f"note    the screen is {pw} x {ph} pixels, {w} x {h} dp, which take the place of Android's")
    for old, new in (("430 x 932", f"{w} x {h}"), ("932 x 430", f"{h} x {w}"),
                     ("1290 x 2796", f"{pw} x {ph}"), ("2796 x 1290", f"{ph} x {pw}")):
        wanted = wanted.replace(old, new)
ours = os.path.join(work, "orientation-request-test.logcat")
with open(ours, "w", encoding="utf-8") as handle:
    handle.write(wanted)
compare_log("orientation-request-test", subprocess.CompletedProcess([], 0, "", log), ours, {})
EOF
fi

# gamepad-test (ADR-0068 decision 5): once it is ready, SessionCheck's pad request connects a hidden GCVirtualController
# and plays tools/gamepad-check.sh's steps on it, which the app gets through GameController as a real pad's. Its lines
# must be Android 15's for the AVD's uinput Xbox pad, with the virtual pad's name, vendor and product in the device
# line, which GameController reports for it.
if wanted gamepad-test; then
  if begin_start gamepad-test io.github.kisame76.minato.test.gamepad 1; then
    if await_line guest-log "^I/gamepad-test: ready: pad$" 300; then
      request pad
      if await_line guest-log "^I/gamepad-test: done$" 60; then
        ok "  done, $(seconds "$TAP" "$SEEN") s after the tap"
        sleep 2
      else
        failed_line "no done after the pad" gamepad-test-1
      fi
    else
      failed_line "no ready line" gamepad-test-1
    fi
  fi
  end_start gamepad-test 1
  fetch_events
  grep -E " (virtual pad|pad (dis)?connected|pad ignored)" "$EVENTS" | sed 's/^[0-9.]* [0-9]* /note    /' || true
  python3 - "$WORK/gamepad-test-1.log" fixtures/expected/gamepad-test.logcat "$WORK" "$EVENTS" <<'EOF' \
    || failures=$((failures + 1))
import os
import re
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

path, expected, work, events = sys.argv[1:]
log = open(path, encoding="utf-8", errors="replace").read()
wanted = open(expected, encoding="utf-8").read()
# A button the virtual pad lacks has no lines (a touch controller has no home button and no stick clicks).
codes = {"Button Home": "BUTTON_MODE", "Left Thumbstick Button": "BUTTON_THUMBL",
         "Right Thumbstick Button": "BUTTON_THUMBR"}
for element in re.findall(r" virtual pad lacks (.+)$", open(events, encoding="utf-8").read(), re.M):
    code = codes.get(element)
    if code is None:
        continue
    print(f"note    the virtual pad has no {element}: no KEYCODE_{code} lines")
    wanted = "".join(line for line in wanted.splitlines(True) if f" KEYCODE_{code}," not in line)
android = 'added: name "Minato Test Pad", sources 0x1000511, vendor 0x45e, product 0x2e0,'
ours = re.search(r'^I/gamepad-test: (added: name ".*", sources 0x1000511, vendor 0x[0-9a-f]+, product 0x[0-9a-f]+,)',
                 log, re.M)
if ours:
    print(f"note    the virtual pad's device line: {ours.group(1)}")
    wanted = wanted.replace(android, ours.group(1))
mine = os.path.join(work, "gamepad-test.logcat")
with open(mine, "w", encoding="utf-8") as handle:
    handle.write(wanted)
compare_log("gamepad-test", subprocess.CompletedProcess([], 0, "", log), mine, {})
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
# ADR-0063 in the Simulator: iOS's location answer is granted beforehand (simctl privacy), and its position is the one
# tools/android-reference.sh gives the emulator, 52.52 N, 13.405 E. permission-request-test-fine asks for the location
# group, Minato's alert answers Allow by itself (permission-allow), and its log must be Android 15's with both granted.
# location-manager-test-fine and -coarse start with the grant their names say, as the AVD's pm grant gives it: the
# host's record of the app says so before the start, and Minato seeds the runtime from it; their fixes come from
# CoreLocation, and their logs must be Android 15's.
if [[ "$ON_DEVICE" == no ]] && { wanted permission-request-test-fine || wanted location-manager-test-fine ||
    wanted location-manager-test-coarse; }; then
  xcrun simctl privacy "$SIM_UDID" grant location "$BUNDLE_ID"
  xcrun simctl location "$SIM_UDID" set 52.52,13.405
  if wanted permission-request-test-fine; then
    # A fresh app for the host: no answer stored from an earlier run.
    rm -f "$SUPPORT/data/io.github.kisame76.minato.test.permreqfine/permissions.json"
    run_one permission-request-test-fine io.github.kisame76.minato.test.permreqfine
    fetch_events
    # The app asks the host once: its second request finds both permissions granted, as on Android.
    answers=$(count_lines " permission [0-9]+ group 1 outcome 1$" "$EVENTS")
    if [[ "$answers" == 1 ]]; then
      ok "  the host granted the location request fine"
    else
      fail "  the host granted $answers location requests fine, not 1"
    fi
  fi
  for twin in fine:1 coarse:2; do
    app=location-manager-test-${twin%:*}
    wanted "$app" || continue
    package=io.github.kisame76.minato.test.locman${twin%:*}
    mkdir -p "$SUPPORT/data/$package"
    echo "{\"location\":{\"outcome\":${twin#*:},\"denials\":0}}" > "$SUPPORT/data/$package/permissions.json"
    run_one "$app" "$package"
  done
fi

# ADR-0073 decision 8 in the Simulator: permission-request-test-camera asks for CAMERA, Minato's alert answers Allow by
# itself (permission-allow), and iOS's camera answer is granted beforehand (simctl privacy); its log must be Android
# 15's, where the harness grants CAMERA before the start.
if [[ "$ON_DEVICE" == no ]] && wanted permission-request-test-camera; then
  xcrun simctl privacy "$SIM_UDID" grant camera "$BUNDLE_ID"
  # A fresh app for the host: no answer stored from an earlier run.
  rm -f "$SUPPORT/data/io.github.kisame76.minato.test.permreqcamera/permissions.json"
  run_one permission-request-test-camera io.github.kisame76.minato.test.permreqcamera
  fetch_events
  # The app asks the host once: its second request finds CAMERA granted, as on Android.
  answers=$(count_lines " permission [0-9]+ group 3 outcome 1$" "$EVENTS")
  if [[ "$answers" == 1 ]]; then
    ok "  the host granted the camera request"
  else
    fail "  the host granted $answers camera requests, not 1"
  fi
fi

# The MinatoUITests runner (app/UITests, its own scheme), which drives SpringBoard: built once per run, when a round
# needs it; false when it does not build.
UITESTS=build/DerivedData-uitests
uitests_built=""
uitests_build() {
  if [[ -z "$uitests_built" ]]; then
    local destination="generic/platform=iOS Simulator"
    [[ "$ON_DEVICE" != yes ]] || destination="generic/platform=iOS"
    # The project as project.yml has it now, with the runner's target and the team that signs it (tools/gen.sh).
    # Xcode asks Apple (the signed-in account) only when a profile is about to run out; the Simulator needs none.
    local runner_signing=""
    [[ "$ON_DEVICE" != yes ]] || runner_signing=$(tools/provisioning-updates.sh io.github.kisame76.minato \
      io.github.kisame76.minato.uitests.xctrunner)
    if tools/gen.sh > "$WORK/uitests-build.log" 2>&1 &&
        xcodebuild build-for-testing -project Minato.xcodeproj -scheme MinatoUITests -destination "$destination" \
        -derivedDataPath "$UITESTS" $runner_signing -quiet >> "$WORK/uitests-build.log" 2>&1; then
      uitests_built=yes
    elif grep -q -E "No Accounts|No Account for Team|requires a development team" "$WORK/uitests-build.log"; then
      # Xcode signs the runner for the iPhone with the account signed in to it; without one its rounds cannot run.
      # Not a test's failure, but no pass either: the run still ends with exit 1.
      uitests_built=noaccount
      echo "skipped  the UI test runner needs an Xcode account (Xcode > Settings > Accounts); its rounds do not run"
      failures=$((failures + 1))
    else
      uitests_built=no
      fail "  the UI test runner did not build: $(grep -m 1 'error:' "$WORK/uitests-build.log")"
    fi
  fi
  [[ "$uitests_built" == yes ]]
}
# Set once the runner could not start, because iOS's UI automation is off or the screen is locked (the iPhone's
# Settings > Developer > Enable UI Automation), or because its build is older than the device's iOS: uitest's one FAIL
# line says which, later runner tests are not tried, and the rounds that need the runner leave out their own FAIL
# lines (runner_fail, runner_failed_line).
RUNNER_DOWN=""
runner_fail() { [[ -n "$RUNNER_DOWN" ]] || fail "$1"; }
runner_failed_line() { [[ -n "$RUNNER_DOWN" ]] || failed_line "$@"; }
# Runs the runner's test $1 against this run's Simulator or iPhone, with TEST_RUNNER_MINATO_WAIT=$3 seconds (for a
# banner or an alert) and TEST_RUNNER_MINATO_ARG=$4 (the test's own argument); prints its notes, and its failure,
# false when it failed. $2 names the round's log.
uitest() {
  local run
  [[ -z "$RUNNER_DOWN" ]] || return 1
  run=$(ls "$UITESTS"/Build/Products/*.xctestrun | head -n 1)
  local started=$SECONDS status=0
  stamp "uitest $1 $2 begins"
  TEST_RUNNER_MINATO_WAIT=${3:-0} TEST_RUNNER_MINATO_ARG=${4:-} xcodebuild test-without-building -xctestrun "$run" \
      -destination "id=${DEVICE_UDID:-${SIM_UDID:-}}" \
      -only-testing:"MinatoUITests/NotificationTapUITests/$1" > "$WORK/uitest-$1-$2.log" 2>&1 || status=$?
  stamp "uitest $1 $2 ends: exit $status after $((SECONDS - started)) s"
  if [[ $status == 0 ]]; then
    grep -o "MINATO-UITEST .*" "$WORK/uitest-$1-$2.log" | sed 's/^MINATO-UITEST/note   /' || true
    return 0
  fi
  if grep -q -E "enabling automation mode|Failed to initialize for UI testing" "$WORK/uitest-$1-$2.log"; then
    RUNNER_DOWN=yes
    fail "  the iPhone's UI automation is off or the screen is locked (Settings > Developer > Enable UI Automation)"
    return 1
  fi
  if grep -q -E "needs to be updated|muss aktualisiert werden" "$WORK/uitest-$1-$2.log"; then
    RUNNER_DOWN=yes
    fail "  the runner's build is stale: delete $UITESTS and run again"
    return 1
  fi
  grep -E "error: |XCTAssert|Failing tests" "$WORK/uitest-$1-$2.log" | head -n 3 | sed 's/^/      | /' || true
  return 1
}

# ADR-0074 decision 1 in the Simulator: permission-request-test-notifications asks for POST_NOTIFICATIONS, Minato's
# alert answers Allow by itself (permission-allow); iOS's own prompt, which simctl privacy does not cover, comes in a
# Simulator that never answered it, and the MinatoUITests runner answers it Allow (later runs find the answer). An
# alert of SpringBoard's still up at the round's end is dismissed, with a line: it would outlive Minato's end and
# hold up the next run's library. Its log must be Android 15's, where the harness grants POST_NOTIFICATIONS before
# the start.
if [[ "$ON_DEVICE" == no ]] && wanted permission-request-test-notifications; then
  # A fresh app for the host: no answer stored from an earlier run.
  rm -f "$SUPPORT/data/io.github.kisame76.minato.test.permreqnotifications/permissions.json"
  allow=""
  if uitests_build; then
    uitest testAllowAlert notifications 45 > "$WORK/allow.out" 2>&1 &
    allow=$!
  fi
  run_one permission-request-test-notifications io.github.kisame76.minato.test.permreqnotifications
  if [[ -n "$allow" ]]; then
    if wait "$allow"; then ok "  the runner answered iOS's notifications prompt Allow"; else
      echo "note    no iOS notifications prompt came (Minato's answer is iOS's already)"; fi
    uitest testDismissAlert notifications
  fi
  fetch_events
  # The app asks the host once: its second request finds POST_NOTIFICATIONS granted, as on Android.
  answers=$(count_lines " permission [0-9]+ group 4 outcome 1$" "$EVENTS")
  if [[ ! -f "$EVENTS" ]]; then
    # Minato's session-check folder went during the round: another check on this Simulator, or Minato reinstalled.
    fail "  no events from Minato (another check on this Simulator?)"
  elif [[ "$answers" == 1 ]]; then
    ok "  the host granted the notifications request"
  else
    fail "  the host granted $answers notifications requests, not 1"
  fi
fi

# ADR-0074 decision 2 in the Simulator: notify-test has no grant, so nothing is active and the host shows nothing;
# notify-test-granted starts with the grant, as the AVD's pm grant gives it (the host's record of the app says so, and
# iOS's answer for Minato is granted from the -notifications round), so its posts reach UNUserNotificationCenter. Both
# logs must be Android 15's.
if [[ "$ON_DEVICE" == no ]] && { wanted notify-test || wanted notify-test-granted; }; then
  if wanted notify-test; then
    rm -f "$SUPPORT/data/io.github.kisame76.minato.test.notify/permissions.json"
    run_one notify-test io.github.kisame76.minato.test.notify
    fetch_events
    if grep -q -E " notification posted " "$EVENTS"; then fail "  the host posted without the grant"; fi
  fi
  if wanted notify-test-granted; then
    mkdir -p "$SUPPORT/data/io.github.kisame76.minato.test.notifygranted"
    echo '{"notifications":{"outcome":1,"denials":0}}' \
      > "$SUPPORT/data/io.github.kisame76.minato.test.notifygranted/permissions.json"
    run_one notify-test-granted io.github.kisame76.minato.test.notifygranted
    fetch_events
    posts=$(count_lines " notification posted " "$EVENTS")
    cancels=$(count_lines " notification cancelled " "$EVENTS")
    if [[ "$posts" == 3 && "$cancels" == 1 ]]; then
      ok "  the host posted 3 notifications and cancelled 1"
    else
      fail "  the host posted $posts notifications and cancelled $cancels, not 3 and 1"
    fi
    # What iOS holds (ADR-0074 decision 3, the N2 hand check made a check): its delivered list after each post and
    # cancel must step as Android's active list does in the reference log (the repost replaces, the post on a missing
    # channel adds nothing, the cancel and the cancel of all remove), without Android's ranker_group summary, which is
    # not mapped, and with the reference's repeats folded. The host reads iOS's list once per post or cancel; a read
    # the guest's next request overtook (a busy Mac) says "late" and proves nothing for its step, so one may be late.
    # In front, HIGH ("messages") shows a banner, the list and a sound, LOW ("quiet") the list only. The Debug host
    # logs these only while a check runs, for its test apps alone.
    reference=fixtures/expected/notify-test-granted.logcat
    want=$(sed -n -E 's/^I\/notify-test-granted: .*: active \[(.*)\]$/\1/p' "$reference" | python3 -c 'import sys
for line in sys.stdin:
    items = [i for i in line.rstrip("\n").split(", ") if i and not i.startswith("ranker_group|")]
    print("[" + ", ".join(sorted(items)) + "]")' | awk '{ if ($0 != last) print; last = $0 }')
    got=$(sed -n -E 's/^[0-9.]+ [0-9]+ notifications delivered (\[.*\]|late)$/\1/p' "$EVENTS")
    late=$(grep -c -x late <<<"$got" || true)
    if [[ -n "$want" && "$(wc -l <<<"$got")" == "$(wc -l <<<"$want")" && "$late" -le 1 ]] &&
      paste -d '\t' <(echo "$got") <(echo "$want") | awk -F '\t' '$1 != "late" && $1 != $2 { bad = 1 } END { exit bad }'
    then
      ok "  iOS's list stepped as Android's active list: $(echo "$want" | tr '\n' ' ')"
      [[ "$late" == 0 ]] ||
        echo "note    one read of iOS's list came after the next request: $(echo "$got" | tr '\n' ' ')"
    else
      fail "  iOS's list stepped $(echo "${got:-(no lines)}" | tr '\n' ' '), not $(echo "$want" | tr '\n' ' ')"
    fi
    want="chat|7 banner list sound,chat|7 banner list sound,null|8 list"
    got=$(sed -n -E 's/^[0-9.]+ [0-9]+ notification presented (.*)$/\1/p' "$EVENTS" | paste -s -d , -)
    if [[ "$got" == "$want" ]]; then ok "  in front: the HIGH posts with a banner, the LOW one in the list only"; else
      fail "  in front: \"${got:-(no lines)}\", not \"$want\""; fi
  fi
fi

# The seconds from the tapping process's last restart to the game mode process's first active scene, from the events,
# or "none": a restart that iOS answers while it still brings Minato to the front starts no process (measured on the
# iPhone), so game mode must be active within seconds of it.
active_after_restart() {
  [[ -f "$EVENTS" ]] || { echo none; return 0; }
  awk '$3 == "restart" && ($4 == "workspace" || $4 == "ownURL") { at = $1 }
    $3 == "process" && $4 == "gameMode" { game = $2 }
    game != "" && $2 == game && $3 == "scene" && $4 == "didActivate" && !done { printf "%.1f\n", $1 - at; done = 1 }
    END { if (!done) print "none" }' "$EVENTS"
}
# Minato in its library with notification-response-save: it keeps the response to notify-tap-test's delivered
# notification for the next tap request, which then hands it to the delegate as fast as iOS's didReceive comes.
save_response() {
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  echo yes > "$CHECK/no-compile"
  echo io.github.kisame76.minato.test.notifytap > "$CHECK/notification-response-save"
  reset_events
  send_requests
  launch
  if await_line events " notification response saved$" 20; then ok "  $1: the tap's response is kept"; else
    fail "  $1: the tap's response was not kept"; fi
}
# Whether TapActivity of the tap $1 got its content intent's extras as Android gives them (a nested Bundle, a String[]
# and an int[], a Uri, an Intent, a ParcelUuid): its "the tap's extras" line must be the reference log's. A process
# that restarts for the tap gets the intent from the notification's store, which kept only strings, numbers and
# booleans before 2026-11-10. The start's first such line is the tap's: a second later the new process's Application
# sends its own content intent, in the process, with every extra.
tap_extras() {
  local prefix="I/notify-tap-test: the tap's extras: " want got
  want=$(grep -m 1 -F "$prefix" fixtures/expected/notify-tap-test.logcat || true)
  fetch "$CHECK_REL/guest-log" || true
  got=$(grep -m 1 -F "$prefix" "$CHECK/guest-log" 2>/dev/null || true)
  if [[ -n "$want" && "$got" == "$want" ]]; then ok "  $1: the content intent's extras are Android's"; else
    fail "  $1: the content intent's extras: \"${got#"$prefix"}\", not \"${want#"$prefix"}\""
  fi
}
# The round's precondition: the tap reached the delegate before Minato was active, and so did the restart, or its
# wait for active with the fix. Only then does the round stand for the user's tap; a restart that came at active
# proves nothing. Prints the round's events of the tap and the restart.
tap_precondition() {
  grep -E " (notification (tap|cold tap|foreground tap|launch)|restart|exit|process|launched|scene) " "$EVENTS" |
    sed 's/^/      ev /' || true
  local verdict="not met: no events"
  [[ ! -f "$EVENTS" ]] || verdict=$(awk '$3 == "notification" && $4 == "tap" && $5 == "state" { tap = $6; tapper = $2 }
    tap != "" && !seen && $3 == "restart" && $4 == "waits" { verdict = "met: tap at " tap ", restart waited"; seen = 1 }
    tap != "" && !seen && $3 == "restart" && $4 == "state" {
      verdict = ($5 == "active" ? "not met" : "met") ": tap at " tap ", restart at " $5; seen = 1 }
    tap != "" && $2 == tapper && $3 == "scene" && $4 == "didActivate" { active = 1 }
    # A build from before the state events logs only the restart, by its way: before or after the scene was active.
    tap != "" && way == "" && $3 == "restart" && ($4 == "workspace" || $4 == "ownURL") {
      way = (active ? "not met" : "met") ": tap at " tap ", restart " (active ? "after" : "before") " active" }
    END { if (tap == "") print "not met: no tap"; else if (seen) print verdict; else if (way != "") print way
      else print "not met: no restart" }' \
    "$EVENTS")
  if [[ "$verdict" == met:* && "$verdict" != *"tap at active"* ]]; then
    ok "  $1: precondition $verdict"
  elif [[ "$ON_DEVICE" == yes ]]; then
    fail "  $1: precondition $verdict (the round does not stand for the user's tap)"
  else
    # The Simulator restarts too fast for the old bug's state: a note there, the iPhone's round decides.
    echo "note      $1: precondition $verdict (in the Simulator; the iPhone's round stands for the user's tap)"
  fi
}
game_mode_came() {
  local seconds
  seconds=$(active_after_restart)
  if [[ "$seconds" != none ]] && awk -v s="$seconds" 'BEGIN { exit !(s <= 5) }'; then
    ok "  $1: game mode was active ${seconds} s after the restart"
  else
    fail "  $1: game mode was not active within 5 s of the restart (${seconds})"
  fi
}

# ADR-0074 decision 4 in the Simulator and on the iPhone: notify-tap-test starts with the grant (the host's record of
# the app) and sends its notification's content intent itself, which is what SystemUI does on a tap; its first lines
# must be Android 15's. Its post comes in the Application's onCreate, before the session, and the host must get it all
# the same. Then session-check's notification-tap taps the delivered notification, its response sent to Minato's
# delegate as UIKit sends a tap's: the guest sends the content intent again (APKRT_EVENT_NOTIFICATION_TAP), TapActivity
# opens a second time, the notification, auto-cancelled, is gone, and the delegate called its completion handler on the
# main thread, where UIKit asserts.
# The three rounds run on the iPhone too (--device), where the user's tap is what they stand in for; the app's grant
# goes into its data folder by devicectl there.
if wanted notify-tap-test; then
  mkdir -p "$WORK/notifytap-data"
  echo '{"notifications":{"outcome":1,"denials":0}}' > "$WORK/notifytap-data/permissions.json"
  if [[ "$ON_DEVICE" == yes ]]; then
    devicectl_copy to --source "$WORK/notifytap-data" \
      --destination "$SUPPORT_REL/data/io.github.kisame76.minato.test.notifytap"
  else
    mkdir -p "$SUPPORT/data/io.github.kisame76.minato.test.notifytap"
    cp "$WORK/notifytap-data/permissions.json" "$SUPPORT/data/io.github.kisame76.minato.test.notifytap/"
  fi
  if begin_start notify-tap-test io.github.kisame76.minato.test.notifytap 1; then
    if await_line guest-log "^I/notify-tap-test: done$" 120; then
      request notification-tap
      if await_line guest-log "^I/notify-tap-test: active after the tap: 0$" 30; then
        ok "  the host's tap opened TapActivity again and cancelled the notification"
        sleep 1
      else
        failed_line "no second opening after the host's tap" notify-tap-test-1
      fi
    else
      failed_line "no done" notify-tap-test-1
    fi
  fi
  end_start notify-tap-test 1
  fetch_events
  posts=$(count_lines " notification posted tap\|3 " "$EVENTS")
  if [[ "$posts" == 1 ]]; then ok "  the host got the post of the Application's onCreate"; else
    fail "  the host got ${posts:-0} posts of the Application's onCreate, not 1"; fi
  on_main=$(count_lines " notification response completed on the main thread true$" "$EVENTS")
  if [[ "$on_main" == 1 ]]; then ok "  the delegate completed the tap's response on the main thread"; else
    fail "  the delegate completed the tap's response on the main thread ${on_main:-0} times, not once"; fi
  # A start that failed has no lines: the comparison fails, the check goes on.
  lines=$(wc -l < fixtures/expected/notify-tap-test.logcat)
  grep -E "^I/notify-tap-test: " "$WORK/notify-tap-test-1.log" | head -n "$lines" > "$WORK/notify-tap-test-head.log" ||
    true
  if cmp -s "$WORK/notify-tap-test-head.log" fixtures/expected/notify-tap-test.logcat; then
    ok "  notify-tap-test's first $((lines)) lines are Android 15's"
  else
    fail "  notify-tap-test's first lines differ from Android 15's"
    diff fixtures/expected/notify-tap-test.logcat "$WORK/notify-tap-test-head.log" | sed 's/^/      | /' || true
  fi
fi

# The global frame rate limit (Settings > Apps > Frame Rate Limit: 120, 60 or 30 fps, the screen's highest by
# default). Start 1 with 30, which the request frame-rate-limit stores as the user's pick at each launch: game mode
# logs the limit and the display rate the guest gets, the frame clock ticks 33.3 ms apart, and vsync-test's display
# mode is at 30 Hz with its frame callbacks about 33 ms apart; graphics/host holds presents to the limit too, for apps
# that present without pacing (ADR-0078 decision 4: "present limit 30 result 0"). Start 2 with 0 (none): the screen's
# highest again and no present limit, so the stored choice is none after the round.
if wanted vsync-test; then
  for start in 1 2; do
    limit=30
    [[ $start == 1 ]] || limit=0
    FRAME_RATE_LIMIT=$limit
    if begin_start vsync-test io.github.kisame76.minato.test.vsync "$start"; then
      fetch_events
      line=$(grep -o -E "frame rate limit [0-9]+ unlock .*" "$EVENTS" | tail -n 1 || true)
      if [[ -n "$line" ]]; then
        line=${line#frame rate limit }
        screen=${line##* screen }
        rate=$limit
        [[ $limit != 0 ]] || rate=$screen
        if [[ "$line" == "$limit unlock 0 display $rate screen $screen" ]]; then
          ok "  limit $limit: the guest's display rate is $rate (the screen's highest $screen)"
        else
          failed_line "limit $limit: \"$line\", not \"$limit unlock 0 display $rate screen $screen\"" \
            "vsync-test-$start"
        fi
        present=$(grep -o -E "present limit -?[0-9]+ result -?[0-9]+" "$EVENTS" | tail -n 1 || true)
        if [[ "$present" == "present limit $limit result 0" ]]; then
          ok "  limit $limit: graphics/host holds presents to $limit (0 = none)"
        else
          failed_line "limit $limit: \"${present:-no present limit line}\", not \"present limit $limit result 0\"" \
            "vsync-test-$start"
        fi
        want=$(awk -v r="$rate" 'BEGIN { printf "%.2f", 1000 / r }')
        if await_event "frames 120 interval [0-9.]+/[0-9.]+/[0-9.]+ .*" 30; then
          median=$(echo "$EVENT" | sed -E 's/.*interval [0-9.]+\/([0-9.]+)\/.*/\1/')
          if awk -v m="$median" -v w="$want" 'BEGIN { exit !(m > w * 0.9 && m < w * 1.1) }'; then
            ok "  limit $limit: the frame clock's ticks $median ms apart (1/$rate s: $want)"
          else
            failed_line "limit $limit: the frame clock's ticks $median ms apart, not $want" "vsync-test-$start"
          fi
        else
          failed_line "limit $limit: no frames line" "vsync-test-$start"
        fi
        if await_line guest-log '^I/vsync-test: done$' 60; then
          mode=$(grep -E "^I/vsync-test-device: display 0: " "$CHECK/guest-log" | head -n 1 || true)
          apart=$(grep -E "^I/vsync-test-device: frames " "$CHECK/guest-log" | head -n 1 |
            sed -E 's/.*frames ([0-9]+) and ([0-9]+) us apart/\1 \2/' || true)
          if [[ "$mode" == *" at $rate.0]"* || "$mode" == *" at $rate.0,"* ]]; then
            ok "  limit $limit: vsync-test's display mode is at $rate Hz"
          else
            failed_line "limit $limit: vsync-test's display mode: ${mode#*: }, not at $rate Hz" "vsync-test-$start"
          fi
          echo "note    vsync-test's frame callbacks ${apart:-?} us apart"
        else
          failed_line "limit $limit: vsync-test did not finish" "vsync-test-$start"
        fi
      else
        failed_line "no frame rate limit line at the start" "vsync-test-$start"
      fi
    fi
    end_start vsync-test "$start"
  done
  FRAME_RATE_LIMIT=""
fi

# The frame rate limit's hold of apps that present without pacing of their own (ADR-0078 decision 4):
# present-limit-gles swaps with EGL at a swap interval of 0, present-limit-vulkan presents a FIFO swapchain in a loop,
# each 1 s to settle and 3 s counted, then logs its presents a second and finishes. Start 1 with the limit 30:
# graphics/host holds the presents to 27-32 a second (one more where a late present catches up on the limit's grid).
# Start 2 with 0 (none): more than 100 on the iPhone's 120 Hz screen, more than 40 in the Simulator (the Mac's screen,
# at 60 Hz or more).
for app in present-limit-gles present-limit-vulkan; do
  wanted "$app" && listed "$app" || continue
  package=io.github.kisame76.minato.test.presentlimit.${app#present-limit-}
  for start in 1 2; do
    limit=30
    [[ $start == 1 ]] || limit=0
    FRAME_RATE_LIMIT=$limit
    if begin_start "$app" "$package" "$start"; then
      if await_line guest-log "^I/$app: presents counted$" 60; then
        fetch_events
        present=$(grep -o -E "present limit -?[0-9]+ result -?[0-9]+" "$EVENTS" | tail -n 1 || true)
        [[ "$present" == "present limit $limit result 0" ]] ||
          failed_line "limit $limit: \"${present:-no present limit line}\", not \"present limit $limit result 0\"" \
            "$app-$start"
        rate=$(grep -E "^I/$app-device: presents a second: [0-9]+$" "$CHECK/guest-log" | tail -n 1 |
          sed -E 's/.*: //' || true)
        low=27 high=32
        if [[ $limit == 0 ]]; then
          high=100000
          low=101
          [[ "$ON_DEVICE" == yes ]] || low=41
        fi
        if [[ -n "$rate" ]] && (( rate >= low && rate <= high )); then
          ok "  limit $limit: $app presents $rate a second (unpaced)"
        else
          failed_line "limit $limit: $app presents ${rate:-?} a second, not $low to $high" "$app-$start"
        fi
      else
        failed_line "limit $limit: $app did not count its presents" "$app-$start"
      fi
    fi
    end_start "$app" "$start"
  done
  FRAME_RATE_LIMIT=""
done

# A tap outside the text field while the keyboard is up (ime-outside-test; ADR-0068 a, amended 2026-11-10): for each
# of its five kinds of field the MinatoUITests runner taps above the field once the keyboard is up. Minato hides the
# keyboard (the user's wish), except for SDL's field, and the game gets the tap as on Android 15: "game touched true"
# for every kind, the keyboard up only for "sdl". Android's log, fixtures/expected/ime-outside-test.logcat, has the
# keyboard up for every kind. The runner is built before the start; the app waits for the tap, not a fixed time.
if [[ "$ON_DEVICE" == no ]] && wanted ime-outside-test && uitests_build; then
  if begin_start ime-outside-test io.github.kisame76.minato.test.imeoutside 1; then
    for kind in plain cocos godot flutter sdl; do
      # A kind whose tap never came waits a minute (the app's WAIT_MS) before the next one shows.
      if ! await_line guest-log "^I/ime-outside-test: $kind shown$" 90; then
        failed_line "no keyboard for $kind" ime-outside-test-1
        break
      fi
      uitest testTapOutsideField "$kind" || fail "  the runner did not tap outside $kind"
      # The bar after the tap, before the next kind's keyboard (the app's next field comes 4 s after the tap).
      bar_with_focus "the tap outside $kind" ime-outside-test-1 1
    done
    await_line guest-log "^I/ime-outside-test: done$" 90 || true
    for kind in plain cocos godot flutter sdl; do
      keyboard=true
      [[ "$kind" == sdl ]] || keyboard=false
      want="I/ime-outside-test: $kind after the tap: keyboard $keyboard, game touched true, focus true"
      if grep -q -x -F "$want" "$CHECK/guest-log"; then
        ok "  $kind: the game got the tap, the keyboard $([[ $keyboard == true ]] && echo stayed || echo went)"
      else
        fail "  $kind: $(grep -E "^I/ime-outside-test: $kind after the tap" "$CHECK/guest-log" | head -n 1 |
          sed 's/^I\/ime-outside-test: //'), not \"${want#I/ime-outside-test: $kind after the tap: }\""
      fi
    done
  fi
  end_start ime-outside-test 1
fi

# Edge deferral (device-regressions entry 3, the landscape edge's "popup"): game mode defers every screen edge. In
# landscape iOS gives the first swipe up from the bottom edge to the system all the same, on the iPhone (iOS 27.2,
# pass 5) as in the Simulator, for Minato's bare control view controller too (EDGE_CONTROL): a known iOS limit, a note,
# not a failure. What Minato owes is Android's when the system takes a gesture: the app's touch down arrives and is
# cancelled (ACTION_CANCEL, never left pressed), and the return's menu (ADR-0032 decision 7) goes by the check's
# Continue. The MinatoUITests runner swipes twice over edge-back-test-landscape, with the device turned; its pad logs
# each touch. A first swipe that stays with the app (iOS changed) is an ok. On the iPhone only; EDGE_SWIPE_SIM=1 runs
# it in the Simulator all the same.
if { [[ "$ON_DEVICE" == yes ]] || [[ -n "${EDGE_SWIPE_SIM:-}" ]]; } && wanted edge-back-test-landscape &&
    uitests_build; then
  for app in edge-back-test-landscape; do
    if begin_start $app io.github.kisame76.minato.test.edgeback 1 && await_line guest-log "^I/$app: ready$" 60; then
      sleep 1
      downs=$(count_lines "^I/$app: touch: down$" "$CHECK/guest-log")
      cancels=$(count_lines "^I/$app: touch: cancel$" "$CHECK/guest-log")
      uitest testSwipeUpFromBottomTwice $app > "$WORK/$app-swipes.out" ||
        runner_fail "  $app: the runner's swipes failed"
      cat "$WORK/$app-swipes.out"
      # What the window asked of the system: deferredEdges 15 is every edge.
      grep -o -E "orientation [0-9]+ statusBarHidden .*" "$EVENTS" | head -n 1 | sed 's/^/      | /' || true
      fetch "$CHECK_REL/guest-log" || true
      grep -E "^I/$app-device: touch: " "$CHECK/guest-log" | sed 's/^/      | /' || true
      if grep -q "the first swipe: Minato in front" "$WORK/$app-swipes.out"; then
        ok "  $app: the first swipe up from the bottom edge kept Minato in front"
      elif grep -q "the first swipe: Minato left the front" "$WORK/$app-swipes.out"; then
        echo "note    $app: the first swipe went to the system (iOS in landscape, a known limit: entry 3)"
      fi
      if (( $(count_lines "^I/$app: touch: down$" "$CHECK/guest-log") > downs )); then
        ok "  $app: the app got the first swipe's touch down"
      else
        runner_fail "  $app: the first swipe did not reach the app"
      fi
      if grep -q "the first swipe: Minato left the front" "$WORK/$app-swipes.out"; then
        if (( $(count_lines "^I/$app: touch: cancel$" "$CHECK/guest-log") > cancels )); then
          ok "  $app: the system's swipe cancelled the app's touch"
        else
          runner_fail "  $app: the system took the swipe, but the app's touch was not cancelled"
        fi
      fi
      if grep -q -E "the (first|second) swipe: Minato left the front" "$WORK/$app-swipes.out"; then
        launch
        if await_event "menu continued" 20; then ok "  $app: back in front, the return's menu went by Continue"
        else failed_line "$app: no menu at the return" "$app-1"; fi
      else
        runner_fail "  $app: the second swipe did not put Minato in the background"
      fi
    fi
    end_start $app 1
  done
  # The control (EDGE_CONTROL=1, an experiment of 2026-11-10): the runner launches Minato as a bare UIKit view
  # controller in landscape with the same deferral and no session (EdgeControl), and swipes it the same way. Only
  # notes: a control that loses the first swipe too says it is iOS's own behaviour; one that keeps it, Minato's.
  if [[ -n "${EDGE_CONTROL:-}" ]]; then
    terminate
    uitest testSwipeUpFromBottomTwice edge-control 0 control > "$WORK/edge-control-swipes.out" ||
      runner_fail "  edge control: the runner's swipes failed"
    cat "$WORK/edge-control-swipes.out"
    if grep -q "the first swipe: Minato in front" "$WORK/edge-control-swipes.out"; then
      echo "note    edge control: the bare view controller kept the first swipe: the cause is Minato's"
    elif grep -q "the first swipe: Minato left the front" "$WORK/edge-control-swipes.out"; then
      echo "note    edge control: the bare view controller lost the first swipe too: iOS's own behaviour"
    fi
    terminate
  fi
fi

# An app's request to remove an app (ADR-0077, APKRT_REQUEST_UNINSTALL): uninstall-test asks three ways to remove an
# absent app: the two intents reach Minato, which answers failed at once without an alert (PackageInstaller.uninstall
# answers STATUS_PENDING_USER_ACTION by itself, as Android does); then it asks twice to remove itself. Start 1: Minato's
# alert names the app and the check's Cancel declines both times, so its log is Android 15's (where the reference
# declines too). Start 2: Delete confirms the first one: the library removes the app and the session ends at once,
# back in the library without it. uninstall-noperm-test lacks REQUEST_DELETE_PACKAGES: the framework refuses it as
# Android does, and Minato is never asked. Without the host's answer the requests wait and no "done" comes.
same_as_android() {
  python3 - "$1" "$WORK/$1-$2.log" "fixtures/expected/$1.logcat" <<'PY' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

app, log, expected = sys.argv[1:]
compare_log(app, subprocess.CompletedProcess([], 0, "", open(log, encoding="utf-8", errors="replace").read()), expected,
            {})
PY
}
if wanted uninstall-test; then
  if begin_start uninstall-test io.github.kisame76.minato.test.uninstall 1; then
    if await_line guest-log '^I/uninstall-test: done$' 120; then
      ok "  done, $(seconds "$TAP" "$SEEN") s after the tap"
    else
      failed_line "no done: an uninstall request went unanswered" uninstall-test-1
    fi
    fetch_events
    absent=$(count_lines " uninstall [0-9]+ failed: not in the library$" "$EVENTS")
    alerts=$(count_lines " uninstall alert io\.github\.kisame76\.minato\.test\.uninstall$" "$EVENTS")
    declined=$(count_lines " uninstall [0-9]+ cancelled$" "$EVENTS")
    if [[ "$absent" == 2 && "$alerts" == 2 && "$declined" == 2 ]]; then
      ok "  the absent app failed twice without an alert; the app's own removal asked twice, declined twice"
    else
      fail "  absent app failed $absent times (2), alerts $alerts (2), declined $declined (2)"
    fi
  fi
  end_start uninstall-test 1
  same_as_android uninstall-test 1
  if begin_start uninstall-test io.github.kisame76.minato.test.uninstall 2; then
    if await_event "uninstall [0-9]+ removed" 120 && await_event "uninstall removed the app itself" 10; then
      ok "  Delete removed the app itself"
      if await_event "process library.*" 30 &&
          [[ "$(field state io.github.kisame76.minato.test.uninstall)" == None ]]; then
        ok "  its session ended, the library has it no more"
      else
        failed_line "the library still has the app, or no library came" uninstall-test-2
      fi
    else
      failed_line "Delete did not remove the app" uninstall-test-2
    fi
  fi
  end_start uninstall-test 2
fi
if wanted uninstall-noperm-test; then
  if begin_start uninstall-noperm-test io.github.kisame76.minato.test.uninstallnoperm 1; then
    if await_line guest-log '^I/uninstall-noperm-test: done$' 120; then
      ok "  done, $(seconds "$TAP" "$SEEN") s after the tap"
    else
      failed_line "no done" uninstall-noperm-test-1
    fi
    fetch_events
    if grep -q -E " uninstall " "$EVENTS"; then fail "  Minato was asked for an app without the permission"
    else ok "  Minato was never asked"; fi
  fi
  end_start uninstall-noperm-test 1
  same_as_android uninstall-noperm-test 1
fi

# An app installer in Minato (APKRT_REQUEST_ASK_INSTALL_SOURCES, APKRT_REQUEST_CONFIRM_INSTALL): install-flow-test
# without the grant opens the settings page for unknown app sources, Minato's alert answers Allow, and the grant is
# the app's at once; it hands its own APK to the package installer, Minato's install alert answers Install, and the
# installer ends with RESULT_OK. Its second start has the grant from the start, kept in the app's profile.
if wanted install-flow-test; then
  flow="io.github.kisame76.minato.test.installflow"
  # The Simulator's run starts without the grant; on the iPhone a grant of an earlier run fails round 1's precondition.
  [[ "$ON_DEVICE" == yes ]] || rm -f "$SUPPORT/data/$flow/profile.json"
  for round in 1 2; do
    if begin_start install-flow-test "$flow" "$round"; then
      if await_line guest-log "^I/install-flow-test: done$" 120; then
        log="$CHECK/guest-log"
        if [[ $round == 1 ]] && grep -q "^I/install-flow-test: canRequestPackageInstalls: true$" "$log"; then
          fail "  precondition not met: the app had the grant from an earlier run (its profile.json)"
        elif [[ $round == 1 ]]; then
          if grep -q "^I/install-flow-test: canRequestPackageInstalls after the settings page: true$" "$log"; then
            ok "  Minato's alert allowed the app to install apps"
          else
            fail "  no grant after the settings page for unknown app sources"
          fi
        elif grep -q "^I/install-flow-test: canRequestPackageInstalls: true$" "$log"; then
          ok "  the second start has the grant from the start"
        else
          fail "  the second start has no grant: the app's profile did not keep it"
        fi
        if grep -q "^I/install-flow-test: install result: ok$" "$log"; then
          ok "  the handed-over APK went to the library, RESULT_OK"
        else
          fail "  the install ended $(sed -n -E 's/^I\/install-flow-test: install result: //p' "$log" | head -n 1)"
        fi
      else
        failed_line "no done" "install-flow-test-$round"
      fi
    fi
    end_start install-flow-test "$round"
    fetch_events
    grep -E " install (sources )?[0-9]+ " "$EVENTS" | sed 's/^/      | /' || true
  done
fi

# An app installer through a PackageInstaller session in Minato (the same alerts as install-flow-test): without the
# grant the settings page asks, Minato's alert answers Allow; the app writes its own APK into a session, commits it,
# starts the confirm intent of the pending status, Minato's install alert answers Install, and the session's sender
# gets STATUS_SUCCESS once the library has the app. The second start has the grant from the start.
if wanted install-session-flow-test; then
  sessionflow="io.github.kisame76.minato.test.installsessionflow"
  [[ "$ON_DEVICE" == yes ]] || rm -f "$SUPPORT/data/$sessionflow/profile.json"
  for round in 1 2 3; do
    if begin_start install-session-flow-test "$sessionflow" "$round"; then
      if await_line guest-log "^I/install-session-flow-test: done$" 120; then
        log="$CHECK/guest-log"
        if [[ $round == 1 ]]; then
          if grep -q "^I/install-session-flow-test: canRequestPackageInstalls after the settings page: true$" "$log"; then
            ok "  Minato's alert allowed the app to install apps"
          else
            fail "  no grant after the settings page for unknown app sources"
          fi
        elif grep -q "^I/install-session-flow-test: canRequestPackageInstalls: true$" "$log"; then
          ok "  start $round has the grant from the start"
        else
          fail "  the second start has no grant: the app's profile did not keep it"
        fi
        if [[ $round == 3 ]]; then
          # The user declines the install: the confirm ends cancelled and the sender gets STATUS_FAILURE_ABORTED.
          if grep -q "^I/install-session-flow-test: confirm result: cancelled$" "$log" &&
              grep -q "^I/install-session-flow-test: final status: 3$" "$log"; then
            ok "  the user's Cancel ended the session with STATUS_FAILURE_ABORTED"
          else
            fail "  the declined install did not end with ABORTED"
          fi
        elif grep -q "^I/install-session-flow-test: status: pending user action$" "$log" &&
            grep -q "^I/install-session-flow-test: confirm result: ok$" "$log" &&
            grep -q "^I/install-session-flow-test: final status: 0$" "$log"; then
          ok "  the session's APK went to the library, STATUS_SUCCESS"
        else
          ended=$(sed -n -E 's/^I\/install-session-flow-test: (final status|confirm result): //p' "$log" | tr '\n' ' ')
          fail "  the session ended: $ended"
        fi
      else
        failed_line "no done" "install-session-flow-test-$round"
      fi
    fi
    end_start install-session-flow-test "$round"
    fetch_events
    grep -E " install (sources )?[0-9]+ " "$EVENTS" | sed 's/^/      | /' || true
  done
fi

# A split set through a PackageInstaller session (install-set-flow-test, splits-test's base, arm64, en and feature APKs
# in one session): Minato's host takes the set's paths, checks them as the import takes a unit (ADR-0048) and, after
# the install alert's Install, hands them to the library as one unit; the sender gets STATUS_SUCCESS, and the
# library's next start (round 2's) has splits-test's app with its splits. Round 2, with the grant from round 1's
# profile, declines the install: cancelled, STATUS_FAILURE_ABORTED.
if wanted install-set-flow-test; then
  setflow="io.github.kisame76.minato.test.installsetflow"
  set_app="io.github.kisame76.minato.test.splits"
  [[ "$ON_DEVICE" == yes ]] || rm -f "$SUPPORT/data/$setflow/profile.json"
  for round in 1 2; do
    if begin_start install-set-flow-test "$setflow" "$round"; then
      if await_line guest-log "^I/install-set-flow-test: done$" 120; then
        log="$CHECK/guest-log"
        if [[ $round == 2 ]]; then
          if grep -q "^I/install-set-flow-test: confirm result: cancelled$" "$log" &&
              grep -q "^I/install-set-flow-test: final status: 3$" "$log"; then
            ok "  the user's Cancel ended the set's session with STATUS_FAILURE_ABORTED"
          else
            fail "  the declined set did not end with ABORTED"
          fi
          # Round 2's library start installed round 1's set as one entry of splits-test's app with its splits.
          fetch "$SUPPORT_REL/library.json" || true
          splits=$(python3 - "$SUPPORT/library.json" "$set_app" 2>/dev/null <<'PY' || echo none
import json
import sys
entry = {e["id"]: e for e in json.load(open(sys.argv[1]))["entries"]}.get(sys.argv[2])
print("none" if entry is None else len(entry.get("splits") or []))
PY
)
          if [[ "$splits" =~ ^[1-9][0-9]*$ ]]; then ok "  the library has the set as one app with $splits splits"; else
            fail "  the library has no entry with splits for the set's app: $splits"; fi
        elif grep -q "^I/install-set-flow-test: status: pending user action$" "$log" &&
            grep -q "^I/install-set-flow-test: confirm result: ok$" "$log" &&
            grep -q "^I/install-set-flow-test: final status: 0$" "$log"; then
          ok "  the session's set went to the library, STATUS_SUCCESS"
        else
          ended=$(sed -n -E 's/^I\/install-set-flow-test: (final status|confirm result): //p' "$log" | tr '\n' ' ')
          fail "  the set's session ended: $ended"
        fi
      else
        failed_line "no done" "install-set-flow-test-$round"
      fi
    fi
    end_start install-set-flow-test "$round"
    fetch_events
    grep -E " install (sources )?[0-9]+ " "$EVENTS" | sed 's/^/      | /' || true
  done
fi

# ADR-0074 decision 4's cold tap: once notify-tap-test has run and its notification is delivered,
# Minato starts and session-check's notification-cold-tap taps it as it launches, before the library has its entries,
# as didReceive comes when the tap launched Minato: the tap waits for the library, which checks the stored content
# intent (its own TapActivity) and starts the app with it, so TapActivity is the first activity, before the app's own
# send.
if wanted notify-tap-test; then
  if begin_start notify-tap-test io.github.kisame76.minato.test.notifytap 2; then
    await_line guest-log "^I/notify-tap-test: done$" 120 || failed_line "no done" notify-tap-test-2
  fi
  end_start notify-tap-test 2
  save_response "the cold tap"
  terminate
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  echo yes > "$CHECK/no-compile"
  echo yes > "$CHECK/guest-log"
  echo io.github.kisame76.minato.test.notifytap > "$CHECK/notification-cold-tap"
  reset_events
  send_requests
  launch
  if await_line guest-log "^I/notify-tap-test: opened by the tap: room 7$" 120; then
    first=$(grep -m1 -E "^I/notify-tap-test: (opened by the tap|sent the content intent)" "$CHECK/guest-log")
    if [[ "$first" == "I/notify-tap-test: opened by the tap: room 7" ]]; then
      ok "  the cold tap started TapActivity in place of the launcher"
    else
      fail "  the cold tap started the launcher: $first"
    fi
    sleep 1
    tap_extras "the cold tap"
  else
    failed_line "no TapActivity after the cold tap" notify-tap-test-3
  fi
  sleep 2
  end_start notify-tap-test 3
  fetch_events
  tap_precondition "the cold tap"
  game_mode_came "the cold tap"
  on_main=$(count_lines " notification response completed on the main thread true$" "$EVENTS")
  if [[ "$on_main" == 1 ]]; then ok "  the delegate completed the cold tap's response on the main thread"; else
    fail "  the delegate completed the cold tap's response on the main thread $on_main times, not once"; fi
fi

# The same tap with Minato in the background, in its library: Settings comes to the front, then Minato again, and
# session-check's notification-foreground-tap taps the delivered notification as the scene enters the foreground,
# before it is active, where iOS's didReceive comes for a tap in Notification Center. The library starts the app
# with TapActivity once Minato is active.
if wanted notify-tap-test; then
  if begin_start notify-tap-test io.github.kisame76.minato.test.notifytap 4; then
    await_line guest-log "^I/notify-tap-test: done$" 120 || failed_line "no done" notify-tap-test-4
  fi
  end_start notify-tap-test 4
  save_response "the tap from the background"
  # Minato stays up in its library; the guest log of the start the tap makes comes into the folder from here on.
  echo yes > "$CHECK/guest-log"
  send_requests
  sleep 2
  launch "$AWAY"
  sleep 2
  echo io.github.kisame76.minato.test.notifytap > "$CHECK/notification-foreground-tap"
  send_requests
  launch
  if await_line guest-log "^I/notify-tap-test: opened by the tap: room 7$" 120; then
    ok "  the tap from the background started TapActivity"
    sleep 1
    tap_extras "the tap from the background"
  else
    failed_line "no TapActivity after the tap from the background" notify-tap-test-5
  fi
  sleep 2
  end_start notify-tap-test 5
  fetch_events
  tap_precondition "the tap from the background"
  game_mode_came "the tap from the background"
fi

# NOTIFY_REAL: the tap of the user, through Notification Center, in the states Minato can be in at a tap (ADR-0074
# decision 4; device-regressions entry 1): a guest in game mode in front, Minato's library in front, Minato in the
# background, Minato not running, and the guest's game mode in the background (game-background). NOTIFY_REAL=1 runs all
# five; a list of them ("game-background cold") only those. The MinatoUITests runner (app/UITests, its own scheme)
# drives SpringBoard only: it goes Home, opens Notification Center and taps notify-tap-test's "Tap me", so iOS launches
# or resumes Minato as for the user's tap; the session's events say what Minato did. On the iPhone the runner is
# installed first and removed at the end (the runner only), which the sitting's OK covers; a refused install, such as by
# the free team's limit of 3 apps on a device, stops these rounds with its own line.
if [[ -n "${NOTIFY_REAL:-}" ]] && wanted notify-tap-test; then
  echo "== notify-tap-test, the user's tap through Notification Center"
  # Each runner call and each copy of the iPhone with the time of day (pass 4: copies hung in these rounds only).
  stamps_before=$STAMPS
  STAMPS=yes
  real_ready=yes
  uitests_build || real_ready=no
  if [[ "$real_ready" == yes && "$ON_DEVICE" == yes ]]; then
    runner=$(ls -d "$UITESTS"/Build/Products/Debug-iphoneos/MinatoUITests-Runner.app 2>/dev/null)
    if ! on_device install app --device "$DEVICE_ID" "$runner" > "$WORK/uitests-install.log" 2>&1; then
      fail "  the UI test runner was not installed (the free team's limit of 3 apps on a device?):"
      grep -i -m 3 "error\|limit\|maximum" "$WORK/uitests-install.log" | sed 's/^/      | /' || true
      real_ready=no
    fi
  fi
  # The folder's requests for a start that iOS makes: the guest log on, no compile mode, no tap in the library.
  real_requests() {
    rm -rf "$CHECK"
    mkdir -p "$CHECK"
    echo yes > "$CHECK/no-compile"
    echo yes > "$CHECK/guest-log"
    echo yes > "$CHECK/continue"
    reset_events
    send_requests
  }
  # In the Simulator, notification-repost: Minato hands notify-tap-test's notification to iOS again, delivered $1 s
  # later, so that its banner comes in the round's state, and the runner taps the banner (the Simulator treats
  # Notification Center as the Lock Screen, where a tap on an entry opens nothing). On the iPhone the runner taps
  # the entry in Notification Center, as the user did; there is no banner to wait for.
  repost_request() {
    [[ "$ON_DEVICE" == yes ]] || echo "io.github.kisame76.minato.test.notifytap $1" > "$CHECK/notification-repost"
  }
  banner_wait() { if [[ "$ON_DEVICE" == yes ]]; then echo 0; else echo "$1"; fi; }
  # Whether TapActivity opened a second time within 30 s.
  opened_again() {
    local end=$((SECONDS + 30))
    while (( SECONDS < end )); do
      fetch "$CHECK_REL/guest-log" || true
      (( $(count_lines "^I/notify-tap-test: opened by the tap: room 7$" "$CHECK/guest-log") >= 2 )) && return 0
      sleep 0.5
    done
    return 1
  }
  # Round $2 in state $1: notify-tap-test posts "Tap me", Minato is brought into the state, the runner taps.
  real_tap() {
    local state=$1 round=$2 label="the real tap, $1"
    [[ "$NOTIFY_REAL" == 1 || " $NOTIFY_REAL " == *" $state "* ]] || return 0
    if begin_start notify-tap-test io.github.kisame76.minato.test.notifytap "$round"; then
      await_line guest-log "^I/notify-tap-test: done$" 120 || failed_line "no done" "notify-tap-test-$round"
    fi
    if [[ "$state" == game || "$state" == game-background ]]; then
      # TapActivity opens again in the running app, as for the host's tap of start 1. game-background: Minato goes
      # to the background first, as Notification Center may take it on the iPhone; the tap's return must not stop at
      # Minato's menu (ADR-0032 decision 7: a notification tap of the running app resumes it).
      # The banner of the app's own post goes first (about 5 s), so that the runner waits for the new one.
      sleep 6
      # game-background: the check's own Continue (begin_start's "continue") is off, so that only the tap can take
      # the return's menu away.
      [[ "$state" != game-background ]] || rm -f "$CHECK/continue"
      repost_request 8
      send_requests
      if [[ "$state" == game-background ]] && ! uitest testGoHome "$round"; then
        runner_fail "  $label: Minato did not go to the background"
      fi
      # TapActivity opens a second time. Through Notification Center the first drag from the top edge may open
      # Minato's menu (game mode defers the edge), which pauses the app: its line after the tap then says so.
      if uitest testTapNotification "$round" "$(banner_wait 30)" && opened_again; then
        ok "  $label: TapActivity opened again in the running app"
      else
        runner_failed_line "$label: no second opening" "notify-tap-test-$round"
      fi
      sleep 1
      end_start notify-tap-test "$round"
      fetch_events
      on_main=$(count_lines " notification response completed on the main thread true$" "$EVENTS")
      if [[ "$on_main" == 1 ]]; then ok "  $label: the delegate completed on the main thread"; else
        fail "  $label: the delegate completed on the main thread $on_main times, not once"; fi
      return
    fi
    end_start notify-tap-test "$round"
    real_requests
    repost_request 25
    send_requests
    launch
    [[ "$ON_DEVICE" == yes ]] || await_event "notification reposted" 30 ||
      fail "  $label: the notification was not handed to iOS again"
    [[ "$state" != cold ]] || terminate
    if [[ "$state" == background ]] && ! uitest testGoHome "$round"; then
      runner_fail "  $label: Minato did not go to the background"
    fi
    if uitest testTapNotification "$round" "$(banner_wait 40)" &&
        await_line guest-log "^I/notify-tap-test: opened by the tap: room 7$" 60; then
      ok "  $label: TapActivity opened"
      sleep 1
      tap_extras "$label"
    else
      runner_failed_line "$label: no TapActivity after the tap" "notify-tap-test-$round"
    fi
    sleep 2
    end_start notify-tap-test "$((round + 1))"
    fetch_events
    grep -E " (notification (tap|launch)|restart|exit|process|launched|scene) " "$EVENTS" |
      sed 's/^/      ev /' || true
    game_mode_came "$label"
  }
  if [[ "$real_ready" == yes ]]; then
    # A Simulator that shows its Lock Screen (as after its boot) is unlocked first: a tap on a Lock Screen
    # notification opens nothing until then. The iPhone must be unlocked for the sitting.
    [[ "$ON_DEVICE" == yes ]] || uitest testUnlock 0 >/dev/null || true
    real_tap game 6
    real_tap library 7
    real_tap background 9
    real_tap cold 11
    real_tap game-background 13
  fi
  if [[ "$ON_DEVICE" == yes && "$real_ready" == yes ]]; then
    on_device uninstall app --device "$DEVICE_ID" io.github.kisame76.minato.uitests.xctrunner >/dev/null 2>&1 &&
      ok "  the UI test runner is removed again" || fail "  the UI test runner was not removed"
  fi
  STAMPS=$stamps_before
fi

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
  opacity=$(sed -n -E 's/^[0-9.]+ [0-9]+ layer opaque ([01])$/\1/p' "$EVENTS" 2>/dev/null | tr -d '\n' || true)
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
# The network the host reports (ADR-0054 step d): the Mac's or the iPhone's path, Wi-Fi up and mobile data off as on
# apk-runner-35 with its mobile data off (fixtures/expected/<app>.logcat, the Mac's tools/network-check.sh and the others
# of its kind); the device must be online over Wi-Fi (or Ethernet on the Mac). cellular-network-test plays a cellular
# path and stays with the Mac's check.
run_one network-test io.github.kisame76.minato.test.network
run_one network-request-test io.github.kisame76.minato.test.networkrequest
run_one network-security-test io.github.kisame76.minato.test.networksecurity
run_one network-security-config-test io.github.kisame76.minato.test.networksecurityconfig
run_one wifi-state-test io.github.kisame76.minato.test.wifistate
run_one net-io-test io.github.kisame76.minato.test.netio
# Another device on the network (the networking gap): tools/lan-server.py on this Mac is the other device, for this app
# only. On the iPhone it listens on the Mac's Wi-Fi address (the phone must be on the same network) and iOS asks the user
# for the local network permission at the first connection; in the Simulator it is the loopback. The address goes to the
# app through tmp/guest-environment and is never printed, logged or kept.
if wanted lan-test; then
  lan_host=127.0.0.1
  if [[ "$ON_DEVICE" == yes ]]; then
    lan_host=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)
  fi
  if [[ -z "$lan_host" ]]; then
    fail "lan-test needs the Mac on Wi-Fi: no address to serve on"
  else
    # LAN_SETTINGS' second start comes minutes later (the runner in Settings twice): the server waits for it.
    python3 tools/lan-server.py --bind "$lan_host" --seconds "$([[ -n "${LAN_SETTINGS:-}" ]] && echo 480 || echo 120)" \
      >/dev/null 2>&1 &
    lan_server=$!
    sleep 1
    echo "MINATO_LAN_HOST=$lan_host" > "$WORK/guest-environment"
    if [[ "$ON_DEVICE" == yes ]]; then
      devicectl_copy to --source "$WORK/guest-environment" --destination "tmp/guest-environment"
    else
      cp "$WORK/guest-environment" "$CONTAINER/tmp/guest-environment"
    fi
    # On the iPhone iOS asks for the local network at the first connection: the runner answers Allow, the "allowed"
    # state (a later run finds the answer). The Simulator has no such permission.
    allow=""
    if [[ "$ON_DEVICE" == yes ]] && uitests_build; then
      uitest testAllowAlert lan-allow 60 localnetwork > "$WORK/lan-allow.out" 2>&1 &
      allow=$!
    fi
    run_one lan-test io.github.kisame76.minato.test.lan
    if [[ -n "$allow" ]]; then
      if wait "$allow"; then ok "  the runner answered iOS's local network alert Allow"; else
        echo "note    no local network alert came (iOS has Minato's answer already)"; fi
    fi
    # LAN_SETTINGS=1, only after the user's yes to the runner changing Minato's Local Network switch in Settings: the
    # "denied" state. The runner reads the switch first, turns it off for a second start, and sets it back as it was,
    # also when that start failed; nothing else in Settings is touched. The second start's lines are printed.
    if [[ "$ON_DEVICE" == yes && -n "${LAN_SETTINGS:-}" ]] && uitests_build; then
      was=""
      if uitest testLocalNetworkSetting lan-read 0 read > "$WORK/lan-read.out"; then
        was=$(sed -n -E 's/^note +local network was (on|off)$/\1/p' "$WORK/lan-read.out" | head -n 1)
      fi
      if [[ -z "$was" ]]; then
        runner_fail "  the runner could not read Minato's Local Network switch"
        # What the runner saw (counts and kinds of elements, no names): uitest printed it into the file.
        [[ -n "$RUNNER_DOWN" ]] || sed 's/^/      | /' "$WORK/lan-read.out"
      else
        echo "note    Minato's Local Network switch was $was"
        if uitest testLocalNetworkSetting lan-off 0 off > /dev/null; then
          run_start lan-test io.github.kisame76.minato.test.lan 2
          grep -E "^I/lan-test: " "$WORK/lan-test-2.log" | sed 's/^/      denied | /' || true
        else
          runner_fail "  the runner did not turn Minato's Local Network off"
        fi
        uitest testLocalNetworkSetting lan-back 0 "$was" > /dev/null ||
          fail "  Minato's Local Network switch was not set back to $was: set it in Settings > Minato"
      fi
    fi
    kill "$lan_server" 2>/dev/null || true
    if [[ "$ON_DEVICE" == yes ]]; then
      : > "$WORK/guest-environment"
      devicectl_copy to --source "$WORK/guest-environment" --destination "tmp/guest-environment" || true
    else
      rm -f "$CONTAINER/tmp/guest-environment"
    fi
    rm -f "$WORK/guest-environment"
  fi
fi
# The package installer for an app without install rights, and the exit reasons (the Unity test APK resolve list, batch E).
run_one package-installer-test io.github.kisame76.minato.test.packageinstaller
# android.media's DRM, codec list, retriever, extractor, image reader and TV input manager without the hardware.
run_one media-absent-test io.github.kisame76.minato.test.mediaabsent
run_one media-extractor-test io.github.kisame76.minato.test.mediaextractor
run_one media-codec-audio-test io.github.kisame76.minato.test.mediacodecaudio
run_one media-codec-decoders-test io.github.kisame76.minato.test.mediacodecdecoders
run_one media-codec-opus-test io.github.kisame76.minato.test.mediacodecopus
run_one media-extractor-ogg-test io.github.kisame76.minato.test.mediaextractorogg
# ADR-0056 c2: the WebView's page on graphics/host's WKWebView, which goes into the session's view below the window's
# layer, and the layer lets it through.
if wanted webview-test; then
  run_one webview-test io.github.kisame76.minato.test.webview
  fetch_events
  if grep -q -E "^[0-9.]+ [0-9]+ webviews 1 layer opaque 0$" "$EVENTS"; then
    ok "  the page in the session's view, the window's layer not opaque over it"
  else
    fail "  no page in the session's view: $(grep -E ' webviews ' "$EVENTS" | tail -1)"
  fi
fi
# ADR-0043's test 1: views, a TextView and a Button drawn in software. This check plays no touches, so the Button's
# click comes from the app's performClick, 5 s after its label's change, which its device line names; the Mac's
# tools/views-check.sh and the AVD tap it.
[[ "$ON_DEVICE" == no ]] || run_one views-test io.github.kisame76.minato.test.views
# On the iPhone only, as its APPS entry (the Simulator has no MoltenVK).
[[ "$ON_DEVICE" == no ]] || run_one vk-ahb-test io.github.kisame76.minato.test.vkahb
[[ "$ON_DEVICE" == no ]] || run_one gles-procs-test io.github.kisame76.minato.test.glesprocs
[[ "$ON_DEVICE" == no ]] || run_one engine-start-test io.github.kisame76.minato.test.enginestart
# hardware-bitmap-test: Bitmap.Config.HARDWARE bitmaps made by Bitmap.copy, BitmapFactory and ImageDecoder keep
# their pixels (hwui patch 0012), in the Simulator and on the iPhone.
run_one hardware-bitmap-test io.github.kisame76.minato.test.hardwarebitmap
# graphics-classes-test: BitmapRegionDecoder, Gainmap, AnimatedImageDrawable and Mesh, whose natives hwui's patch
# 0013 registers, in the Simulator and on the iPhone.
run_one graphics-classes-test io.github.kisame76.minato.test.graphicsclasses
# program-binary-test (a game engine's shader cache), in the Simulator and on the iPhone: its first start saves its GL
# programs' binaries, its second, a new process, loads them and binaries a driver must refuse without a crash. Both
# starts' lines must be Android 15's.
if wanted program-binary-test; then
  run_start program-binary-test io.github.kisame76.minato.test.programbinary 1
  run_start program-binary-test io.github.kisame76.minato.test.programbinary 2
  python3 - "$WORK/program-binary-test-1.log" "$WORK/program-binary-test-2.log" \
    fixtures/expected/program-binary-test.logcat <<'EOF' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

first, second, expected = sys.argv[1:]
log = "".join(open(path, encoding="utf-8", errors="replace").read() for path in (first, second))
compare_log("program-binary-test", subprocess.CompletedProcess([], 0, "", log), expected, {})
EOF
fi
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
if wanted obb-large-test && listed obb-large-test; then
  # The import inflated the 2.2 GB OBB into the app's OBB folder before the app was ready: the time from the unit's
  # arrival is shown, and the app reads the file at offsets beyond 2^31 (fixtures/expected/obb-large-test.logcat).
  echo "  obb-large-test: ready $((SECONDS - ${LARGE_XAPK_START:-SECONDS})) s after the .xapk arrived (a run's time, the device is shared)"
  run_one obb-large-test io.github.kisame76.minato.test.obblarge
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
  [[ -z "${COMPILE:-}" && -n "$GODOT_TEST" ]] && wanted "godot-bench-$flavor" || continue
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

# Engines' fields with text (ime-engines-test, ADR-0068; the user's bug of 2026-11-05 in device-regressions entry 4):
# a Godot-style hidden EditText, a framework's own editable as Flutter's, a Cocos2d-x-style EditText, then AlertDialogs
# that ask for the keyboard with their window's soft input mode only, as an engine's text input dialog does. Each of
# the seven fields logs whether the keyboard is up for it, from its window's insets, which follow UIKit's keyboard
# here: each line must be Android 15's (fixtures/expected/ime-engines-test.logcat, LatinIME on apk-runner-35), and
# the session's "keyboard text bar" events must give each field's text to the bar, by its length, in order.
if wanted ime-engines-test; then
  if begin_start ime-engines-test io.github.kisame76.minato.test.imeengines 1; then
    if await_line guest-log '^I/ime-engines-test: done$' 120; then
      ok "  done, $(seconds "$TAP" "$SEEN") s after the tap"
    else
      failed_line 'no "done"' ime-engines-test-1
    fi
  fi
  fetch_events
  end_start ime-engines-test 1
  python3 - "$WORK/ime-engines-test-1.log" "$EVENTS" fixtures/expected/ime-engines-test.logcat <<'PY' ||
import re
import sys

log, events, expected = sys.argv[1:]
prefix = "I/ime-engines-test: "
pattern = re.compile(r"^I/ime-engines-test: ([a-z-]+): keyboard (true|false)$")
ours = dict(m.groups() for m in map(pattern.match, open(log, encoding="utf-8", errors="replace").read().splitlines())
            if m)
failed = 0
for line in open(expected, encoding="utf-8").read().splitlines():
    match = pattern.match(line)
    if not match:
        continue
    field, android = match.groups()
    if ours.get(field) == android:
        print(f"ok      {field}: keyboard {android}, as Android 15")
    else:
        print(f"FAIL    {field}: keyboard {ours.get(field, 'not logged')}, Android 15: {android}")
        failed = 1
# Godette, Dash, Cocos, then "Player 1" for each of the dialogs that show it, among the bar's other texts.
bars = [int(n) for n in re.findall(r" keyboard text bar of (\d+) units$", open(events, encoding="utf-8").read(), re.M)]
wanted = [7, 4, 5, 8, 8, 8]
left = iter(bars)
if all(any(n == want for n in left) for want in wanted):
    print("ok      the bar had each field's text: " + ", ".join(f"{n} units" for n in wanted))
else:
    print(f"FAIL    the bar's texts: expected {wanted} units in this order, got {bars}")
    failed = 1
sys.exit(failed)
PY
    failures=$((failures + 1))
fi

# A field at the window's bottom under the keyboard (ime-pan-test, target 35, and ime-pan-test-34): its window's
# soft input mode unspecified, adjustPan and adjustResize in turn, the keyboard up for 12 s in each. A screenshot at
# each "<mode> shown" must show the magenta field above the keyboard, or not, as apk-runner-35's screenshots do with
# LatinIME: panned for unspecified and pan, covered for resize at target 35 (edge to edge), resized at target 34.
# Each screenshot must fall in its mode's keyboard: from the host's "keyboard show(" event (1 s on, for its
# animation) to its "keyboard hide", on the device's clock. A screenshot outside fails as "late capture", whatever it
# shows; each mode's line gives the times (on the iPhone the log and the screenshot take seconds over devicectl).
pan_round() {
  # Its own names for the times: await_line reads the caller's "seen" (bash's dynamic scope).
  local app=$1 package=$2 wanted=$3 mode verdict want shown_at shot_start shot_end
  local -a shots=()
  wanted "$app" || return 0
  if begin_start "$app" "$package" 1; then
    for mode in unspecified pan resize; do
      if await_line guest-log "^I/$app: $mode shown$" 60; then
        shown_at=$SEEN
        shot_start=$(now)
        capture "$WORK/$app-$mode.png"
        shot_end=$(now)
        verdict=$(python3 - "$WORK/$app-$mode.png" <<'PY'
import sys
sys.path.insert(0, "tools")
from window_pixels import read_png

width, height, rows, _ = read_png(sys.argv[1])
# The field's magenta (sRGB 255,0,255; Display P3 about 234,51,247): seen when a row holds enough of it.
seen = sum(1 for row in rows if sum(1 for i in range(0, len(row), 16)
                                    if row[i] > 180 and row[i + 1] < 110 and row[i + 2] > 180) > width // 16)
print("visible" if seen >= 10 else "covered")
PY
)
        shots+=("$mode $verdict $shown_at $shot_start $shot_end")
      else
        failed_line "no \"$mode shown\"" "$app-1"
        break
      fi
    done
    await_line guest-log "^I/$app: done$" 60 || true
  fi
  end_start "$app" 1
  fetch_events
  local shot
  for shot in "${shots[@]}"; do
    read -r mode verdict shown_at shot_start shot_end <<< "$shot"
    want=$(echo "$wanted" | tr ' ' '\n' | grep "^$mode=" | cut -d= -f2)
    # The mode's keyboard: its show is the Nth "keyboard show(" of the start, its hide the "keyboard hide" after it.
    local index=$(( $(echo "unspecified pan resize" | tr ' ' '\n' | grep -n "^$mode$" | cut -d: -f1) ))
    local inside
    inside=$(python3 - "$EVENTS" "$index" "$shot_start" "$shot_end" "$shown_at" <<'PY'
import sys
events, index, start, end, seen = sys.argv[1], int(sys.argv[2]), *map(float, sys.argv[3:])
shows, hides = [], []
for line in open(events, errors="replace"):
    fields = line.split(" ", 2)
    if len(fields) < 3:
        continue
    if fields[2].startswith("keyboard show("):
        shows.append(float(fields[0]))
    elif fields[2].strip() == "keyboard hide":
        hides.append(float(fields[0]))
if len(shows) < index:
    print("no keyboard show %d" % index)
    sys.exit()
up = shows[index - 1]
down = next((hide for hide in hides if hide > up), None)
times = "shown %.1f s after the keyboard, capture %.1f..%.1f s, keyboard down at %s" % (
    seen - up, start - up, end - up, "%.1f s" % (down - up) if down else "never")
print(("inside " if start >= up + 1 and down is not None and end <= down else "late ") + times)
PY
)
    echo "note    $app $mode: ${inside#* }"
    if [[ "$inside" != inside* ]]; then
      fail "  $app $mode: late capture (${inside#* })"
    elif [[ "$verdict" == "$want" ]]; then
      ok "  $app $mode: the field is $verdict, as on Android 15"
    else
      fail "  $app $mode: the field is $verdict, Android 15: $want"
    fi
  done
}
pan_round ime-pan-test io.github.kisame76.minato.test.imepan "unspecified=visible pan=visible resize=covered"
pan_round ime-pan-test-34 io.github.kisame76.minato.test.imepan34 "unspecified=visible pan=visible resize=visible"

# A NativeActivity app with its own dex starts through app_process (ADR-0069 step 4a): native-jni-ime-test's JNI calls
# into the framework answer as on Android 15, up to the keyboard's show once the decor view has the focus (its first
# 13 lines of fixtures/expected/native-jni-ime-test.logcat, the window's size left out, which is the device's). On
# activity-host they answer that there is no Java. Its keys are the Mac's tools/native-activity-check.sh's.
if wanted native-jni-ime-test; then
  if begin_start native-jni-ime-test io.github.kisame76.minato.test.nativejniime 1; then
    await_line guest-log '^I/native-jni-ime-test: showSoftInput\(decorView, SHOW_FORCED\) again: ' 60 || true
  fi
  end_start native-jni-ime-test 1
  got=$(grep '^I/native-jni-ime-test: ' "$WORK/native-jni-ime-test-1.log" | sed -n '2,13p' || true)
  if [[ "$got" == "$(sed -n '2,13p' fixtures/expected/native-jni-ime-test.logcat)" ]]; then
    ok "  native-jni-ime-test's JNI answers are Android 15's, 12 lines"
  else
    fail "  native-jni-ime-test's JNI answers are not Android 15's:"
    diff <(sed -n '2,13p' fixtures/expected/native-jni-ime-test.logcat) <(echo "$got") | sed 's/^/      | /' || true
  fi
fi

# The keyboard's delete key in an empty SDL field (ime-surrounding-test-delete, as Ren'Py's empty name field):
# Android's keyboard still calls deleteSurroundingText(1, 0) with nothing before the cursor, which SDL2 turns into
# the game's backspace (fixtures/expected/ime-surrounding-test-delete.logcat, LatinIME on apk-runner-35). The
# MinatoUITests runner taps UIKit's delete key once the keyboard is up; the app's log must be Android 15's.
# Without an Xcode account for the runner the round is skipped with uitests_build's line, before the app starts.
if wanted ime-surrounding-test-delete && { uitests_build || [[ "$uitests_built" != noaccount ]]; }; then
  if begin_start ime-surrounding-test-delete io.github.kisame76.minato.test.imesurroundingdelete 1; then
    if await_line guest-log '^I/ime-surrounding-test-delete: showSoftInput true$' 120 &&
        await_event 'keyboard show\(' 20 && uitests_build && uitest testKeyboardDelete 1; then
      if await_line guest-log '^I/ime-surrounding-test-delete: done$' 15; then
        ok "  the delete key in the empty field reached the app"
      else
        failed_line "the delete key in the empty field reached nothing" ime-surrounding-test-delete-1
      fi
    else
      runner_failed_line "no keyboard to tap the delete key on" ime-surrounding-test-delete-1
    fi
  fi
  end_start ime-surrounding-test-delete 1
  # Without the runner's delete key the log cannot be Android's: the runner's one line said why.
  if [[ -z "$RUNNER_DOWN" ]]; then
    python3 - "$WORK/ime-surrounding-test-delete-1.log" fixtures/expected/ime-surrounding-test-delete.logcat <<'PY' ||
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

log, expected = sys.argv[1:]
compare_log("ime-surrounding-test-delete",
            subprocess.CompletedProcess([], 0, "", open(log, encoding="utf-8", errors="replace").read()), expected, {})
PY
      failures=$((failures + 1))
  fi
  # The bar's hide key (the user's bug of 2026-11-05): an SDL field keeps the keyboard at a tap outside, as on Android
  # 15, so the bar has Apple's key that hides it; the runner taps it, and Minato hides the keyboard as at the user's
  # own hide, which the guest hears of.
  if begin_start ime-surrounding-test-delete io.github.kisame76.minato.test.imesurroundingdelete 2; then
    if await_line guest-log '^I/ime-surrounding-test-delete: showSoftInput true$' 120 &&
        await_event 'keyboard show\(' 20 && uitests_build && uitest testKeyboardHide 2; then
      if await_event 'keyboard hide key' 10 && await_event 'keyboard dismissed' 10; then
        ok "  the bar's hide key put the SDL field's keyboard away"
        no_keyboard_views "the hide key" ime-surrounding-test-delete-2
      else
        failed_line "the bar's hide key did not put the keyboard away" ime-surrounding-test-delete-2
      fi
    else
      runner_failed_line "no Hide Keyboard key over the SDL field's keyboard" ime-surrounding-test-delete-2
    fi
  fi
  end_start ime-surrounding-test-delete 2
  # On the iPhone the runner goes again (the runner only).
  [[ "$ON_DEVICE" != yes ]] ||
    on_device uninstall app --device "$DEVICE_ID" io.github.kisame76.minato.uitests.xctrunner >/dev/null 2>&1 || true
fi

# A piece of the keyboard bar's glass that stays after the keyboard goes (device-regressions entry 2, reported in an
# engine's name field): SessionCheck's keyboard-views lists the bar's views on screen in Minato's windows. While the
# keyboard is up the list must have the bar (it sees where the bar lives); after ime-surrounding-test-return hides
# the keyboard at the Return the runner types, as Ren'Py's name field does, nothing of it may stay. The hide key's
# round above asks the same after its hide.
if wanted ime-surrounding-test-return && uitests_build; then
  if begin_start ime-surrounding-test-return io.github.kisame76.minato.test.imesurroundingreturn 1; then
    if await_line guest-log '^I/ime-surrounding-test-return: showSoftInput true$' 120 &&
        await_event 'keyboard show\(' 20; then
      keyboard_views 1
      if [[ "$VIEWS" == *KeyboardBarView* ]]; then ok "  the bar is on screen while the keyboard is up: $VIEWS"; else
        failed_line "the list has no bar while the keyboard is up: $VIEWS" ime-surrounding-test-return-1; fi
      if uitest testKeyboardReturn 1 && await_event 'keyboard hide' 15; then
        ok "  the Return hid the keyboard"
        no_keyboard_views "the Return" ime-surrounding-test-return-1
      else
        runner_failed_line "the Return did not hide the keyboard" ime-surrounding-test-return-1
      fi
      await_line guest-log '^I/ime-surrounding-test-return: done$' 15 || true
    else
      failed_line "no keyboard for the Return" ime-surrounding-test-return-1
    fi
  fi
  end_start ime-surrounding-test-return 1
  # Start 2: the keyboard stays up through the three-finger menu (open, Continue), a trip to the background and back
  # (the return's menu goes by the check's Continue) and a turn to landscape and back; after each step the bar must
  # not stay on screen without the keyboard, and after the Return nothing of it may stay.
  if begin_start ime-surrounding-test-return io.github.kisame76.minato.test.imesurroundingreturn 2; then
    if await_line guest-log '^I/ime-surrounding-test-return: showSoftInput true$' 120 &&
        await_event 'keyboard show\(' 20; then
      bar_with_focus "the keyboard up" ime-surrounding-test-return-2
      request menu
      if await_event "menu opened" 15; then bar_with_focus "the menu and its Continue" ime-surrounding-test-return-2
      else failed_line "the menu did not open" ime-surrounding-test-return-2; fi
      launch "$AWAY"
      sleep 3
      launch
      if await_event "menu continued" 15; then bar_with_focus "a trip to the background" ime-surrounding-test-return-2
      else failed_line "no Continue at the return" ime-surrounding-test-return-2; fi
      if uitest testTurn 2-landscape 0 landscape; then
        bar_with_focus "a turn to landscape" ime-surrounding-test-return-2
        uitest testTurn 2-portrait 0 portrait || fail "  the runner did not turn the device back"
        bar_with_focus "the turn back" ime-surrounding-test-return-2
      else
        runner_failed_line "the runner did not turn the device" ime-surrounding-test-return-2
      fi
      if [[ "$FOCUS" == yes ]]; then
        if uitest testKeyboardReturn 2 && await_event 'keyboard hide' 15; then
          ok "  the Return after them hid the keyboard"
          no_keyboard_views "the Return after them" ime-surrounding-test-return-2
        else
          runner_failed_line "the Return after them did not hide the keyboard" ime-surrounding-test-return-2
        fi
      fi
    else
      failed_line "no keyboard for the steps" ime-surrounding-test-return-2
    fi
  fi
  end_start ime-surrounding-test-return 2
  # Start 3: the edge back with the keyboard up, the BACK key's on Android 15 (tools/edge-back-check.sh): the keyboard
  # goes, and nothing of the bar may stay.
  if begin_start ime-surrounding-test-return io.github.kisame76.minato.test.imesurroundingreturn 3; then
    if await_line guest-log '^I/ime-surrounding-test-return: showSoftInput true$' 120 &&
        await_event 'keyboard show\(' 20; then
      bar_with_focus "the keyboard up" ime-surrounding-test-return-3 1
      if uitest testSwipeFromLeftEdge 3 && await_event 'keyboard (hide|dismissed)' 15; then
        ok "  the edge back hid the keyboard"
        no_keyboard_views "the edge back" ime-surrounding-test-return-3
      else
        runner_failed_line "the edge back did not hide the keyboard" ime-surrounding-test-return-3
      fi
    else
      failed_line "no keyboard for the edge back" ime-surrounding-test-return-3
    fi
  fi
  end_start ime-surrounding-test-return 3
fi

# ADR-0076's starting window, the host's view over the session until the starting activity's first frame:
# starting-window-test on a fresh install (each run imports it new). Start 1, its launcher SplashColorActivity
# (windowSplashScreenBackground #2E7D32, Android 15's splash), shows Android's default for the appearance and keeps the
# colour the guest computed; start 2 shows it from the cache. Start 3, KeepActivity (MINATO_START_INTENT through the
# Debug guest environment), holds its first frame back for 2 s with a pre-draw listener: the view stays that long.
if wanted starting-window-test; then
  swpkg="io.github.kisame76.minato.test.startingwindow"
  guest_environment() {
    if [[ "$ON_DEVICE" == yes ]]; then
      devicectl_copy to --source "$WORK/guest-environment" --destination "tmp/guest-environment" || true
    else
      cp "$WORK/guest-environment" "$CONTAINER/tmp/guest-environment"
    fi
  }
  for round in 1 2 3; do
    activity=SplashColorActivity
    if [[ $round == 3 ]]; then
      activity=KeepActivity
      echo "MINATO_START_INTENT=intent:#Intent;component=$swpkg/.KeepActivity;end" > "$WORK/guest-environment"
      guest_environment
    fi
    if begin_start starting-window-test "$swpkg" "$round" &&
        await_line guest-log "^I/starting-window-test: $activity: drawn" 60; then
      sleep 1
      fetch_events
      line() { sed -n -E "s/^[0-9.]+ [0-9]+ starting window: $1$/\1/p" "$EVENTS" | head -n 1; }
      shown=$(line "(#[0-9A-F]{6} from (cache|default))")
      gone=$(line "gone at the first frame of window [0-9]+ after ([0-9]+) ms")
      case $round in
        1)
          kept=$(line "(#[0-9A-F]{6} kept for the next start, (day|night))")
          default="#FFFFFF from default"
          [[ "$kept" != *night ]] || default="#000000 from default"
          if [[ "$shown" == "$default" ]]; then ok "  start 1: $shown"; else
            fail "  start 1 showed \"$shown\", not \"$default\""; fi
          if [[ "$kept" == "#2E7D32 "* ]]; then ok "  start 1 kept $kept"; else
            fail "  start 1 kept \"$kept\", not Android 15's #2E7D32"; fi
          ;;
        2)
          if [[ "$shown" == "#2E7D32 from cache" ]]; then ok "  start 2: $shown"; else
            fail "  start 2 showed \"$shown\", not \"#2E7D32 from cache\""; fi
          ;;
        3)
          if [[ "$gone" =~ ^[0-9]+$ ]] && (( gone >= 2000 )); then
            ok "  KeepActivity's held frame kept the view $gone ms"
          else
            fail "  KeepActivity's view went after \"$gone\" ms, not at least 2000"
          fi
          ;;
      esac
      [[ -n "$gone" ]] || fail "  start $round: the view did not go at a first frame"
    else
      failed_line "no $activity: drawn" "starting-window-test-$round"
    fi
    end_start starting-window-test "$round"
  done
  : > "$WORK/guest-environment"
  if [[ "$ON_DEVICE" == yes ]]; then guest_environment; else rm -f "$CONTAINER/tmp/guest-environment"; fi
  rm -f "$WORK/guest-environment"
fi

# The same glass check without the MinatoUITests runner, so that it runs on an iPhone whose UI automation is off:
# ime-surrounding-test-hide hides the keyboard by itself 15 s after it came (hideSoftInputFromWindow, as an SDL game
# ends its text input), the user's real path of a guest-initiated hide. The bar must be in the list while the keyboard
# is up and nothing of it may stay after the hide; the app's log must be Android 15's. Its field is empty, as SDL's
# DummyEdit, so the bar holds 0 units. The 15 s leave the iPhone's copies time to ask for the views before the hide
# (pass 6: with 3 s they were asked after it); an answer that came after the hide all the same says so.
if wanted ime-surrounding-test-hide; then
  if begin_start ime-surrounding-test-hide io.github.kisame76.minato.test.imesurroundinghide 1; then
    if await_line guest-log '^I/ime-surrounding-test-hide: showSoftInput true$' 120 &&
        await_event 'keyboard show\(' 20; then
      keyboard_views 1
      if [[ "$VIEWS" == *KeyboardBarView* ]]; then ok "  the bar is on screen while the keyboard is up: $VIEWS"
      elif awk '/^[0-9.]+ [0-9]+ keyboard hide/ { hidden = 1 } hidden && /^[0-9.]+ [0-9]+ keyboard views / { late = 1 }
          END { exit !late }' "$EVENTS"; then
        failed_line "the views were asked after the app's own hide, too late to tell: $VIEWS" \
          ime-surrounding-test-hide-1
      else
        failed_line "the list has no bar while the keyboard is up: $VIEWS" ime-surrounding-test-hide-1; fi
      if await_event 'keyboard hide' 25; then
        ok "  the app's own hide reached Minato"
        no_keyboard_views "the app's own hide" ime-surrounding-test-hide-1
      else
        failed_line "the app's own hide did not hide the keyboard" ime-surrounding-test-hide-1
      fi
      await_line guest-log '^I/ime-surrounding-test-hide: done$' 15 || true
    else
      failed_line "no keyboard for the app's own hide" ime-surrounding-test-hide-1
    fi
  fi
  end_start ime-surrounding-test-hide 1
  python3 - "$WORK/ime-surrounding-test-hide-1.log" fixtures/expected/ime-surrounding-test-hide.logcat <<'PY' ||
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

log, expected = sys.argv[1:]
compare_log("ime-surrounding-test-hide",
            subprocess.CompletedProcess([], 0, "", open(log, encoding="utf-8", errors="replace").read()), expected, {})
PY
    failures=$((failures + 1))
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
# WEBVIEW_TOUCH: webview-touch-test (ADR-0056 c2, test 3); its points come from its device lines.
if [[ -n "${WEBVIEW_TOUCH:-}" ]]; then
  if begin_start webview-touch-test io.github.kisame76.minato.test.webviewtouch 1; then
    if await_line guest-log '^I/webview-touch-test: ready$' 120; then
      sed -n -E 's/^I\/webview-touch-test-device: (.*)$/point   \1/p' "$CHECK/guest-log"
      echo "touch   now, a second apart: the Button, the tap point, a swipe up along the swipe line, and after the"
      echo "        app's \"scrolled back\" the field point"
      if await_line guest-log '^I/webview-touch-test: done$' 300; then
        ok "  its last line, $(seconds "$SEEN" "$(now)") s ago"
        sleep 2
      else
        failed_line 'no "done" within 300 s' webview-touch-test-1
      fi
    else
      failed_line 'no "ready"' webview-touch-test-1
    fi
  fi
  end_start webview-touch-test 1
  python3 - "$WORK/webview-touch-test-1.log" fixtures/expected/webview-touch-test.logcat <<'PY' \
    || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

path, expected = sys.argv[1:]
compare_log("webview-touch-test", subprocess.CompletedProcess([], 0, "", open(path, encoding="utf-8",
                                                                             errors="replace").read()), expected, {})
PY
fi

# HANG: hang-test; the host's menu and Quit while its main thread hangs.
if [[ -n "${HANG:-}" ]]; then
  QUIT_AFTER=12
  if begin_start hang-test io.github.kisame76.minato.test.hang 1; then
    if ! await_line guest-log '^I/hang-test: hanging$' 120; then
      failed_line 'no "hanging"' hang-test-1
    elif ! await_event "menu opened" 30; then
      failed_line "no menu while the guest hangs" hang-test-1
    elif ! await_event "quit" 15; then
      failed_line "no Quit while the guest hangs" hang-test-1
    else
      quit_at=${EVENT%% *}
      if await_event "process library.*" 15; then
        ok "  the library is back $(seconds "$quit_at" "${EVENT%% *}") s after the Quit, the guest hung"
      else
        failed_line "no library within 15 s of the Quit" hang-test-1
      fi
    fi
  fi
  QUIT_AFTER=
  end_start hang-test 1
  sed 's/^/      | /' "$WORK/hang-test-1.log" | grep "hang-test" || true
fi

# COMPILE: one app's plain start, its compile in compile mode and its compiled start (M6 c, E6).
run_compile_measure() {
  local app=$1 package=$2 file pattern plain="" compiled="" status="" peak="" odex="" vdex="" container="" taking=""
  local began ended dex_mb
  read -r file pattern <<< "$(last_line "$app")"
  QUIT_AFTER=${COMPILE_QUIT_AFTER:-60}
  if begin_start "$app" "$package" 1; then
    if await_line "$file" "$pattern" 600; then
      plain=$(seconds "$TAP" "$SEEN")
      ok "  plain: its last line $plain s after the tap"
    else
      failed_line "no last line of the plain start" "$app-1"
    fi
    # The Quit ends the session; it asks for compile mode, which the app starts as a process of its own. An app that
    # ends itself first (Godot kills itself after its run) is left without the request: a short start with a Quit asks.
    if [[ -n "$plain" ]] && ! await_event "process compileMode $package.*" "${COMPILE_WAIT:-100}"; then
      end_start "$app" 1
      QUIT_AFTER=3
      if begin_start "$app" "$package" 3; then await_event "process compileMode $package.*" 240 || true; fi
    fi
    if [[ -n "$plain" && -n "$EVENT" ]] && await_event "compile started $package" 60; then
      began=${EVENT%% *}
      if await_event "compile ended .*" 900; then
        ended=${EVENT%% *}
        status=$(sed -E 's/^[0-9.]+ [0-9]+ compile ended ([0-9-]+).*/\1/' <<< "$EVENT")
        peak=$(sed -nE 's/.* peak ([0-9-]+)$/\1/p' <<< "$EVENT")
        if [[ "$status" == 0 ]]; then
          ok "  dex2oat: $(seconds "$began" "$ended") s, peak $peak MB"
        else
          fail "  dex2oat ended with status $status"
        fi
      else
        failed_line "compile mode did not end" "$app-1"
      fi
      if [[ "$status" == 0 ]]; then
        # The library process signs and keeps the code at its start, after the restart.
        if await_event "compiled kept $package .*" 240; then
          read -r odex vdex container taking <<< "$(sed -E \
            's/.* odex ([0-9]+) vdex ([0-9]+) container ([0-9]+) taking ([0-9.]+)$/\1 \2 \3 \4/' <<< "$EVENT")"
          ok "  kept: odex $odex, vdex $vdex, container $container bytes, taking $taking s"
        else
          failed_line "the library did not keep the compiled code" "$app-1"
        fi
      fi
    elif [[ -n "$plain" ]]; then
      failed_line "no compile mode after the Quit" "$app-1"
    fi
  fi
  QUIT_AFTER=
  end_start "$app" 1
  if [[ -n "$container" ]]; then
    QUIT_AFTER=${COMPILE_QUIT_AFTER:-60}
    # E4's proof, in the Simulator: ART's -verbose:oat log (the Debug guest environment) must say the library's own
    # odex is the best oat file and executable. begin_start leaves tmp/guest-environment alone.
    [[ "$ON_DEVICE" == yes ]] || echo "MINATO_MEASURE_ART_OPTIONS=-verbose:oat" > "$CONTAINER/tmp/guest-environment"
    if begin_start "$app" "$package" 2; then
      if await_line "$file" "$pattern" 600; then
        compiled=$(seconds "$TAP" "$SEEN")
        ok "  compiled: its last line $compiled s after the tap"
      else
        failed_line "no last line of the compiled start" "$app-2"
      fi
    fi
    QUIT_AFTER=
    end_start "$app" 2
    if [[ "$ON_DEVICE" != yes ]]; then
      rm -f "$CONTAINER/tmp/guest-environment"
      if grep -q "/oat/arm64/base.odex (executable=1)" "$WORK/$app-2.log"; then
        ok "  oat proof: ART runs the library's own odex, executable"
      else
        failed_line "no oat proof: the compiled start's log does not say its odex is executable" "$app-2"
        grep -i "oat" "$WORK/$app-2.log" | head -n 5 | sed 's/^/      | /' || true
      fi
    fi
  fi
  dex_mb=$(python3 - "$(apk "$app")" <<'PY'
import sys
import zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    dex = [i.file_size for i in archive.infolist() if i.filename.startswith("classes") and i.filename.endswith(".dex")]
    print(f"{sum(dex) / 1048576:.2f}")
PY
)
  python3 - "$app" "$dex_mb" "${plain:-}" "${compiled:-}" "${began:-}" "${ended:-}" "${peak:-}" "${odex:-}" \
    "${vdex:-}" "${container:-}" "${taking:-}" <<'PY'
import sys
app, dex, plain, compiled, began, ended, peak, odex, vdex, container, taking = sys.argv[1:]
def number(text):
    return float(text) if text else None
def cell(value, digits):
    return "" if value is None else f"{value:.{digits}f}"
seconds = None if not began or not ended else float(ended) - float(began)
ratio = None if not plain or not compiled else float(compiled) / float(plain)
per_mb = None if seconds is None or float(dex) == 0 else seconds / float(dex)
peak_per_mb = None if not peak or int(peak) < 0 or float(dex) == 0 else int(peak) / float(dex)
print("csv," + ",".join([app, dex, cell(number(plain), 1), cell(number(compiled), 1), cell(ratio, 2), cell(seconds, 1),
                         cell(per_mb, 2), peak, cell(peak_per_mb, 1), odex, vdex, container, taking]))
PY
}
# Ren'Py with an early Interface start: SDL must have its window, so set_mode does not fail (the log's "Could not set
# video mode." with "Could not fetch native window" is the failure that exits the game with status 1).
if wanted renpy-splash-test && listed renpy-splash-test; then
  # The log is taken at the last line: a compile mode that follows the session truncates the guest log.
  seen_log=$WORK/renpy-splash-test-seen.log
  : > "$seen_log"
  if begin_start renpy-splash-test io.github.kisame76.minato.test.renpy 1; then
    if await_line guest-log "^I/python *: minato-renpy-test: done$" 300; then
      cp "$CHECK/guest-log" "$seen_log"
      ok "  its last line, $(seconds "$TAP" "$SEEN") s after the tap"
    else
      failed_line "no last line" "renpy-splash-test-1"
    fi
  fi
  end_start renpy-splash-test 1
  cp "$seen_log" "$WORK/renpy-splash-test-1.log"
  if grep -q "Could not set video mode\|Could not fetch native window" "$seen_log"; then
    fail "  renpy-splash-test: SDL had no window at Interface start"
    grep -E "native window|video mode|Interface start" "$seen_log" | head -n 5 | sed 's/^/      | /' || true
  elif grep -q "Interface start took" "$seen_log"; then
    ok "  renpy-splash-test: set_mode got its window ($(grep -m1 'Interface start took' "$seen_log" | sed 's/.*took //'))"
  else
    fail "  renpy-splash-test: no Interface start in the log"
  fi
fi
# The runtime's 1 Hz memory picture (memory_watermark.h) from a round's guest log $1, kept in $WORK past the run.
# Printed as "note  mem ..." lines (the instrument's numbers, no paths): the first sample, the one with the
# highest foot=, every memory-warning sample, and the last; de-duplicated when those pick the same line.
mem_notes() {
  [[ -s "$1" ]] || return 0
  awk '
    function emit(s,   k) {
      for (k = 1; k <= nseen; k++) if (seen[k] == s) return
      seen[++nseen] = s; print "note  " s
    }
    /^I\/apkrt *: mem foot=/ {
      s = $0; sub(/^I\/apkrt *: /, "", s)
      if (first == "") first = s
      last = s
      f = s; sub(/^mem foot=/, "", f); sub(/[^0-9].*/, "", f)
      if (f + 0 > peakv) { peakv = f + 0; peak = s }
      if (s ~ / warn$/) warn[++nw] = s
    }
    END {
      if (first == "") exit 0
      emit(first); emit(peak)
      for (i = 1; i <= nw; i++) emit(warn[i])
      emit(last)
    }
  ' "$1"
}
# Minato's Ren'Py memory game for MINUTES: the footprint over time and the memory warnings it got. The game says more
# scenes than MINUTES can play, so the sitting ends it, not the game; each warning reaches the guest as one onLowMemory
# once it has started (Lifecycle's memoryWarning), so the two counts are the warnings since the guest's start.
if wanted renpy-memory-test && listed renpy-memory-test; then
  minutes=${MINUTES:-15}
  environment=$WORK/guest-environment
  echo "MINATO_SCENES=$((minutes * 120))" > "$environment"
  if [[ "$ON_DEVICE" == yes ]]; then
    devicectl_copy to --source "$environment" --destination "tmp/guest-environment"
  else
    cp "$environment" "$CONTAINER/tmp/guest-environment"
  fi
  if begin_start renpy-memory-test io.github.kisame76.minato.test.renpymemory 1; then
    end=$((SECONDS + minutes * 60))
    ended=no
    # iOS's memory kill (jetsam) ends the whole process and writes no event of Minato's, so the round also watches the
    # footprint log, which gets a line every 10 s while the process lives: none for 30 s ends the wait early, and
    # tools/renpy_footprint.py --lived judges the whole log afterwards (the iPhone's red run of 2026-11-10 was killed
    # 15 s in and still said "played for 5 min").
    lines=0
    changed=$SECONDS
    # The frame rate: the session's ping answers with the frames its windows have presented so far, so each answer
    # gives the rate since the one before (texture parking restores a frame of a cycled sequence per shown frame).
    rates=()
    presents=-1
    pinged=$SECONDS
    while (( SECONDS < end )); do
      sleep 20
      fetch_events
      if tail -n +$((seen + 1)) "$EVENTS" | grep -E '^[0-9.]+ [0-9]+ (could not start|program ended) ' >/dev/null; then
        ended=yes
        break
      fi
      request ping
      EVENT=""
      await_event "pong presents [0-9]+ scene .*|could not start .*|program ended .*" 10 || true
      if [[ "$EVENT" =~ (could\ not\ start|program\ ended) ]]; then
        ended=yes
        break
      elif [[ "$EVENT" =~ pong\ presents\ ([0-9]+) ]]; then
        if (( presents >= 0 && SECONDS > pinged )); then
          rates+=($(( (BASH_REMATCH[1] - presents) / (SECONDS - pinged) )))
        fi
        presents=${BASH_REMATCH[1]}
        pinged=$SECONDS
      fi
      # A failed fetch (the iPhone's copy) says nothing: the container stays readable after a kill.
      if fetch "$CHECK_REL/footprint-log"; then
        now_lines=$(wc -l <"$CHECK/footprint-log")
        if (( now_lines > lines )); then
          lines=$now_lines
          changed=$SECONDS
        elif (( SECONDS - changed > 30 )); then
          break
        fi
      fi
    done
    fetch "$CHECK_REL/footprint-log" || true
    if [[ "$ended" == yes ]]; then
      failed_line "the game ended before $minutes min" "renpy-memory-test-1"
    elif ! killed=$(python3 tools/renpy_footprint.py --lived "$CHECK/footprint-log" "$minutes"); then
      failed_line "$killed" "renpy-memory-test-1"
    else
      [[ -z "$killed" ]] || echo "$killed"
      ok "  played for $minutes min"
    fi
    if (( ${#rates[@]} > 0 )); then
      sorted=($(printf '%s\n' "${rates[@]}" | sort -n))
      echo "csv,renpy_fps_min,renpy_fps_median,renpy_fps_last"
      echo "csv,${sorted[0]},${sorted[$(( ${#sorted[@]} / 2 ))]},${rates[$(( ${#rates[@]} - 1 ))]}"
    else
      echo "note  renpy-memory-test: no ping answered, so no frame rate"
    fi
    # The ANGLE parking line (the runtime's memory watermark line of the guest log): parks, restores, present
    # times. Without it, the guest log's line counts tell why: no mem lines = the log is stale or cut short, mem lines
    # but no gl line = libGLESv2's line symbols did not resolve, mem lines that stop early = the sampler stalled.
    fetch "$CHECK_REL/guest-log" || true
    park=$(grep "angle park " "$CHECK/guest-log" 2>/dev/null | tail -n 1 || true)
    if [[ -n "$park" ]]; then echo "note  $park"; else
      count() { grep -c "$1" "$CHECK/guest-log" 2>/dev/null || true; }
      echo "note  renpy-memory-test: no angle park line in the guest log (guest log: $(count '') lines," \
        "$(count '^I/apkrt   : mem ') mem, $(count '^I/apkrt   : gl ') gl, $(count 'angle park ') park)"
    fi
  fi
  end_start renpy-memory-test 1
  # The guest log's 1 Hz memory picture, which $WORK keeps while the on-device folder is cleaned up: the
  # first sample, the highest foot=, every warning sample and the last, so a run shows the "other" split.
  mem_notes "$WORK/renpy-memory-test-1.log"
  # An empty file is no guest environment (the script cannot delete in the iPhone's container).
  if [[ "$ON_DEVICE" == yes ]]; then
    : > "$WORK/guest-environment"
    devicectl_copy to --source "$WORK/guest-environment" --destination "tmp/guest-environment" || true
  else
    rm -f "$CONTAINER/tmp/guest-environment"
  fi
  # renpy-memory-test's footprint summary (tools/renpy_footprint.py): the CSV of its min/max/end footprint and memory
  # warnings. A run with no footprint sample is a device fetch or timing artifact, so a note, not a failure; the
  # "game ended before N min" check above stays the real gate on the run.
  python3 tools/renpy_footprint.py "$CHECK/footprint-log" || fail "  renpy-memory-test: footprint summary failed"
fi
if [[ -n "${COMPILE:-}" ]]; then
  echo "csv,app,dex_mb,plain_s,compiled_s,compiled_over_plain,dex2oat_s,dex2oat_s_per_dex_mb,peak_mb,"\
"peak_mb_per_dex_mb,odex_bytes,vdex_bytes,container_bytes,take_s"
  wanted compute-app-test && run_compile_measure compute-app-test io.github.kisame76.minato.test.computeapp
  wanted godot-bench-gles && run_compile_measure godot-bench-gles io.github.kisame76.minato.godotbench.gles
fi

echo "java start check: $failures failed"
exit $((failures > 0))
