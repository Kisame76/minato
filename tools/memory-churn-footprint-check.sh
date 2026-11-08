#!/usr/bin/env bash
# The footprint half of fixtures/suite/memory-churn-test.c and memory-release-test.c on the Mac: iOS ends a process
# by its physical footprint (phys_footprint), not its resident size, which is all the guest can read of itself. Runs
# the fixtures once per scenario on its own through apk-run, takes the peak footprint from the runtime's exit
# measures (--measures), and prints each scenario's peak above an idle run's. A scenario that releases what it
# frees stays near one block (128 MiB); one that keeps it reaches two. Android has no footprint, so there is no
# reference: the bound is what Android's resident size does in the fixtures' own tests
# (fixtures/expected/memory-churn-test.out and memory-release-test.out).
# Needs the fixtures (build/fixtures/bin/memory-churn-test and memory-release-test, tools/build-fixtures.sh), the
# converter and apk-run (build/native/macos-app) and bionic's libraries (tools/build-bionic.sh).
# Usage: tools/memory-churn-footprint-check.sh
# Optional environment:
#   FIXTURES_BIN  the fixtures' folder, default: build/fixtures/bin
#   NATIVE        the native build, default: build/native/macos-app
set -euo pipefail
cd "$(dirname "$0")/.."

bin=${FIXTURES_BIN:-build/fixtures/bin}
native=${NATIVE:-build/native/macos-app}
convert=$native/importer/converter/apk-convert
run=$native/runtime/apk-run
work=$(mktemp -d "${TMPDIR:-/tmp}/memory-churn-footprint.XXXXXX")
trap 'rm -rf "$work"' EXIT
for needed in "$bin/memory-churn-test" "$bin/memory-release-test" "$convert" "$run" build/bionic/lib/libc.so; do
  if [[ ! -e "$needed" ]]; then echo "skipped: $needed is not built"; exit 0; fi
done

mkdir -p "$work/system-lib64" "$work/tmp"
for program in memory-churn-test memory-release-test; do
  "$convert" convert "$bin/$program" "$work/$program.dylib" --sign - >/dev/null
done
for library in libc libc-bigregions libm libdl ld-android liblog libnetd_client libstdc++ libandroid libnativewindow \
    libz libcamera2ndk libmediandk libsync libamidi libbinder_ndk libneuralnetworks libOpenMAXAL; do
  "$convert" convert "build/bionic/lib/$library.so" "$work/system-lib64/$library.so.dylib" --sign - >/dev/null
done

peak() {
  MINATO_FAULT_LOG=0 "$run" --mount "/data/local/tmp=$work/tmp" --libraries "/system/lib64=$work/system-lib64" \
    --cwd /data/local/tmp --argv0 "./$1" --env LD_LIBRARY_PATH=/data/local/tmp \
    --mount "/system=build/bionic/system" --measures "$work/measures" "$work/$1.dylib" "$2" \
    >/dev/null 2>&1
  awk '$1 == "footprint-peak" { printf "%d", $2 / 1048576 }' "$work/measures"
}

idle=$(peak memory-churn-test idle)
echo "idle: footprint peak $idle MiB"
for scenario in malloc_free mmap_pieces madvise_dontneed madvise_free dontneed_other_range free_other_range \
    free_returns private_file; do
  printf 'memory-churn-test %s: footprint peak %d MiB above idle\n' "$scenario" \
    $(($(peak memory-churn-test "$scenario") - idle))
done
for scenario in remove_anonymous cold dontneed_private_anonymous dontneed_shared_anonymous dontneed_memfd remap \
    munmap_piece gc_reserve protect_none; do
  printf 'memory-release-test %s: footprint peak %d MiB above idle\n' "$scenario" \
    $(($(peak memory-release-test "$scenario") - idle))
done
