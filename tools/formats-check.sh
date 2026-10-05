#!/usr/bin/env bash
# Checks Minato's choice from bundletool's .apks against bundletool itself (ADR-0049 a2, test 2). A test bundle is
# built at test time: a base with language, density and ABI splits, an on-demand feature module, an install-time
# module on a device feature, and an install-time, a fast-follow and an on-demand asset pack with ASTC and ETC2
# textures, each linked by our aapt2 against our framework-res and put together by bundletool's build-bundle. It goes
# through build-apks (default, --mode=universal, --local-testing), and extract-apks chooses from each for the device
# spec Minato chooses for: the install with the on-demand module asked for, as Minato installs it, and each pack's own
# on top. importer/APKKit's BundletoolRealOutputTests then opens each .apks as a container and must name the same
# APKs and packs, and protoc decodes the tests' own toc writer against bundletool's commands.proto.
# Skips with a message when bundletool (tools/fetch-bundletool.py), the JDK, aapt2, protoc or framework-res is
# missing; downloads nothing. Writes below build/formats-check/.
# Usage: tools/formats-check.sh
# Optional environment:
#   SWIFT_BUILD  the build folder for swift test, build/swiftpm-formats-check by default
set -euo pipefail
cd "$(dirname "$0")/.."

skip() {
  echo "formats check: skipped, $1"
  exit 0
}
JAR=$(/usr/bin/python3 tools/fetch-bundletool.py 2>/dev/null) || skip "bundletool differs from its pin or is missing (tools/fetch-bundletool.py)"
JAVA=$(find -L build/tools/jdk-17 -maxdepth 5 -path '*/bin/java' -type f 2>/dev/null | head -n 1)
[[ -n "$JAVA" ]] || skip "no JDK in build/tools/jdk-17"
AAPT2=build/aapt2/aapt2
PROTOC=build/aapt2/protoc
FRAMEWORK_RES=build/framework-res/framework-res.apk
[[ -x "$AAPT2" && -x "$PROTOC" ]] || skip "no $AAPT2 or $PROTOC (tools/build-aapt2.sh)"
[[ -f "$FRAMEWORK_RES" ]] || skip "no $FRAMEWORK_RES (tools/build-framework-res.sh)"

WORK=build/formats-check
rm -rf "${WORK:?}"
mkdir -p "$WORK/src" "$WORK/modules" "$WORK/out"
bundletool() { "$JAVA" -jar "$JAR" "$@"; }

# The modules' sources: manifests, resources, assets and libraries.
/usr/bin/python3 - "$WORK/src" <<'EOF'
import os
import struct
import sys
import zlib

root = sys.argv[1]
ANDROID = 'xmlns:android="http://schemas.android.com/apk/res/android"'
DIST = 'xmlns:dist="http://schemas.android.com/apk/distribution"'


