#!/usr/bin/env bash
# Releases Minato (docs/RELEASING.md): moves CHANGELOG.md's Unreleased section under the version, sets the version,
# build number and label in project.yml and commits; builds the Release IPA; tags and pushes. The tag's workflow
# (.github/workflows/release.yml) makes the GitHub release from that CHANGELOG section, and this script uploads
# Minato.ipa, Minato.ipa.sha256 and source-entry.json (the website's SideStore and AltStore source entry) to it, with
# the source archive of the GPL-2.0 parts and its .sha256 (tools/make-source-archive.py).
# Run it in a clean clone of main with the sources fetched (docs/BUILDING.md), after the release checks.
# Usage: tools/release.sh VERSION [--dry-run]
#   VERSION    1.0.0, or 1.0.0-beta.1 for a beta (a pre-release on GitHub)
#   --dry-run  builds the IPA and the entry into build/release with Unreleased as the notes, and changes, tags and
#              pushes nothing; MINATO_REPO (owner/name) stands in for the origin remote where there is no git
set -euo pipefail
cd "$(dirname "$0")/.."

version="${1:-}" dry_run=""
[[ "${2:-}" != --dry-run ]] || dry_run=yes
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?$ ]]; then
  echo "usage: tools/release.sh VERSION [--dry-run], VERSION like 1.0.0 or 1.0.0-beta.1" >&2
  exit 64
fi
# Apple takes only numbers as the app's version, so a beta's suffix becomes a label that About shows, and the build
# number, which only grows, tells the betas of one version apart in SideStore and AltStore.
marketing="${version%%-*}"
label=""
[[ "$version" != *-beta.* ]] || label="Beta ${version##*-beta.}"
tag="v$version"
repo=$(git remote get-url origin 2>/dev/null |
  sed -E 's#^(git@github\.com:|https://github\.com/)##; s#\.git$##' || true)
repo="${repo:-${MINATO_REPO:-}}"
[[ -n "$repo" ]] || { echo "no origin remote and no MINATO_REPO" >&2; exit 1; }
out=build/release

if [[ -z "$dry_run" ]]; then
  [[ "$(git branch --show-current)" == main ]] || { echo "a release is made from main" >&2; exit 1; }
  [[ -z "$(git status --porcelain)" ]] || { echo "the tree is not clean: release from a clean clone" >&2; exit 1; }
  git fetch -q origin main
  [[ "$(git rev-parse HEAD)" == "$(git rev-parse origin/main)" ]] || { echo "main is not origin/main" >&2; exit 1; }
  if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then echo "$tag exists" >&2; exit 1; fi
  grep -q '^ *MINATO_RELEASE_LABEL:' project.yml || { echo "project.yml has no MINATO_RELEASE_LABEL" >&2; exit 1; }
fi

# The guest libraries the app packs must be those of this tree, not left over from an older build. Before the release
# commit: a failing guard must leave nothing behind that a rerun refuses ("main is not origin/main").
python3 tools/check-guest-fresh.py

# The notes: the Unreleased section, which the release commit puts under the version's heading.
notes=$(awk '/^## / { if (found) exit; if ($0 == "## Unreleased") found = 1; next } found' CHANGELOG.md |
  sed -e '/./,$!d')
grep -q '[^[:space:]]' <<< "$notes" || { echo "CHANGELOG.md's Unreleased section is empty" >&2; exit 1; }
build=$(( $(sed -nE 's/^ *CURRENT_PROJECT_VERSION: "?([0-9]+)"?$/\1/p' project.yml) + 1 ))

if [[ -z "$dry_run" ]]; then
  python3 - "$version" "$(date -u +%Y-%m-%d)" <<'EOF'
import sys

version, today = sys.argv[1:]
text = open("CHANGELOG.md").read()
assert text.count("\n## Unreleased\n") == 1, "CHANGELOG.md needs exactly one Unreleased heading"
text = text.replace("\n## Unreleased\n", f"\n## Unreleased\n\n## {version} - {today}\n", 1)
with open("CHANGELOG.md", "w") as f:
    f.write(text)
