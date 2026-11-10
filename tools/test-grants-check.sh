#!/usr/bin/env bash
# Pins that MINATO_TEST_GRANTS, the Mac harness's grant of calendar, call log and voicemail permissions to a test app
# (framework/java/android/app/ManifestPackage.java, set by apk-run's --env), is never set by the iPhone's side: no file
# of the app, the importer or the graphics host (the code that starts a guest on iOS) may name it. It also checks that
# the framework reads it once, as a constant, so that an app's own setenv later grants nothing.
# Usage: tools/test-grants-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

status=0
found=$(grep -rl "MINATO_TEST_GRANTS" app importer graphics runtime/src 2>/dev/null || true)
if [[ -n "$found" ]]; then
  echo "FAIL    MINATO_TEST_GRANTS is named where the iPhone's guest start lives:"
  echo "$found" | sed 's/^/      | /'
  status=1
fi
if ! grep -q 'static final java.util.Set<String> TEST_GRANTS = testGrants(System.getenv("MINATO_TEST_GRANTS"))' \
  framework/java/android/app/ManifestPackage.java; then
  echo "FAIL    ManifestPackage no longer reads MINATO_TEST_GRANTS once, as a constant"
  status=1
fi
[[ "$status" != 0 ]] || echo "ok      MINATO_TEST_GRANTS is only the Mac harness's, read once"
exit "$status"
