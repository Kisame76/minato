#!/usr/bin/env bash
# Prints the -ffile-prefix-map flags that keep this machine's build paths out of the guest libraries. The
# guest builds pass them to CMake (CMAKE_C_FLAGS/CMAKE_CXX_FLAGS/CMAKE_ASM_FLAGS); without them __FILE__
# (CHECK/LOG macros, BoringSSL) and debug info bake in the absolute build root, which a public release must
# not carry (tools/check-no-build-paths.sh enforces it). -ffile-prefix-map covers -fmacro-prefix-map
# (__FILE__) and -fdebug-prefix-map (debug info). The vendored AOSP trees map to nothing so their paths read
# like AOSP's (bionic/..., art/..., system/...); our own tree maps to a repo-relative path. The build root's
# real path is mapped too, since a release clone may sit under a symlinked path. Run from the repo root (the
# guest build scripts cd there first).
set -euo pipefail

emit() {
  # Most specific first: the vendored AOSP trees to nothing, then everything else under the root to a
  # repo-relative path. Any path under the root is stripped of its local prefix either way.
  local r="$1"
  printf -- ' -ffile-prefix-map=%s/build/art/tree/=' "$r"
  printf -- ' -ffile-prefix-map=%s/build/hwui/tree/=' "$r"
  printf -- ' -ffile-prefix-map=%s/build/sources/=' "$r"
  printf -- ' -ffile-prefix-map=%s/=' "$r"
}

root="$PWD"
real="$(pwd -P)"
flags="$(emit "$root")"
[[ "$real" != "$root" ]] && flags="$flags$(emit "$real")"
printf '%s\n' "${flags# }"
