#!/usr/bin/env bash
# Builds framework-res (ADR-0039 b2, decision 8) from the tag's core/res with the host aapt2 of tools/build-aapt2.sh,
# as core/res/Android.bp's framework-res does:
#   - the remote views' color table: aapt2 compiles remote_color_resources_res's public.xml and colors.xml, links
#     them with RemoteThemeColorsAndroidManifest.xml, and its resources.arsc becomes res/raw/
#     remote_views_color_resources.arsc, with remote_color_resources_res/symbols.xml as res/values/symbols.xml
#     (the genrules remote-color-resources-*);
#   - core/res/res and those two compiled with --pseudo-localize, which Soong sets for every app it builds
#     (java/aar.go: "it will be stripped out later for release builds that don't want it"; the AVD's has en-XA and
#     ar-XB), linked with core/res/AndroidManifest.xml as Soong's manifest_fixer.py leaves it and core/res/assets,
#     with framework-res's aaptflags (--private-symbols com.android.internal, --no-auto-version, --auto-add-overlay,
#     --enable-sparse-encoding), --enable-compact-entries, which Soong adds to every app (java/aar.go), the product's
#     locales, the version Soong gives the platform (minSdk and targetSdk 35, versionCode 35, versionName 15, as the
#     AVD's framework-res.apk has them), and the feature flags of tools/aconfig-flags.py with ap3a's values. The
#     compiled files go to aapt2 link in the order of their names, which makes resources.arsc the AVD's, byte for
#     byte (tools/framework-res-check.py).
# Results: build/framework-res/framework-res.apk, and build/framework-res/framework-res/, the folder Minato ships
# as /system/framework/framework-res/ (ApkAssets.loadFromDir): the APK's entries unpacked.
# Usage: tools/build-framework-res.sh
set -euo pipefail
cd "$(dirname "$0")/.."

AAPT2=build/aapt2/aapt2
RES=build/sources/frameworks_base/core/res
OUT=build/framework-res
if [[ ! -x "$AAPT2" ]]; then
  echo "No $AAPT2: run tools/build-aapt2.sh first." >&2
  exit 1
fi
if [[ ! -d "$RES" || ! -d build/sources/build_release/aconfig/ap3a ]]; then
  echo "No $RES or ap3a's flag values: run tools/fetch-sources.py --set framework first." >&2
  exit 1
fi

# The product's locales, which Soong hands aapt2 link as -c (java/app.go, ProductAAPTConfig): PRODUCT_LOCALES of
# build/make's target/product/languages_default.mk at the tag, which the AVD's sdk_phone64 takes, with the
# pseudolocales en_XA and ar_XB; its sr_Latn_RS in aapt2's form, b+sr+Latn+RS. Others, such as en_XC
# (languages_full.mk) or de_AT, are left out, as on the AVD.
LOCALES=(af_ZA am_ET ar_EG ar_XB as_IN az_AZ be_BY bg_BG bn_BD bs_BA ca_ES cs_CZ da_DK de_DE el_GR en_AU en_CA en_GB
  en_IN en_US en_XA es_ES es_US et_EE eu_ES fa_IR fi_FI fr_CA fr_FR gl_ES gu_IN hi_IN hr_HR hu_HU hy_AM in_ID is_IS
  it_IT iw_IL ja_JP ka_GE kk_KZ km_KH kn_IN ko_KR ky_KG lo_LA lt_LT lv_LV mk_MK ml_IN mn_MN mr_IN ms_MY my_MM nb_NO
  ne_NP nl_NL or_IN pa_IN pl_PL pt_BR pt_PT ro_RO ru_RU si_LK sk_SK sl_SI sq_AL b+sr+Latn+RS sr_RS sv_SE sw_TZ ta_IN
  te_IN th_TH tl_PH tr_TR uk_UA ur_PK uz_UZ vi_VN zh_CN zh_HK zh_TW zu_ZA)

rm -rf "$OUT"
mkdir -p "$OUT/remote/compiled" "$OUT/remote/res/raw" "$OUT/remote/res/values" "$OUT/compiled"

