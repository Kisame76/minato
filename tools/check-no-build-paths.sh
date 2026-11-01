#!/usr/bin/env bash
# Fails if a built app (or any directory) embeds this machine's build paths, so a public release carries no
# local paths. It scans every regular file as bytes for the build root, the root's real path, or $HOME, and
# descends into archives (jar/apk/zip/aar) so a path compressed inside one does not slip through. The guest
# libraries used to bake these in through __FILE__ (CHECK/LOG macros, BoringSSL) until -ffile-prefix-map was
# added to their builds (tools/prefix-map-flags.sh). tools/release.sh runs it right after the archive; it
# also runs on a given app:
#   tools/check-no-build-paths.sh build/release/Minato.xcarchive/Products/Applications/Minato.app
set -euo pipefail

target="${1:-}"
if [[ -z "$target" || ! -e "$target" ]]; then
  echo "usage: tools/check-no-build-paths.sh <app-or-directory>" >&2
  exit 64
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
real="$(cd "$root" && pwd -P)"
home="${HOME:-}"

# The needles, one per line, de-duplicated. real often equals root; the release clone usually sits under
# HOME, which then also catches a toolchain (an NDK or SDK) under the home folder. -F matches them as fixed
# strings, and the root is caught wherever its spelling stays intact, a "build/../" path among them.
patterns="$(mktemp)"
trap 'rm -f "$patterns"' EXIT
{
  printf '%s\n' "$root"
  [[ "$real" != "$root" ]] && printf '%s\n' "$real"
  # A HOME of "/" or one character would match everything; skip it.
  [[ -n "$home" && ${#home} -ge 2 && "$home" != "$root" && "$home" != "$real" ]] && printf '%s\n' "$home"
} | sort -u > "$patterns"

offenders=""  # one "  <path>: Nx[needle] ..." line per file (or archive entry) that carries a needle

# Pass 1: every regular file's raw bytes (-r walks without following symlinks, -a reads binaries as text).
# This covers plain data files and a stored archive entry; Pass 2 covers compressed entries.
while IFS= read -r file; do
  [[ -n "$file" ]] || continue
  line="  ${file#"$target"/}:"
  while IFS= read -r needle; do
    [[ -n "$needle" ]] || continue
    count="$(grep -aoF -- "$needle" "$file" 2>/dev/null | wc -l | tr -d ' ')"
    [[ "${count:-0}" -gt 0 ]] && line="$line ${count}x[$needle]"
  done < "$patterns"
  offenders="$offenders$line"$'\n'
done < <(grep -rlaF -f "$patterns" "$target" 2>/dev/null || true)

# Pass 2: the decompressed entries of every archive (found by the PK magic, so an odd extension still counts),
# nested archives included. Prints one "  <archive>!<entry>: Nx[needle] ..." line per entry that carries one.
# A failing Python fails the check (set -e): a pass that cannot run must not read as clean.
arc="$(python3 - "$target" "$patterns" <<'PY'
import io, os, sys, zipfile

target, patfile = sys.argv[1], sys.argv[2]
with open(patfile, "rb") as f:
    needles = [line.rstrip(b"\n") for line in f if line.strip()]

def scan(name, data, depth):
    if depth > 8:
        return
    try:
        zf = zipfile.ZipFile(io.BytesIO(data))
    except Exception:
        return
    for info in zf.infolist():
        if info.is_dir():
            continue
        try:
            raw = zf.read(info)
        except Exception:
            continue
        hits = [(n, raw.count(n)) for n in needles]
        hits = [(n, c) for n, c in hits if c]
        if hits:
            counts = " ".join("%dx[%s]" % (c, n.decode("utf-8", "replace")) for n, c in hits)
            print("  %s!%s: %s" % (name, info.filename, counts))
        low = info.filename.lower()
        if low.endswith((".jar", ".zip", ".apk", ".aar", ".apks", ".xapk")):
            scan("%s!%s" % (name, info.filename), raw, depth + 1)

for dirpath, dirs, files in os.walk(target):
    for name in files:
        path = os.path.join(dirpath, name)
        if os.path.islink(path):
            continue
        try:
            with open(path, "rb") as f:
                head = f.read(4)
                if head != b"PK\x03\x04":
                    continue
                data = head + f.read()
        except Exception:
            continue
        scan(os.path.relpath(path, target), data, 0)
PY
)"
[[ -n "$arc" ]] && offenders="$offenders$arc"$'\n'

if [[ -z "${offenders//[$'\n']/}" ]]; then
  echo "check-no-build-paths: clean ($target)"
  exit 0
fi

echo "check-no-build-paths: FAIL -- $target embeds this machine's build paths:" >&2
printf '%s' "$offenders" | grep -v '^[[:space:]]*$' >&2
echo "add -ffile-prefix-map for the build root to the guest builds (see tools/build-bionic.sh)" >&2
exit 1
