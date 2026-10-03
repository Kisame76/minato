#!/usr/bin/env bash
# Checks the framework's plain-Java parts on the host's JVM (ADR-0045 step b): the classes under
# framework/java/com/android/minato that use no android.* class, with the tests under framework/tests/java, which
# run without JUnit (each test class has its own main). Today: ADR-0045 decision 3's stubs (ServiceStub) and the
# managers' generator (tools/managergen), and the policy and table of decision 6's runtime permissions:
#   - framework/tests/managergen_test.py checks the generator on framework/tests/managergen's samples, whose
#     output goes into the build folder for GeneratedSampleTest;
#   - tools/tests/stub_supertypes_test.py checks framework-check's guard of stub supertypes (b3.5a);
#   - managergen --implement (ADR-0039 b3.5a) writes SampleAppManager from framework/tests/managergen/java's
#     SampleBaseManager, for ImplementedSampleTest;
#   - Wide, an interface of 300 methods as big as IActivityManager, is generated into the build folder;
#   - javac 17, then each *Test class's main; any FAIL fails the check.
# The JDK comes from tools/fetch-sources.py --set art.
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
mkdir -p "$OUT/gen/com/android/minato/services" "$OUT/classes"
{
  echo "package com.android.minato.services;"
  echo "public interface Wide {"
  for i in $(seq 0 299); do echo "    int m$i();"; done
  echo "}"
} >"$OUT/gen/com/android/minato/services/Wide.java"

python3 framework/tests/managergen_test.py
# framework-check's guard of stub supertypes and its allowlist's reasons (b3.5a).
python3 tools/tests/stub_supertypes_test.py
# ADR-0045 decision 6's protection levels in our framework-res against the AVD's; skipped while not built.
python3 framework/tests/platform_permissions_test.py
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

"$JDK/bin/javac" -source 17 -target 17 -encoding UTF-8 -Xlint:all -Werror -d "$OUT/classes" \
  $(find framework/java/com/android/minato framework/tests/java framework/tests/managergen/java "$OUT/gen" \
    -name '*.java' | sort)

failed=0
for test in $(cd framework/tests/java && find . -name '*Test.java' | sed 's|^\./||; s|\.java$||; s|/|.|g' | sort); do
  "$JDK/bin/java" -cp "$OUT/classes" "$test" || failed=$((failed + 1))
done
if [[ $failed -gt 0 ]]; then
  echo "framework host check: $failed test classes failed"
  exit 1
fi
echo "framework host check: fine"
