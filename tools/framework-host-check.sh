#!/usr/bin/env bash
# Checks the framework's plain-Java parts on the host's JVM (ADR-0045 step b): the classes under
# framework/java/com/android/minato that use no android.* class, with the tests under framework/tests/java, which
# run without JUnit (each test class has its own main). Today: ADR-0045 decision 3's stubs (ServiceStub) and the
# managers' generator (tools/managergen), and the policy and table of decision 6's runtime permissions:
#   - framework/tests/managergen_test.py checks the generator on framework/tests/managergen's samples, whose
#     output goes into the build folder for GeneratedSampleTest;
#   - tools/tests/stub_supertypes_test.py checks framework-check's guard of stub supertypes (b3.5a);
#   - tools/tests/stub_references_test.py checks its guard of run-time references to stubs (step c2a);
#   - tools/tests/compatgen_test.py checks the compat changes' table, which tools/build-framework.sh generates
#     (step c2e);
#   - tools/tests/appopsgen_test.py checks the app op table, which tools/build-framework.sh generates too (ADR-0045's
#     storage and app ops step);
#   - framework/tests/certificate_mask_test.py checks the harnesses' marks for the certificates a test APK's check
#     proved (b3.5b1);
#   - framework/tests/app_process_run_test.py checks that the harness converts a container again unless it was made
#     from the very ELF file given (step c1);
#   - framework/tests/event_log_tags_test.py checks tools/event-log-tags.py, which writes EventLogTags for
#     framework.jar (ADR-0039 step c2c3);
#   - framework/tests/keycode_labels_test.py checks KeyEvent's native key labels against KeyEvent.java (step c2c3);
#   - framework/tests/axis_labels_test.py checks the native axis labels against MotionEvent.java (ADR-0045 step 7b1);
#   - managergen --implement (ADR-0039 b3.5a) writes SampleAppManager from framework/tests/managergen/java's
#     SampleBaseManager, for ImplementedSampleTest;
#   - Wide, an interface of 300 methods as big as IActivityManager, is generated into the build folder;
#   - org.json, which the guest has in core-libart and the host's JDK lacks, is compiled from libcore's sources for
#     SignersFile, the guest's reader of signers.json (b3.5b1), with the annotations it names; UnsupportedAppUsage,
#     which the fetched sources lack, is an empty one of ours;
#   - javac 17, then each *Test class's main; any FAIL fails the check.
# The JDK and libcore's sources come from tools/fetch-sources.py --set art.
# Usage: tools/framework-host-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

JDK=$(echo build/tools/jdk-17/*/Contents/Home)
OUT=build/framework-host
if [[ ! -x "$JDK/bin/javac" ]]; then
  echo "No JDK: run tools/fetch-sources.py --set art first." >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/gen/com/android/minato/services" "$OUT/classes" "$OUT/json" "$OUT/json-src/android/compat/annotation"
{
  echo "package com.android.minato.services;"
  echo "public interface Wide {"
  for i in $(seq 0 299); do echo "    int m$i();"; done
  echo "}"
} >"$OUT/gen/com/android/minato/services/Wide.java"

python3 framework/tests/managergen_test.py
# framework-check's guard of stub supertypes and its allowlist's reasons (b3.5a).
python3 tools/tests/stub_supertypes_test.py
# framework-check's guard of run-time references to stubs and its list's reasons (step c2a).
python3 tools/tests/stub_references_test.py
# The compat changes' table and what its generator refuses (step c2e).
python3 tools/tests/compatgen_test.py
# The app op table and what its generator refuses (ADR-0045's storage and app ops step).
python3 tools/tests/appopsgen_test.py
# The harnesses' marks for the certificates a test APK's check proved (b3.5b1).
python3 framework/tests/certificate_mask_test.py
# The harness's containers, made again unless made from the very file (step c1).
python3 framework/tests/app_process_run_test.py
# ADR-0045 decision 6's protection levels in our framework-res against the AVD's; skipped while not built.
python3 framework/tests/platform_permissions_test.py
# The event log tags generator, which writes EventLogTags for framework.jar (ADR-0039 step c2c3).
python3 framework/tests/event_log_tags_test.py
# KeyEvent's key labels in libandroid_runtime against KeyEvent.java's constants (ADR-0039 step c2c3).
python3 framework/tests/keycode_labels_test.py
# The axis labels of libinput's stand-in in libandroid_runtime against MotionEvent.java's constants (ADR-0045 7b1).
python3 framework/tests/axis_labels_test.py
python3 tools/managergen/managergen.py --api framework/tests/managergen/sample-current.txt \
  --hand framework/tests/managergen/hand --out "$OUT/gen" \
  --class com.android.minato.sample.SampleManager --class com.android.minato.sample.SampleDevice \
  --class com.android.minato.sample.SampleFormat --class com.android.minato.sample.SampleCallback \
  --class com.android.minato.sample.SampleListener --class com.android.minato.sample.SampleBase \
  --class com.android.minato.sample.SampleChild --class com.android.minato.sample.SampleOther \
  --class com.android.minato.sample.SampleShape --class com.android.minato.sample.SampleSquare
# ADR-0039 b3.5a's --implement: SampleAppManager extends the AOSP-like abstract SampleBaseManager, as
# ApplicationPackageManager extends PackageManager; ImplementedSampleTest runs it.
python3 tools/managergen/managergen.py \
  --implement framework/tests/managergen/java/com/android/minato/sample/pm/SampleBaseManager.java \
  --as com.android.minato.sample.app.SampleAppManager --hand framework/tests/managergen/hand --out "$OUT/gen"

LIBCORE=build/sources/libcore
printf 'package android.compat.annotation;\n\npublic @interface UnsupportedAppUsage {\n}\n' \
  >"$OUT/json-src/android/compat/annotation/UnsupportedAppUsage.java"
"$JDK/bin/javac" -source 17 -target 17 -encoding UTF-8 -nowarn -d "$OUT/json" \
  "$LIBCORE"/json/src/main/java/org/json/*.java "$LIBCORE"/luni/src/main/java/libcore/util/{NonNull,Nullable}.java \
  "$LIBCORE/luni/src/main/java/libcore/api/IntraCoreApi.java" \
  build/sources/modules_utils/java/android/annotation/SystemApi.java \
  "$OUT/json-src/android/compat/annotation/UnsupportedAppUsage.java"

"$JDK/bin/javac" -source 17 -target 17 -encoding UTF-8 -Xlint:all -Werror -cp "$OUT/json" -d "$OUT/classes" \
  $(find framework/java/com/android/minato framework/tests/java framework/tests/managergen/java "$OUT/gen" \
    -name '*.java' | sort)

failed=0
for test in $(cd framework/tests/java && find . -name '*Test.java' | sed 's|^\./||; s|\.java$||; s|/|.|g' | sort); do
  "$JDK/bin/java" -cp "$OUT/classes:$OUT/json" "$test" || failed=$((failed + 1))
done
if [[ $failed -gt 0 ]]; then
  echo "framework host check: $failed test classes failed"
  exit 1
fi
echo "framework host check: fine"