def write(path, content):
    path = os.path.join(root, path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(content if isinstance(content, bytes) else content.encode())


def png(red):
    """A 1x1 PNG, so aapt2 compiles it as it compiles an app's."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes([0, red, 0, 0]))) + chunk(b"IEND", b""))


def module(name, body, pack=False):
    """A feature module has the base's versionCode; bundletool refuses one in an asset pack."""
    version = "" if pack else ' android:versionCode="1"'
    write(f"{name}/AndroidManifest.xml",
          f'<manifest {ANDROID} {DIST} package="org.example.formats" split="{name}"{version}>\n{body}\n</manifest>\n')


write("base/AndroidManifest.xml", f"""<manifest {ANDROID} {DIST} package="org.example.formats"
    android:versionCode="1" android:versionName="1.0">
  <uses-sdk android:minSdkVersion="21" android:targetSdkVersion="35"/>
  <application android:label="@string/app_name" android:icon="@drawable/icon" android:hasCode="false"/>
</manifest>
""")
write("base/res/values/strings.xml", '<resources><string name="app_name">Formats</string>'
      '<string name="camera">Camera</string><string name="ar">AR</string></resources>\n')
write("base/res/values-de/strings.xml", '<resources><string name="app_name">Formate</string></resources>\n')
write("base/res/values-fr/strings.xml", '<resources><string name="app_name">Formats FR</string></resources>\n')
write("base/res/drawable-xhdpi/icon.png", png(1))
write("base/res/drawable-xxhdpi/icon.png", png(2))
write("base/res/drawable-xxxhdpi/icon.png", png(3))
for abi in ("arm64-v8a", "armeabi-v7a", "x86_64"):
    write(f"base/lib/{abi}/libformats.so", f"not an ELF file, for {abi}\n")
module("camera", '  <dist:module dist:title="@string/camera"><dist:delivery><dist:on-demand/></dist:delivery>'
       '<dist:fusing dist:include="true"/></dist:module>\n  <application android:hasCode="false"/>')
module("ar", '  <dist:module dist:title="@string/ar"><dist:delivery><dist:install-time><dist:conditions>'
       '<dist:device-feature dist:name="android.hardware.camera.ar"/></dist:conditions></dist:install-time>'
       '</dist:delivery><dist:fusing dist:include="false"/></dist:module>\n  <application android:hasCode="false"/>')
for pack, delivery in (("start", "install-time"), ("level2", "fast-follow"), ("level3", "on-demand")):
    module(pack, f'  <dist:module dist:type="asset-pack"><dist:fusing dist:include="true"/>'
           f'<dist:delivery><dist:{delivery}/></dist:delivery></dist:module>', pack=True)
    for format in ("astc", "etc2"):
        write(f"{pack}/assets/textures#tcf_{format}/{pack}.ktx", f"{pack} in {format}\n")
    write(f"{pack}/assets/{pack}.txt", f"{pack}\n")
EOF

# Each module linked in protobuf's format, as build-bundle takes it: its manifest under manifest/, resources.pb and
# res/ from aapt2, then its assets and libraries. Features link against the base for its strings.
"$AAPT2" compile --dir "$WORK/src/base/res" -o "$WORK/base-res.zip"
"$AAPT2" link -o "$WORK/base-binary.apk" -I "$FRAMEWORK_RES" --manifest "$WORK/src/base/AndroidManifest.xml" \
  "$WORK/base-res.zip"
for name in base camera ar start level2 level3; do
  inputs=()
  [[ "$name" == base ]] && inputs=("$WORK/base-res.zip")
  includes=(-I "$FRAMEWORK_RES")
  [[ "$name" == base ]] || includes+=(-I "$WORK/base-binary.apk")
  "$AAPT2" link --proto-format -o "$WORK/$name-linked.apk" "${includes[@]}" \
    --manifest "$WORK/src/$name/AndroidManifest.xml" ${inputs[@]+"${inputs[@]}"}
  /usr/bin/python3 - "$WORK/$name-linked.apk" "$WORK/src/$name" "$WORK/modules/$name.zip" <<'EOF'
import os
import sys
import zipfile

linked, source, module = sys.argv[1:]
# bundletool refuses a resource table in an asset pack, which aapt2 writes anyway.
pack = os.path.basename(module) in ("start.zip", "level2.zip", "level3.zip")
with zipfile.ZipFile(linked) as apk, zipfile.ZipFile(module, "w", zipfile.ZIP_DEFLATED) as out:
    for entry in apk.infolist():
        if pack and entry.filename == "resources.pb":
            continue
        name = "manifest/AndroidManifest.xml" if entry.filename == "AndroidManifest.xml" else entry.filename
        out.writestr(name, apk.read(entry))
    for folder in ("assets", "lib"):
        for directory, _, files in sorted(os.walk(os.path.join(source, folder))):
            for file in sorted(files):
                path = os.path.join(directory, file)
                out.write(path, os.path.relpath(path, source))
EOF
done

cat >"$WORK/config.json" <<'EOF'
{"optimizations": {"splitsConfig": {"splitDimension": [
  {"value": "ABI"}, {"value": "SCREEN_DENSITY"}, {"value": "LANGUAGE"},
  {"value": "TEXTURE_COMPRESSION_FORMAT", "suffixStripping": {"enabled": true, "defaultSuffix": "etc2"}}]}}}
EOF
modules=$(printf '%s,' "$WORK"/modules/{base,camera,ar,start,level2,level3}.zip)
bundletool build-bundle --modules="${modules%,}" --config="$WORK/config.json" --output="$WORK/app.aab" >/dev/null
for mode in default universal local; do
  case "$mode" in
    universal) extra=(--mode=universal) ;;
    local) extra=(--local-testing) ;;
    *) extra=() ;;
  esac
  bundletool build-apks --bundle="$WORK/app.aab" --output="$WORK/$mode.apks" --aapt2="$AAPT2" \
    ${extra[@]+"${extra[@]}"} >/dev/null 2>"$WORK/$mode-build.log"
