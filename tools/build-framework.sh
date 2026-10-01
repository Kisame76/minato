#!/usr/bin/env bash
# Builds framework.jar (ADR-0039 decision 11) from framework/java into build/framework/framework.jar, which Minato
# ships as /system/framework/framework.jar on the boot class path:
#   - a system image of the boot jars' classes, as Soong makes its system modules: jmod create over
#     build/art/java/all-classes.jar, which holds java.base's module-info, then jlink --add-modules java.base;
#   - javac -source 17 -XDstringConcat=inline against that image;
#   - D8 as for the boot jars (runtime/art/boot_jars.py): --release --min-api 31 --android-platform-build, with the
#     boot jars' classes as its library;
#   - the jar: its DEX files stored and 4-byte aligned, as the boot jars' are.
# In step a it holds our ActivityThread and the interim Process and Application (framework/java); the tag's sources
# come with step b3/c.
# Needs the JDK and D8 of tools/fetch-sources.py --set art and the boot jars' classes of tools/build-art.sh.
# Usage: tools/build-framework.sh
set -euo pipefail
cd "$(dirname "$0")/.."

JDK=$(echo build/tools/jdk-17/*/Contents/Home)
R8=build/sources/r8/r8.jar
BOOT_CLASSES=build/art/java/all-classes.jar
OUT=build/framework
if [[ ! -x "$JDK/bin/javac" || ! -f "$R8" || ! -f "$BOOT_CLASSES" ]]; then
  echo "No JDK, D8 or $BOOT_CLASSES: run tools/fetch-sources.py --set art and tools/build-art.sh first." >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/jmod" "$OUT/classes" "$OUT/dex"
"$JDK/bin/jmod" create --module-version 17 --target-platform android --class-path "$BOOT_CLASSES" \
  "$OUT/jmod/java.base.jmod"
"$JDK/bin/jlink" --module-path "$OUT/jmod" --add-modules java.base --output "$OUT/system" \
  --disable-plugin system-modules
# javac reads a system image through its jrt file system.
cp "$JDK/lib/jrt-fs.jar" "$OUT/system/lib/"

# -XDstringConcat=inline as for the boot jars: their StringConcatFactory lacks makeConcatWithConstants.
"$JDK/bin/javac" -J-Xmx2g -source 17 -target 17 --system="$OUT/system" -XDstringConcat=inline -encoding UTF-8 -g \
  -d "$OUT/classes" \
  $(find framework/java -name '*.java' | sort)
"$JDK/bin/jar" --create --file "$OUT/classes.jar" -C "$OUT/classes" .
"$JDK/bin/java" -Xmx2g -cp "$R8" com.android.tools.r8.D8 --release --min-api 31 --android-platform-build \
  --lib "$BOOT_CLASSES" --output "$OUT/dex" "$OUT/classes.jar"

python3 - "$OUT/dex" "$OUT/framework.jar" <<'PY'
import os, struct, sys, zipfile
folder, path = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(path + ".tmp", "w") as jar:
    for name in sorted(n for n in os.listdir(folder) if n.endswith(".dex")):
        info = zipfile.ZipInfo(name, (2008, 1, 1, 0, 0, 0))
        info.external_attr = 0o644 << 16
        info.compress_type = zipfile.ZIP_STORED
        # zipalign's extra field (0xd935) pads the data to a 4-byte boundary.
        start = jar.fp.tell() + 30 + len(name.encode()) + 6
        padding = -start % 4
        info.extra = struct.pack("<HHH", 0xD935, 2 + padding, 4) + bytes(padding)
        jar.writestr(info, open(os.path.join(folder, name), "rb").read())
os.replace(path + ".tmp", path)
PY
echo "build-framework: $OUT/framework.jar ($(wc -c <"$OUT/framework.jar" | tr -d ' ') bytes)"
