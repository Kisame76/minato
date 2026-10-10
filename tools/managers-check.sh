#!/usr/bin/env bash
# Checks ADR-0045 decision 3's generated managers: tools/managergen writes AudioManager from core/api/current.txt
# and every public class of the Connectivity (with its T part) and Wi-Fi modules' API files, with the hand files of
# framework/managers, into build/managers/gen; and, as compile-only stubs in build/managers/stubs, the core types
# they reach through their signatures, as android.jar would give them; a stub leaves out a member that names a type
# in no API file (another module's). libcore's and ICU's API files are the libraries': their types are the boot
# jars'. location/api/current.txt gives the location types the Wi-Fi module names. Then:
#   - javac 17 compiles both against the boot jars' system image (tools/build-framework.sh), as framework.jar's
#     classes are, with every warning an error but those the API's own signatures bring: deprecated types, raw
#     types, generic varargs, and Throwables without a serialVersionUID, which no API file lists. A hand file's
#     bodies may call framework.jar's own types, as they do in its build (TrafficStats' Log, StrictMode and
#     Process): its classes and stubs.jar stand behind the sources;
#   - each of the three managers has the hidden ctor (android.content.Context) that SystemServiceRegistry calls.
# The stubs never go into framework.jar; ADR-0039 b3/c's build of frameworks/base takes their place. Prints the
# counts of classes, methods and stubs.
# Needs the API files of tools/fetch-sources.py --set framework, libcore's of --set art, and build/framework/system,
# classes.jar and stubs.jar of tools/build-framework.sh.
# Usage: tools/managers-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

JDK=$(echo build/tools/jdk-17/*/Contents/Home)
SOURCES=build/sources
OUT=build/managers
LIBCORE=$SOURCES/libcore/api/current.txt
ICU=$SOURCES/icu/android_icu4j/api/public/current.txt
LOCATION=$SOURCES/location_api/location/api/current.txt
CORE=$SOURCES/frameworks_base_api/core/api/current.txt
CONNECTIVITY=$SOURCES/connectivity_api/framework/api/current.txt
CONNECTIVITY_T=$SOURCES/connectivity_t_api/framework-t/api/current.txt
WIFI=$SOURCES/wifi_api/framework/api/current.txt
for needed in "$JDK/bin/javac" "$LIBCORE" "$ICU" "$LOCATION" "$CORE" "$CONNECTIVITY" "$CONNECTIVITY_T" "$WIFI" \
    build/framework/system build/framework/classes.jar build/framework/stubs.jar build/art/java/all-classes.jar; do
  if [[ ! -e "$needed" ]]; then
    echo "$needed is missing: tools/fetch-sources.py --set framework and --set art, tools/build-art.sh and" \
      "tools/build-framework.sh make it." >&2
    exit 1
  fi
done

rm -rf "$OUT"
mkdir -p "$OUT/classes"
python3 tools/managergen/managergen.py --library-api "$LIBCORE" --library-api "$ICU" \
  --api "$CORE" --api "$LOCATION" --all-of "$CONNECTIVITY" --all-of "$CONNECTIVITY_T" --all-of "$WIFI" \
  --class android.media.AudioManager --class android.telephony.TelephonyManager \
  --class android.telephony.PhoneStateListener --class android.telephony.TelephonyCallback \
  --hand framework/managers --out "$OUT/gen" --references-out "$OUT/stubs"

# The boot jars' java.base exports only the JDK's packages; framework.jar's classes see the rest, as on Android.
"$JDK/bin/jar" tf build/art/java/all-classes.jar | sed -n 's|/[^/]*\.class$||p' | sort -u \
  | grep -v '^javax\{0,1\}/' | tr / . | sed 's|.*|--add-exports java.base/&=ALL-UNNAMED|' > "$OUT/exports"
find "$OUT/gen" "$OUT/stubs" framework/java/com/android/minato/services -name '*.java' | sort > "$OUT/sources"
"$JDK/bin/javac" -J-Xmx4g -source 17 -target 17 --system=build/framework/system @"$OUT/exports" \
  -XDstringConcat=inline -encoding UTF-8 -Xlint:all,-deprecation,-rawtypes,-unchecked,-serial -Werror \
  -Xmaxerrs 1000 -classpath build/framework/classes.jar:build/framework/stubs.jar -d "$OUT/classes" \
  @"$OUT/sources"

failed=0
for manager in android.media.AudioManager android.net.ConnectivityManager android.net.wifi.WifiManager \
    android.telephony.TelephonyManager; do
  if "$JDK/bin/javap" -p -cp "$OUT/classes" "$manager" | grep -qF "  public $manager(android.content.Context);"; then
    echo "ok    $manager has its hidden ctor (Context)"
  else
    echo "FAIL  $manager lacks the hidden ctor (Context) SystemServiceRegistry calls"
    failed=$((failed + 1))
  fi
done
generated=$(find "$OUT/gen" -name '*.java' | wc -l | tr -d ' ')
stubs=$(find "$OUT/stubs" -name '*.java' | wc -l | tr -d ' ')
methods=$(grep -rh 'Unwritten.log(' "$OUT/gen" | wc -l | tr -d ' ')
echo "managers check: $generated classes with $methods generated bodies, $stubs compile-only stubs"
[[ $failed -eq 0 ]] || { echo "managers check: $failed failed"; exit 1; }
echo "managers check: fine"