# The remote views' color table.
"$AAPT2" compile "$RES/remote_color_resources_res/values/public.xml" -o "$OUT/remote/compiled"
"$AAPT2" compile "$RES/remote_color_resources_res/values/colors.xml" -o "$OUT/remote/compiled"
"$AAPT2" link -o "$OUT/remote/remote-color-resources.apk" --manifest "$RES/RemoteThemeColorsAndroidManifest.xml" \
  "$OUT"/remote/compiled/*.flat
unzip -q -p "$OUT/remote/remote-color-resources.apk" resources.arsc \
  >"$OUT/remote/res/raw/remote_views_color_resources.arsc"
cp "$RES/remote_color_resources_res/symbols.xml" "$OUT/remote/res/values/symbols.xml"
"$AAPT2" compile --pseudo-localize --dir "$OUT/remote/res" -o "$OUT/compiled/remote.zip"

# core/res, in 6 parts at once: aapt2 compile takes one file after the other.
find "$RES/res" -type f ! -name ".*" | sort >"$OUT/files.txt"
awk -v out="$OUT/part-" '{ print > (out (NR % 6)) }' "$OUT/files.txt"
jobs=()
for part in 0 1 2 3 4 5; do
  mkdir -p "$OUT/part-$part.flat"
  # shellcheck disable=SC2046
  "$AAPT2" compile --pseudo-localize -o "$OUT/part-$part.flat" $(cat "$OUT/part-$part") &
  jobs+=($!)
done
for job in "${jobs[@]}"; do
  wait "$job" || { echo "build-framework-res: aapt2 compile failed" >&2; exit 1; }
done

# Soong's manifest_fixer.py adds android:extractNativeLibs="true" to <application> of an app that does not embed
# its native libraries (add_extract_native_libs); framework-res's manifest leaves it to the fixer.
python3 - "$RES/AndroidManifest.xml" "$OUT/AndroidManifest.xml" <<'FIXER'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
tags = list(re.finditer(r"<application\b", text))
if len(tags) != 1 or "android:extractNativeLibs" in text:
    sys.exit(f"build-framework-res: {sys.argv[1]} needs one <application> without extractNativeLibs")
at = tags[0].end()
open(sys.argv[2], "w", encoding="utf-8").write(text[:at] + ' android:extractNativeLibs="true"' + text[at:])
FIXER

python3 tools/aconfig-flags.py feature-flags --declarations build/sources/frameworks_base \
  --values build/sources/build_release/aconfig/ap3a --out "$OUT/feature-flags.txt" >/dev/null

locales=$(IFS=,; echo "${LOCALES[*]}")
# The compiled files in the order of their names, whatever part compiled them: the order of aapt2 link's inputs is
# the order of the table's string pool.
flats=()
while IFS= read -r flat; do flats+=("$flat"); done < <(
  for part in 0 1 2 3 4 5; do ls "$OUT/part-$part.flat" | sed "s|^|$OUT/part-$part.flat/|"; done \
    | awk -F/ '{ print $NF "\t" $0 }' | sort | cut -f2-)
"$AAPT2" link -o "$OUT/framework-res.apk" --manifest "$OUT/AndroidManifest.xml" -A "$RES/assets" \
  --private-symbols com.android.internal --no-auto-version --auto-add-overlay --enable-sparse-encoding -c "$locales" \
  --enable-compact-entries \
  --min-sdk-version 35 --target-sdk-version 35 --version-code 35 --version-name 15 \
  --feature-flags "@$OUT/feature-flags.txt" \
  "${flats[@]}" "$OUT/compiled/remote.zip"

mkdir -p "$OUT/framework-res"
unzip -q "$OUT/framework-res.apk" -d "$OUT/framework-res"
size=$(wc -c <"$OUT/framework-res.apk" | tr -d ' ')
echo "build-framework-res: $OUT/framework-res.apk ($size bytes) and $OUT/framework-res/"
