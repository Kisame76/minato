#!/usr/bin/env bash
# Packs one of our test apps into an APK signed with the debug key (ADR-0031). Builds make-apk with
# SwiftPM in release into build/make-apk, a scratch path of its own that never waits for the .build of
# `swift test`, and runs it with the arguments; SwiftPM rebuilds only what changed. The debug key is
# ~/.android/minato-debug-key, made on first use.
# Usage: tools/make-apk.sh --manifest <AndroidManifest.xml> [--lib <lib*.so>]... [--assets <folder>] --out <file.apk>
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
package="$root/importer/APKKit"
scratch="$root/build/make-apk"

swift build -c release --package-path "$package" --scratch-path "$scratch" --product make-apk -q >&2
exec "$(swift build -c release --package-path "$package" --scratch-path "$scratch" --show-bin-path)/make-apk" "$@"