done

# The device spec Minato chooses for (BundletoolRealOutputTests.device).
cat >"$WORK/spec.json" <<'EOF'
{"supportedAbis": ["arm64-v8a"], "supportedLocales": ["en", "de", "fr"], "screenDensity": 480, "sdkVersion": 35,
 "glExtensions": ["GL_KHR_texture_compression_astc_ldr", "GL_OES_compressed_ETC1_RGB8_texture"],
 "deviceFeatures": ["reqGlEsVersion=0x30000"], "sdkRuntime": {"supported": false}}
EOF
# extract-apks' base names for the .apks $1 with the modules $2, sorted.
extracted() {
  local folder="$WORK/out/$(basename "$1" .apks)-${2//,/-}"
  local modules=()
  [[ "$1" == universal ]] || modules=(--modules="$2")
  bundletool extract-apks --apks="$WORK/$1.apks" --device-spec="$WORK/spec.json" --output-dir="$folder" \
    ${modules[@]+"${modules[@]}"} >/dev/null
  (cd "$folder" && ls | LC_ALL=C sort)
}
/usr/bin/python3 - "$WORK" <<EOF
import json
import sys

def names(text):
    return [line for line in text.split("\n") if line]

work = sys.argv[1]
install = {mode: names(text) for mode, text in (("default", """$(extracted default camera)"""),
                                                ("local", """$(extracted local camera)"""),
                                                ("universal", """$(extracted universal camera)"""))}
# Each pack's slices: what asking for it adds to the install. A universal toc holds no pack.
packs = {mode: {pack: sorted(set(names(text)) - set(install[mode])) for pack, text in levels}
         for mode, levels in (("default", (("level2", """$(extracted default camera,level2)"""),
                                           ("level3", """$(extracted default camera,level3)"""))),
                              ("local", (("level2", """$(extracted local camera,level2)"""),
                                         ("level3", """$(extracted local camera,level3)"""))))}
cases = [
    {"apks": "default.apks", "install": install["default"], "packs": packs["default"]},
    {"apks": "universal.apks", "install": install["universal"], "packs": {}},
    {"apks": "local.apks", "install": install["local"], "packs": packs["local"]},
]
with open(f"{work}/cases.json", "w") as handle:
    json.dump(cases, handle, indent=2)
for case in cases:
    print(f"extract-apks: {case['apks']}: {', '.join(case['install'])}; packs {case['packs']}")
EOF

FORMATS_CHECK_DIR="$PWD/$WORK" swift test --package-path importer/APKKit \
  --build-path "${SWIFT_BUILD:-build/swiftpm-formats-check}" --filter BundletoolRealOutputTests >"$WORK/swift-test.log" 2>&1 \
  || { grep -E "✘|error:" "$WORK/swift-test.log" | head -40; echo "FAIL  Minato's choice differs from extract-apks'"; exit 1; }
echo "ok    Minato chooses what extract-apks chooses from bundletool's default, universal and local-testing .apks"

# protoc decodes the tests' own toc writer as protobuf does.
PROTOS="$WORK/proto"
mkdir -p "$PROTOS"
unzip -q -o "$JAR" '*.proto' -x 'google/*' -d "$PROTOS"
# protobuf's own well-known types, from the sources protoc is built from.
WELL_KNOWN=build/sources/protobuf/src
[[ -f "$WELL_KNOWN/google/protobuf/wrappers.proto" ]] || { echo "FAIL  no $WELL_KNOWN/google/protobuf/wrappers.proto"; exit 1; }
"$PROTOC" --proto_path="$PROTOS" --proto_path="$WELL_KNOWN" \
  --decode=android.bundle.BuildApksResult commands.proto <"$WORK/sample-toc.pb" >"$WORK/sample-toc.txt"
for expected in 'package_name: "org.example.sets"' 'variant_number: 2' 'split_id: "config.arm64_v8a"' 'density_dpi: 560' \
    'requires_sdk_runtime: true' 'name: "android.hardware.opengles.version"' 'default_value: "latam"'; do
  grep -q -F "$expected" "$WORK/sample-toc.txt" || { echo "FAIL  protoc's decoding lacks: $expected"; exit 1; }
done
echo "ok    protoc decodes the tests' toc writer as written"
echo "formats check: passed"