EOF
  sed -i '' -E -e "s/^( *MARKETING_VERSION: ).*/\1\"$marketing\"/" \
    -e "s/^( *CURRENT_PROJECT_VERSION: ).*/\1\"$build\"/" -e "s/^( *MINATO_RELEASE_LABEL: ).*/\1\"$label\"/" project.yml
  git commit -q -m "chore: release $tag" CHANGELOG.md project.yml
  echo "committed the release of $tag (local until the push below)"
fi

# A Release build links the runtime's and graphics/host's Release builds (ADR-0037, decision 3).
tools/gen.sh
tools/build-native.sh --app --release
tools/build-graphics-host.sh --release
rm -rf "$out"
mkdir -p "$out"

# The source of the GPL-2.0 with the Classpath Exception parts (core-oj, libopenjdk, libopenjdkjvm) goes on the release
# page next to the IPA (GPL-2.0 section 3(a); tools/make-source-archive.py), and the app's Licenses/*/SOURCE.txt name
# its page and SHA-256, so the archive is made first. A dry run makes it again and compares the bytes.
release_url="https://github.com/$repo/releases/tag/$tag"
archive=$(python3 tools/make-source-archive.py "$version" "$release_url" "$out")
archive_sha=$(cut -d' ' -f1 "$archive.sha256")
if [[ -n "$dry_run" ]]; then
  again=$(python3 tools/make-source-archive.py "$version" "$release_url" "$out/again")
  cmp -s "$archive" "$again" || { echo "the source archive is not reproducible: $again differs" >&2; exit 1; }
  rm -rf "$out/again"
fi
xcodebuild -project Minato.xcodeproj -scheme Minato -configuration Release -destination generic/platform=iOS \
  -archivePath "$out/Minato.xcarchive" -derivedDataPath "$out/DerivedData" -quiet archive CODE_SIGNING_ALLOWED=NO \
  MARKETING_VERSION="$marketing" CURRENT_PROJECT_VERSION="$build" MINATO_RELEASE_LABEL="$label" \
  MINATO_COMMIT="$(git rev-parse --short=12 HEAD 2>/dev/null || true)" \
  MINATO_SOURCE_URL="$release_url" MINATO_SOURCE_SHA256="$archive_sha"
app="$out/Minato.xcarchive/Products/Applications/Minato.app"

# A public release carries no local build paths: the guest libraries must not embed the build root or $HOME.
tools/check-no-build-paths.sh "$app"

# The license text of everything THIRD_PARTY.md lists travels with the app: a row without a text stops the release.
python3 tools/license-check.py "$app"

# SideStore and AltStore sign the app with the user's certificate. An ad-hoc signature, from the inside out, carries
# the entitlements that they compare with the source entry's.
find "$app" -depth \( -name '*.framework' -o -name '*.dylib' -o -name '*.appex' \) -print0 |
  xargs -0 -n 1 codesign -f -s -
codesign -f -s - --entitlements app/Minato.entitlements "$app"
codesign -d --entitlements - --xml "$app" 2>/dev/null > "$out/entitlements.plist"

mkdir "$out/Payload"
cp -R "$app" "$out/Payload/"
(cd "$out" && zip -qry Minato.ipa Payload && rm -rf Payload)
(cd "$out" && shasum -a 256 Minato.ipa > Minato.ipa.sha256)

python3 tools/release-entry.py "$out" "$app" "$marketing" "$build" \
  "https://github.com/$repo/releases/download/$tag/Minato.ipa" "$(date -u +%Y-%m-%d)" "$notes"
echo "built $out/Minato.ipa ($marketing, build $build${label:+, $label}) and $out/source-entry.json"
[[ -z "$dry_run" ]] || exit 0

git tag -a "$tag" -m "Minato $version"
git push -q origin main "$tag"
for _ in $(seq 60); do
  gh release view "$tag" >/dev/null 2>&1 && break
  sleep 10
done
if ! gh release view "$tag" >/dev/null; then
  echo "no release for $tag after 10 minutes: see the release workflow" >&2
  exit 1
fi
gh release upload "$tag" "$out/Minato.ipa" "$out/Minato.ipa.sha256" "$out/source-entry.json" \
  "$archive" "$archive.sha256"
echo "released $tag: https://github.com/$repo/releases/tag/$tag"
