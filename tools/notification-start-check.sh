#!/usr/bin/env bash
# Runs ActivityThread's own check of a cold tap's content intent (ADR-0074 decision 4) on the Mac
# (framework/tests/java_app_run.py): notify-tap-test starts with MINATO_START_INTENT set, as GameSession sets it after
# the host's check, once with an intent of the app's own TapActivity, which starts in place of the launcher, and once
# each with an intent ActivityThread must refuse: another app's activity, a selector, an implicit intent, the package
# field of another app, an activity the app does not have and a URI Intent.parseUri cannot read. A refused one starts
# the launcher (the framework's android.app.Activity), and the app's own send of its content intent a second later
# opens TapActivity only then. The host refuses all of them before, so only this check reaches ActivityThread's.
# Builds apk-run and apk-convert for macOS into build/native/macos-app, app_process with libandroid_runtime.so,
# framework.jar and the app every time, as tools/permission-check.sh does.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/notification-start-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh notify-tap-test >/dev/null

APP=io.github.kisame76.minato.test.notifytap
OWN="$APP/.TapActivity"
failed=0
# One start with MINATO_START_INTENT=$1; the rest are java_app_run.py's options for it. A failed one is counted.
start() {
  local intent=$1
  shift
  python3 framework/tests/java_app_run.py --lifecycle start --env "MINATO_START_INTENT=$intent" "$@" \
    notify-tap-test "$APP" "$APP.TapApplication" build/art build/bionic/lib build/bionic/system \
    build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
    "$NATIVE/runtime/apk-run" build/notification-start-check build/framework-native/app_process64 \
    build/framework/framework.jar build/framework-res/framework-res \
    build/notify-tap-test/notify-tap-test.apk fixtures/expected/notify-tap-test-cold.txt 1290x2796@3,en-US \
    || failed=$((failed + 1))
}
# A refused intent: ActivityThread's warning, the launcher, then the app's own send.
refused() {
  local why=$1 intent=$2 warning=$3
  echo "== $why"
  start "$intent" --in-order "W/ActivityThread: a notification's start intent that $warning" \
    --in-order "I/ActivityTaskManager: Displayed $APP/android.app.Activity" \
    --in-order "I/notify-tap-test: sent the content intent" \
    --in-order "I/notify-tap-test: opened by the tap: room 7"
}

echo "== the app's own activity"
start "intent:#Intent;component=$OWN;S.room=7;end" \
  --in-order "I/notify-tap-test: opened by the tap: room 7" --in-order "I/notify-tap-test: sent the content intent"
refused "another app's activity" "intent:#Intent;component=org.example.other/.TapActivity;S.room=7;end" \
  "is not an activity of $APP"
refused "a selector" "intent:#Intent;component=$OWN;S.room=7;SEL;action=android.intent.action.VIEW;end" \
  "is not an activity of $APP"
refused "an implicit intent" "intent:#Intent;action=android.intent.action.VIEW;package=$APP;S.room=7;end" \
  "is not an activity of $APP"
refused "another app's package field" "intent:#Intent;package=org.example.other;component=$OWN;S.room=7;end" \
  "is not an activity of $APP"
refused "an activity the app does not have" "intent:#Intent;component=$APP/.Elsewhere;S.room=7;end" \
  "names no activity of $APP"
refused "a URI that does not parse" "intent:#Intent;component=$OWN" "does not parse"
if ((failed)); then
  echo "notification start check: $failed of 7 starts failed"
  exit 1
fi
echo "notification start check: fine"
