#!/usr/bin/env bash
# Checks the host aidl of tools/build-aidl.sh (ADR-0045 decision 1, step a), the tool that writes the
# Java of AIDL interfaces for the framework's build:
#   golden       AOSP's own test interfaces of the permission annotations, run with the command line that
#                each golden file names in its "Using:" line, from a folder that has that command's paths;
#                the Java must equal the golden file byte for byte
#   our-check    an interface of ours with a structured parcelable, an enum, an in argument and a oneway
#                call; its Stub.asInterface must hand back a local object through queryLocalInterface, as
#                the local services of ADR-0045 decision 2 rely on
# Usage: tools/aidl-check.sh
# Results: build/aidl-check/
set -euo pipefail
cd "$(dirname "$0")/.."

AIDL=build/aidl/aidl
SOURCES=build/sources/aidl
OUT=build/aidl-check
GOLDEN=tests/golden_output/aidl-test-interface-permission-java-source/gen/android/aidl/tests/permission

if [[ ! -x "$AIDL" ]]; then
  echo "No $AIDL: run tools/build-aidl.sh first." >&2
  exit 1
fi
if [[ ! -d "$SOURCES" ]]; then
  echo "No $SOURCES: run tools/fetch-sources.py --set framework first." >&2
  exit 1
fi

rm -rf "$OUT"
failed=0
checks=0

# The golden files were made in AOSP's tree with aidl at out/host/linux-x86/bin and the sources at
# system/tools/aidl, and the header of each names that command line: the same paths lead here to ours.
tree="$OUT/tree"
mkdir -p "$tree/out/host/linux-x86/bin" "$tree/system/tools" "$tree/frameworks/base/core/java"
ln -s "$PWD/$AIDL" "$tree/out/host/linux-x86/bin/aidl"
ln -s "$PWD/$SOURCES" "$tree/system/tools/aidl"
for name in INoPermission IProtected IProtectedInterface; do
  checks=$((checks + 1))
  golden="$SOURCES/$GOLDEN/$name.java"
  command=$(sed -n 's/^ \* Using: //p' "$golden")
  if [[ "$command" != "out/host/linux-x86/bin/aidl --lang=java "* ]]; then
    echo "FAILED   golden $name: $golden names no aidl --lang=java command"
    failed=$((failed + 1))
    continue
  fi
  # The command line is AOSP's, without quotes or variables: its words are the arguments.
  read -r -a words <<<"$command"
  if ! output=$(cd "$tree" && "${words[@]}" 2>&1); then
    echo "FAILED   golden $name: aidl says: $output"
    failed=$((failed + 1))
    continue
  fi
  generated="$tree/out/soong/.intermediates/system/tools/aidl/aidl-test-interface-permission-java-source/gen/android/aidl/tests/permission/$name.java"
  if ! diff -u "$golden" "$generated" >"$OUT/$name.diff"; then
    echo "FAILED   golden $name: differs from AOSP's golden output, see $OUT/$name.diff"
    failed=$((failed + 1))
    continue
  fi
  echo "ok       golden $name: equals AOSP's golden output"
done

checks=$((checks + 1))
ours="$OUT/ours"
package=io/github/kisame76/minato/check
if ! output=$("$AIDL" --lang=java -o "$ours" -I tools/aidl/check tools/aidl/check/$package/Point.aidl \
    tools/aidl/check/$package/Mode.aidl tools/aidl/check/$package/ICheck.aidl 2>&1); then
  echo "FAILED   our-check: aidl says: $output"
  failed=$((failed + 1))
else
  problems=""
  interface="$ours/$package/ICheck.java"
  # asInterface returns the object the Binder holds in the process, and a proxy only without one.
  for expected in "android.os.IInterface iin = obj.queryLocalInterface(DESCRIPTOR);" \
      "if (((iin!=null)&&(iin instanceof io.github.kisame76.minato.check.ICheck))) {" \
      "return ((io.github.kisame76.minato.check.ICheck)iin);" \
      "android.os.IBinder.FLAG_ONEWAY" \
      'public static final java.lang.String DESCRIPTOR = "io.github.kisame76.minato.check.ICheck";'; do
    grep -qF -- "$expected" "$interface" || problems+=" ICheck.java lacks: $expected;"
  done
  grep -qF "public class Point implements android.os.Parcelable" "$ours/$package/Point.java" \
    || problems+=" Point.java is no Parcelable;"
  grep -qF "public static final int ON = 1;" "$ours/$package/Mode.java" || problems+=" Mode.java lacks ON = 1;"
  if [[ -n "$problems" ]]; then
    echo "FAILED   our-check:$problems"
    failed=$((failed + 1))
  else
    echo "ok       our-check: asInterface takes the local object, oneway, parcelable and enum as expected"
  fi
fi

if [[ $failed -gt 0 ]]; then
  echo "aidl check: $failed of $checks failed"
  exit 1
fi
echo "aidl check: $checks of $checks fine"
