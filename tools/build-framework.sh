#!/usr/bin/env bash
# Builds framework.jar (ADR-0039 decision 11) into build/framework/framework.jar, which Minato ships as
# /system/framework/framework.jar on the boot class path, from bounded sources (b3.1):
#   - a system image of the boot jars' classes, as Soong makes its system modules: jmod create over
#     build/art/java/all-classes.jar, which holds java.base's module-info, then jlink --add-modules java.base;
#   - the generated sources in build/framework/gen: aconfig's Java classes with ap3a's values
#     (tools/aconfig-flags.py java), android.R, com.android.internal.R and android.Manifest from framework-res's link
#     (tools/build-framework-res.sh), aidl's Java for the .aidl files framework/sources.txt names,
#     ApplicationPackageManager from tools/managergen --implement with framework/managers' hand file (b3.5a), our
#     AppOpsManager and StorageManager from managergen's API mode, the public API with the hand files' members (step
#     c2c1, ADR-0045's storage and app ops step), AppOpsTable, AOSP's app op table (tools/appopsgen.py, that step),
#     ManifestFeatureFlags, the feature flags' values framework-res was built with, for ManifestPackage's
#     AconfigFlags.skipCurrentElement (b3.5b1), CompatTable, the compat changes of framework.jar's and the boot jars'
#     sources with the target SDK from which each is enabled (tools/compatgen.py, step c2e), and the EventLogTags
#     classes of the .logtags files framework/sources.txt names (tools/event-log-tags.py, step c2c3), and GapScan,
#     the gap scanner of fixtures/java/gapscan-test in a package of ours, com.android.internal.minato (ActivityThread's
#     MINATO_GAPSCAN);
#   - compile-only stubs, compiled on their own into build/framework/stubs.jar: managergen's (--stubs) for the public
#     API of core, location and the Connectivity and Wi-Fi modules, with the hidden members of
#     framework/stubs/members/ (managergen's hand files: what taken AOSP files call on a public type); the
#     annotations of modules-utils and ravenwood; and the hidden types of framework/stubs/types/, which win over the
#     rest. stubs.jar leaves out every type framework.jar defines;
#   - Minato's patches to taken files (framework/patches), applied to copies in build/framework/src;
#   - javac -source 17 -XDstringConcat=inline of framework/java, the AOSP files of framework/sources.txt and the
#     generated sources, as a patch of java.base as Soong compiles the framework, with -implicit:none so that only
#     those become classes: the stubs come in through the same patch path, which is where a split package's types
#     must be, and never ship. AOSP's files compile with Soong's flags, without lints; ours get a second pass with
#     every lint as an error;
#   - D8 as for the boot jars (runtime/art/boot_jars.py): --release --min-api 31 --android-platform-build, with the
#     boot jars' classes as its library;
#   - the jar: its DEX files stored and 4-byte aligned, as the boot jars' are.
# tools/framework-check.sh checks the result. Needs the JDK and D8 of tools/fetch-sources.py --set art, the boot jars'
# classes and boot_jars.json of tools/build-art.sh, the sources of --set framework, build/aidl/aidl
# (tools/build-aidl.sh) and framework-res with its R classes (tools/build-framework-res.sh).
# Usage: tools/build-framework.sh
set -euo pipefail
cd "$(dirname "$0")/.."

