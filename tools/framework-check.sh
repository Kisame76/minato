#!/usr/bin/env bash
# Checks framework.jar's bounded sources (ADR-0039 b3.1): tools/build-framework.sh compiles framework/sources.txt's
# AOSP files with framework/java and the generated sources against the boot jars and compile-only stubs, then:
#   - every root of sources.txt (its first group) is a class in framework.jar;
#   - our ActivityThread (framework/java) is the one there: its main throws, AOSP's has getApplicationThread;
#   - AssetManager is the one with Minato's patch (framework/patches, b3.3): its system path loads framework-res's
#     folder with loadFromDir and never calls OverlayConfig;
#   - no class is both in framework.jar and among the stubs (build/framework/stubs.jar): stubs never ship;
#   - no class stands on a stub supertype, which would compile but not load, beyond framework/stub-supertypes.txt's
#     entries, and each entry still does (tools/stub_supertypes.py, b3.4).
# Prints the counts of taken files, hand stubs and classes, and framework.jar's size.
# Usage: tools/framework-check.sh   (SKIP_BUILD=1 checks the build there is)
set -euo pipefail
cd "$(dirname "$0")/.."

JDK=$(echo build/tools/jdk-17/*/Contents/Home)
OUT=build/framework
[[ -n "${SKIP_BUILD:-}" ]] || tools/build-framework.sh

failed=0
fail() { echo "FAIL  $1"; failed=$((failed + 1)); }
ok() { echo "ok    $1"; }

"$JDK/bin/jar" tf "$OUT/classes.jar" | grep '\.class$' | sed 's|\.class$||' | sort > "$OUT/classes.txt"
# The roots: the paths before the first blank line after the first group's comment.
roots=$(awk '/^#/ { if (seen) exit; next } /^$/ { if (seen) exit; next } { seen = 1; print }' framework/sources.txt \
  | sed -n 's|.*/java/\(.*\)\.java$|\1|p')
missing=0
for root in $roots; do
  grep -qx "$root" "$OUT/classes.txt" || { fail "root $root is not in framework.jar"; missing=1; }
done
[[ $missing -eq 0 ]] && ok "the $(echo "$roots" | wc -w | tr -d ' ') roots are in framework.jar"

thread=$("$JDK/bin/javap" -cp "$OUT/classes.jar" android.app.ActivityThread)
if [[ "$thread" == *"void main(java.lang.String[]) throws"* && "$thread" != *"getApplicationThread"* ]]; then
  ok "android.app.ActivityThread is ours"
else
  fail "android.app.ActivityThread is not ours"
fi

system_path=$("$JDK/bin/javap" -c -p -cp "$OUT/classes.jar" android.content.res.AssetManager \
  | awk '/static void createSystemAssetsInZygoteLocked\(/ { on = 1 } on && /^$/ { exit } on')
if [[ "$system_path" == *"ApkAssets.loadFromDir"* && "$system_path" != *"OverlayConfig"* ]]; then
  ok "android.content.res.AssetManager is patched: its system path loads the framework-res folder"
else
  fail "android.content.res.AssetManager is not patched"
fi

[[ -f "$OUT/stubs.jar" ]] || { fail "no $OUT/stubs.jar"; exit 1; }
"$JDK/bin/jar" tf "$OUT/stubs.jar" | grep '\.class$' | sed 's|\.class$||' | sort > "$OUT/stub-classes.txt"
leaked=$(comm -12 "$OUT/classes.txt" "$OUT/stub-classes.txt")
if [[ -z "$leaked" ]]; then
  ok "no stub class in framework.jar ($(wc -l < "$OUT/stub-classes.txt" | tr -d ' ') stub classes)"
else
  fail "stub classes in framework.jar: $(echo "$leaked" | head -5 | tr '\n' ' ')"
fi

"$JDK/bin/jar" tf build/art/java/all-classes.jar | grep '\.class$' | sed 's|\.class$||' | sort > "$OUT/boot-classes.txt"
supertypes=$(sed 's|/|.|g' "$OUT/classes.txt" | xargs "$JDK/bin/javap" -cp "$OUT/classes.jar" \
  | python3 tools/stub_supertypes.py "$OUT/classes.txt" "$OUT/boot-classes.txt" framework/stub-supertypes.txt)
if [[ -z "$supertypes" ]]; then
  ok "no class stands on a stub supertype beyond framework/stub-supertypes.txt"
else
  fail "stub supertypes: $(echo "$supertypes" | tr '\n' ';')"
fi

taken=$(grep -cv '^#\|^$' framework/sources.txt)
members=$(find framework/stubs/members -name '*.java' | wc -l | tr -d ' ')
types=$(find framework/stubs/types -name '*.java' | wc -l | tr -d ' ')
echo "framework check: $taken taken files, hand stubs: $members with hidden members, $types hidden types," \
  "$(wc -l < "$OUT/classes.txt" | tr -d ' ') classes," \
  "framework.jar $(wc -c < "$OUT/framework.jar" | tr -d ' ') bytes"
[[ $failed -eq 0 ]] || { echo "framework check: $failed failed"; exit 1; }
echo "framework check: fine"