JDK=$PWD/$(echo build/tools/jdk-17/*/Contents/Home)
R8=build/sources/r8/r8.jar
BOOT_CLASSES=build/art/java/all-classes.jar
SOURCES=build/sources
AIDL=build/aidl/aidl
R_JAVA=build/framework-res/java
FEATURE_FLAGS=build/framework-res/feature-flags.txt
OUT=build/framework
GEN=$OUT/gen
for needed in "$JDK/bin/javac" "$R8" "$BOOT_CLASSES" build/art/boot_jars.json "$AIDL" "$R_JAVA/android/R.java" \
    "$FEATURE_FLAGS" "$SOURCES/modules_utils/java" "$SOURCES/frameworks_base_api/core/api/current.txt"; do
  if [[ ! -e "$needed" ]]; then
    echo "$needed is missing: tools/fetch-sources.py --set art and --set framework, tools/build-art.sh," \
      "tools/build-aidl.sh and tools/build-framework-res.sh make it." >&2
    exit 1
  fi
done

rm -rf "$OUT"
mkdir -p "$OUT/jmod" "$OUT/classes" "$OUT/dex" "$OUT/src" "$GEN/aidl" "$GEN/r" "$GEN/logtags" "$OUT/stubs-src" \
  "$OUT/stub-classes"
"$JDK/bin/jmod" create --module-version 17 --target-platform android --class-path "$BOOT_CLASSES" \
  "$OUT/jmod/java.base.jmod"
"$JDK/bin/jlink" --module-path "$OUT/jmod" --add-modules java.base --output "$OUT/system" \
  --disable-plugin system-modules
# javac reads a system image through its jrt file system.
cp "$JDK/lib/jrt-fs.jar" "$OUT/system/lib/"

# The taken files, linked into their packages' folders; .aidl files go through aidl, .logtags files through
# tools/event-log-tags.py. A taken file that would replace one of ours is an error: framework/java wins by being left
# out of AOSP's list.
taken=()
while IFS= read -r line; do taken+=("$line"); done < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' framework/sources.txt)
for path in "${taken[@]}"; do
  source="$SOURCES/$path"
  [[ -f "$source" ]] || { echo "framework/sources.txt: $path is not in $SOURCES" >&2; exit 1; }
  package=$(sed -n 's/^package \([a-zA-Z0-9_.]*\);.*/\1/p' "$source" | head -1)
  name=$(basename "$path")
  if [[ "$name" == *.aidl ]]; then
    "$AIDL" --lang=java -I"$SOURCES/frameworks_base/core/java" -I"$SOURCES/frameworks_base_graphics/graphics/java" \
      -I"$SOURCES/frameworks_base_types/apex/jobscheduler/framework/java" -Iframework/aidl-imports -o "$GEN/aidl" "$source"
    continue
  fi
  if [[ "$name" == *.logtags ]]; then
    python3 tools/event-log-tags.py "$source" --out "$GEN/logtags"
    continue
  fi
  if [[ -f "framework/java/${package//.//}/$name" ]]; then
    echo "framework/sources.txt: $path would replace framework/java's ${package//.//}/$name" >&2
    exit 1
  fi
  mkdir -p "$OUT/src/${package//.//}"
  ln -s "$PWD/$source" "$OUT/src/${package//.//}/$name"
done
# Minato's patches to taken files of frameworks/base (framework/patches), each applied to a copy in place of the
# file's link, so build/sources keeps the fetched files and their digests.
for patch in framework/patches/*.patch; do
  target=$(sed -n 's|^+++ b/||p' "$patch")
  copy="$OUT/src/${target#*/java/}"
  original="$SOURCES/frameworks_base/$target"
  [[ "$target" == graphics/* ]] && original="$SOURCES/frameworks_base_graphics/$target"
  [[ "$target" == media/* ]] && original="$SOURCES/frameworks_base_types/$target"
  [[ "$target" == rs/* ]] && original="$SOURCES/frameworks_base_types/$target"
  [[ "$target" == telephony/* ]] && original="$SOURCES/frameworks_base_types/$target"
  [[ -L "$copy" ]] || { echo "$patch: $target is not a taken file" >&2; exit 1; }
  rm "$copy"
  patch --quiet -o "$copy" "$original" "$patch"
done
python3 tools/aconfig-flags.py java --declarations "$SOURCES/frameworks_base" \
  --declarations "$SOURCES/frameworks_base_graphics" --declarations "$SOURCES/frameworks_base_hwui" \
  --declarations "$SOURCES/frameworks_base_types" --values "$SOURCES/build_release/aconfig/ap3a" \
  --out "$GEN/aconfig" >/dev/null
cp -R "$R_JAVA/." "$GEN/r/"
# The values of every feature flag framework-res was built with (ap3a's), which Android's parser reads from the
# device's aconfig storage to leave out a manifest element behind a flag that is off (b3.5b1).
mkdir -p "$GEN/aconfig/android/app"
python3 - "$FEATURE_FLAGS" "$GEN/aconfig/android/app/ManifestFeatureFlags.java" <<'PY'
import sys
flags = {}
with open(sys.argv[1], encoding="utf-8") as handle:
    for line in handle.read().splitlines():
        if "=" in line:
            name, value = line.strip().split("=", 1)
            flags[name] = value == "true"
names = sorted(flags)
with open(sys.argv[2], "w", encoding="utf-8") as out:
    out.write("package android.app;\n\n")
    out.write("/** Generated by tools/build-framework.sh from build/framework-res/feature-flags.txt. */\n")
    out.write("final class ManifestFeatureFlags {\n")
    out.write("    /** The flags' names, sorted for Arrays.binarySearch. */\n")
    out.write("    static final String[] NAMES = {\n")
    out.writelines(f'        "{name}",\n' for name in names)
    out.write("    };\n")
    out.write("    /** Each name's value, at its index. */\n")
    out.write("    static final boolean[] VALUES = {\n")
    out.writelines(f"        {'true' if flags[name] else 'false'},\n" for name in names)
    out.write("    };\n\n")
    out.write("    private ManifestFeatureFlags() {\n    }\n}\n")
PY
# Our ApplicationPackageManager (b3.5a): PackageManager's abstract methods, logged once where framework/managers'
# hand file does not answer.
python3 tools/managergen/managergen.py \
  --implement "$SOURCES/frameworks_base/core/java/android/content/pm/PackageManager.java" \
  --as android.app.ApplicationPackageManager --hand framework/managers --out "$GEN/managers" >/dev/null
# Our ContextImpl (step c1): Context's abstract methods, logged once where framework/managers' hand file does not
# answer.
python3 tools/managergen/managergen.py \
  --implement "$SOURCES/frameworks_base/core/java/android/content/Context.java" \
  --as android.app.ContextImpl --hand framework/managers --out "$GEN/managers" >/dev/null
# Our managers from the public API (steps c2c1, c2c2, c2c3 and ADR-0045's storage and app ops step; gap 1's audio and
# connectivity with the Connectivity module's network classes; the Unity test APK probe's TrafficStats and gap 13's
# AudioTrack and AudioDeviceInfo; android.media's absent hardware: DRM, codec lists, retriever, extractor, image
# reader and the TV input manager), logged once where
# framework/managers' hand files do not answer.
python3 tools/managergen/managergen.py --library-api "$SOURCES/libcore/api/current.txt" \
  --api "$SOURCES/frameworks_base_api/core/api/current.txt" --class android.app.AppOpsManager \
  --class android.os.storage.StorageManager \
  --class android.view.accessibility.AccessibilityManager \
  --class android.app.ActivityManager --class android.view.autofill.AutofillManager \
  --class android.view.inputmethod.InputMethodManager --class android.view.textservice.TextServicesManager \
  --class android.view.textclassifier.TextClassificationManager --class android.media.AudioManager \
  --class android.media.AudioTrack --class android.media.AudioDeviceInfo \
  --class android.media.MediaDrm --class android.media.MediaCrypto \
  --class android.media.MediaMetadataRetriever --class android.media.MediaExtractor \
  --class android.media.tv.TvInputManager \
  --class android.media.VolumeShaper --class android.media.MediaDescrambler \
  --class android.media.MediaCas \
  --class android.telephony.TelephonyManager --class android.telephony.PhoneStateListener \
  --class android.telephony.TelephonyCallback --class android.os.Vibrator \
  --class android.app.ActivityOptions --class android.app.BroadcastOptions --class android.app.SearchManager \
  --class android.media.session.MediaController --class android.widget.Toolbar --class android.app.VoiceInteractor \
  --class android.window.SplashScreen \
  --class android.app.LocaleManager --class android.os.storage.OnObbStateChangeListener \
  --class android.net.http.SslCertificate --class android.webkit.WebStorage \
  --class android.webkit.GeolocationPermissions \
  --class android.net.TransportInfo --class android.net.IpPrefix --class android.net.RouteInfo \
  --class android.net.DhcpInfo --class android.net.http.SslError --class android.telephony.BarringInfo \
  --class android.telephony.ims.ImsReasonInfo \
  --class android.telephony.PreciseDataConnectionState --class android.telephony.TelephonyDisplayInfo \
  --class android.media.session.MediaSession --class android.hardware.lights.LightsManager \
  --class android.media.session.MediaSessionManager \
  --class android.os.VibratorManager --class android.net.wifi.WifiInfo --class android.net.wifi.WifiConfiguration \
  --class android.net.wifi.hotspot2.PasspointConfiguration --class android.net.wifi.SoftApConfiguration \
  --class android.net.wifi.WifiNetworkSpecifier --class android.net.wifi.WifiNetworkSuggestion \
  --class android.net.wifi.WifiSsid --class android.net.wifi.WpsInfo \
  --class android.credentials.GetCredentialRequest \
  --class android.credentials.GetCredentialResponse \
  --class android.media.AudioDeviceCallback \
  --class android.media.AudioMixerAttributes \
  --class android.media.Spatializer \
  --class android.net.IpSecManager \
  --class android.net.ProxyInfo \
  --class android.net.SocketKeepalive \
  --class android.os.VibrationAttributes \
  --class android.service.notification.ZenPolicy \
  --class android.telecom.PhoneAccountHandle \
  --class android.telephony.IccOpenLogicalChannelResponse \
  --class android.telephony.NetworkScan \
  --class android.telephony.NetworkScanRequest \
  --class android.telephony.ServiceState \
  --class android.telephony.SignalStrength \
  --class android.telephony.SignalStrengthUpdateRequest \
  --class android.telephony.TelephonyScanManager \
  --class android.telephony.VisualVoicemailSmsFilterSettings \
  --class android.view.inputmethod.InputMethodInfo \
  --class android.view.inputmethod.InputMethodSubtype \
  --class android.view.translation.TranslationCapability \
  --class android.media.RemoteControlClient \
  --class android.media.RemoteController \
  --class android.service.notification.ZenDeviceEffects \
  --class android.telephony.CellLocation \
  --class android.view.inputmethod.ConnectionlessHandwritingCallback \
  --class android.app.AutomaticZenRule \
  --class android.service.notification.Condition \
  --class android.view.accessibility.AccessibilityNodeInfo \
  --class android.view.textclassifier.TextClassification \
  --class android.view.textclassifier.TextClassifier \
  --class android.view.textclassifier.TextLinks \
  --class android.view.translation.ViewTranslationRequest \
  --placeholder android.view.SurfaceControl \
  --placeholder android.view.WindowId \
  --placeholder android.window.InputTransferToken \
  --placeholder android.window.TrustedPresentationThresholds \
  --placeholder android.os.SharedMemory \
  --placeholder android.os.MemoryFile \
  --placeholder android.view.displayhash.DisplayHashResultCallback \
  --class android.hardware.camera2.CameraManager \
  --class android.view.contentcapture.ContentCaptureSession \
  --api "$SOURCES/wifi_api/framework/api/current.txt" --class android.net.wifi.WifiManager \
  --api "$SOURCES/connectivity_api/framework/api/current.txt" --class android.net.ConnectivityManager \
  --class android.net.Network --class android.net.NetworkCapabilities --class android.net.NetworkInfo \
  --class android.net.NetworkRequest --class android.net.LinkProperties \
  --api "$SOURCES/connectivity_t_api/framework-t/api/current.txt" --class android.net.TrafficStats \
  --hand framework/managers --out "$GEN/managers" >/dev/null
# The compat changes' table (step c2e): each @ChangeId field of framework.jar's and the boot jars' sources with the
# target SDK from which its change is enabled, from which ActivityThread.main gives the app its disabled changes.
python3 tools/compatgen.py --sources "$SOURCES" --list framework/sources.txt --java framework/java \
  --boot-jars build/art/boot_jars.json --build "$SOURCES/frameworks_base/core/java/android/os/Build.java" \
  --version-codes "$SOURCES/libcore/dalvik/src/main/java/dalvik/annotation/compat/VersionCodes.java" \
  --out "$GEN/compat" >/dev/null
# The gap scanner (a developer diagnostic: ActivityThread scans the app's own base.apk with MINATO_GAPSCAN=1):
# gapscan-test's GapScan, the one source, in a package of ours, so that it never meets the test app's own copy.
GAPSCAN=fixtures/java/gapscan-test/io/github/kisame76/minato/test/gapscan/GapScan.java
if ! grep -qx 'package io.github.kisame76.minato.test.gapscan;' "$GAPSCAN"; then
  echo "$GAPSCAN: no longer in the package io.github.kisame76.minato.test.gapscan" >&2
  exit 1
fi
mkdir -p "$GEN/gapscan/com/android/internal/minato"
sed 's/^package io\.github\.kisame76\.minato\.test\.gapscan;$/package com.android.internal.minato;/' "$GAPSCAN" \
  > "$GEN/gapscan/com/android/internal/minato/GapScan.java"
# The app op table (ADR-0045's storage and app ops step): AOSP's ops by number with their permissions and default
# modes, from which our AppOpsManager answers its checks and notes.
python3 tools/appopsgen.py --source "$SOURCES/frameworks_base/core/java/android/app/AppOpsManager.java" \
  --hand framework/managers/android/app/AppOpsManager.java --out "$GEN/appops" >/dev/null

# The stubs: the public API with our hidden members, then the annotations, then our hidden types, each replacing the
# one before.
python3 tools/managergen/managergen.py --stubs --hand framework/stubs/members \
  --library-api "$SOURCES/libcore/api/current.txt" --library-api "$SOURCES/icu/android_icu4j/api/public/current.txt" \
  --all-of "$SOURCES/frameworks_base_api/core/api/current.txt" \
  --all-of "$SOURCES/location_api/location/api/current.txt" \
  --all-of "$SOURCES/connectivity_api/framework/api/current.txt" \
  --all-of "$SOURCES/connectivity_t_api/framework-t/api/current.txt" \
  --all-of "$SOURCES/wifi_api/framework/api/current.txt" --out "$OUT/stubs-src" >/dev/null
for annotations in modules_utils/java/android/annotation modules_utils/java/com/android/internal/annotations \
    frameworks_base_graphics/ravenwood/annotations-src/android/ravenwood/annotation; do
  target="$OUT/stubs-src/${annotations#*/java/}"
  [[ "$annotations" == *ravenwood* ]] && target="$OUT/stubs-src/android/ravenwood/annotation"
  mkdir -p "$target"
  cp "$SOURCES/$annotations/"*.java "$target/"
done
(cd framework/stubs/types && find . -name '*.java') | while IFS= read -r stub; do
  mkdir -p "$OUT/stubs-src/$(dirname "$stub")"
  cp "framework/stubs/types/$stub" "$OUT/stubs-src/$stub"
done
# framework.jar's sources and the top-level types they define: no stub stands in for those.
{ find framework/java "$GEN" -name '*.java'; find -L "$OUT/src" -name '*.java'; } | sort > "$OUT/sources.txt"
sed -e "s|^framework/java/||" -e "s|^$GEN/[a-z]*/||" -e "s|^$OUT/src/||" -e 's|\.java$||' "$OUT/sources.txt" \
  | sort > "$OUT/defined.txt"
while IFS= read -r type; do
  rm -f "$OUT/stubs-src/$type.java"
done < "$OUT/defined.txt"
find "$OUT/stubs-src" -name '*.java' | sort > "$OUT/stub-sources.txt"
# The generated sources' folders, each a root of java.base's patch.
GENERATED="$GEN/aconfig:$GEN/aidl:$GEN/logtags:$GEN/r:$GEN/managers:$GEN/compat:$GEN/gapscan"
# -XDstringConcat=inline as for the boot jars: their StringConcatFactory lacks makeConcatWithConstants.
JAVAC=("$JDK/bin/javac" -J-Xmx4g -source 17 -target 17 --system="$OUT/system" -XDstringConcat=inline
  -XDskipDuplicateBridges=true -encoding UTF-8 -g -Xmaxerrs 9999 -proc:none)
# The stubs name framework.jar's types too (and their bodies Unwritten): those sources stand behind them on the patch
# path, and only the stubs themselves become classes.
"${JAVAC[@]}" -nowarn \
  --patch-module java.base="$OUT/stubs-src:framework/java:$OUT/src:$GENERATED" \
  -implicit:none -d "$OUT/stub-classes" \
  @"$OUT/stub-sources.txt"

# stubs.jar leaves out framework.jar's types, should a stub have made one.
python3 - "$OUT/stub-classes" "$OUT/defined.txt" > "$OUT/stub-list.txt" <<'PY'
import os, sys
classes, defined = sys.argv[1], set(open(sys.argv[2]).read().split())
for directory, _, names in sorted(os.walk(classes)):
    for name in sorted(names):
        path = os.path.relpath(os.path.join(directory, name), classes)
        if name.endswith(".class") and path[:-len(".class")].split("$")[0] not in defined:
            print(path)
PY
(cd "$OUT/stub-classes" && "$JDK/bin/jar" --create --file ../stubs.jar @../stub-list.txt)

PATCH="framework/java:$OUT/src:$GENERATED:$OUT/stubs.jar"
"${JAVAC[@]}" -nowarn --patch-module java.base="$PATCH" -implicit:none -d "$OUT/classes" @"$OUT/sources.txt"
# Ours with every lint, against what was just compiled.
mkdir -p "$OUT/lint"
"${JAVAC[@]}" -Xlint:all -Werror --patch-module java.base="framework/java:$OUT/classes:$OUT/stubs.jar" -implicit:none \
  -d "$OUT/lint" $(find framework/java -name '*.java' | sort)

"$JDK/bin/jar" --create --file "$OUT/classes.jar" -C "$OUT/classes" .
"$JDK/bin/java" -Xmx2g -cp "$R8" com.android.tools.r8.D8 --release --min-api 31 --android-platform-build \
  --lib "$BOOT_CLASSES" --output "$OUT/dex" "$OUT/classes.jar"

python3 - "$OUT/dex" "$OUT/framework.jar" <<'PY'
import os, re, struct, sys, zipfile
folder, path = sys.argv[1], sys.argv[2]


def minimized(source):
    """Android's debian.mime.types.minimized genrule (external/mime-support's Android.bp): comments stripped, white space
    normalized, lines without an extension dropped. DefaultMimeMapFactory reads only this format."""
    lines = (" ".join(re.sub(r"#.*$", "", line).split()) for line in open(source, encoding="utf-8"))
    return "".join(line + "\n" for line in lines if " " in line)


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
    # DefaultMimeMapFactory reads its tables as /res/<name> from the boot class path, as Android's mimemap-res.jar has
    # them (third_party/sources.json's mime_map and mime_support).
    for name, source in (("debian.mime.types", "build/sources/mime_support/mime.types"),
                         ("android.mime.types", "build/sources/mime_map/mime/java-res/android.mime.types"),
                         ("vendor.mime.types", "build/sources/mime_map/mime/java-res/vendor.mime.types")):
        info = zipfile.ZipInfo("res/" + name, (2008, 1, 1, 0, 0, 0))
        info.external_attr = 0o644 << 16
        info.compress_type = zipfile.ZIP_STORED
        jar.writestr(info, minimized(source))
os.replace(path + ".tmp", path)
PY
echo "build-framework: $OUT/framework.jar ($(wc -c <"$OUT/framework.jar" | tr -d ' ') bytes)"
