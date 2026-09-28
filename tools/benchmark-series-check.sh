#!/usr/bin/env bash
# Checks how tools/benchmark.sh takes bench-plan's answer (ADR-0037 d), without a device or a build: a
# copy of the script runs in a scratch tree whose swift, git, xcrun, sim.sh and bench-plan are fakes. A
# line that is neither "done" nor "run <run ID>" must stop the series with exit 1 and bench-plan's
# output, before anything uses it, and leave the results untouched; a run ID is used as it comes.
# Usage: tools/benchmark-series-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
failed=0

# A scratch tree whose bench-plan prints $1 first and "done" after.
make_tree() {
  local tree=$1 answer=$2
  mkdir -p "$tree/tools" "$tree/fake" "$tree/bin" "$tree/container/tmp/benchmark"
  cp tools/benchmark.sh "$tree/tools/"
  printf '#!/bin/sh\nexit 0\n' >"$tree/tools/sim.sh"
  printf 'print("FAKE-UDID fake")\n' >"$tree/tools/find-simulator.py"
  cat >"$tree/fake/swift" <<EOF
#!/bin/sh
case " \$* " in *" --show-bin-path "*) echo "$tree/bin" ;; esac
EOF
  cat >"$tree/fake/git" <<'EOF'
#!/bin/sh
[ "$1" = rev-parse ] && echo abc1234
exit 0
EOF
  cat >"$tree/fake/xcrun" <<EOF
#!/bin/sh
echo "\$*" >>"$tree/xcrun.log"
case "\$*" in *get_app_container*) echo "$tree/container" ;; esac
EOF
  cat >"$tree/bin/bench-plan" <<EOF
#!/bin/sh
if [ -e "$tree/asked" ]; then echo done; exit 0; fi
touch "$tree/asked"
printf '%s\n' '$answer'
EOF
  chmod +x "$tree/tools/"*.sh "$tree/tools/find-simulator.py" "$tree/fake/"* "$tree/bin/bench-plan"
  # Earlier results: a finished run and a file beside them.
  mkdir -p "$tree/build/benchmark-simulator/gles-120-heavy-native-00"
  echo kept >"$tree/build/benchmark-simulator/gles-120-heavy-native-00/run.csv"
  echo kept >"$tree/build/benchmark-simulator/notes"
}

# Runs the copy; prints its exit status, its output goes to $tree/out.
run_tree() {
  local tree=$1 status=0
  PATH="$tree/fake:$PATH" "$tree/tools/benchmark.sh" --simulator >"$tree/out" 2>&1 || status=$?
  echo "$status"
}

untouched() {
  local results=$1/build/benchmark-simulator
  [[ "$(cat "$results/gles-120-heavy-native-00/run.csv" 2>/dev/null)" == kept &&
     "$(cat "$results/notes" 2>/dev/null)" == kept && "$(ls "$results" | wc -l | tr -d ' ')" == 2 ]]
}

# Answers that are not a run ID: empty, a path, a pattern, a short number, a missing prefix, more words,
# two lines.
index=0
for answer in "run " "run" "run ." "run ../build" "run *" "run gles-120-heavy-apk-3" "gles-120-heavy-apk-03" \
  "run gles-120-heavy-apk-03 more" "run gles-120-heavy-apk-03/.." \
  $'run gles-120-heavy-apk-03\nrun gles-120-heavy-apk-04' "Done" "hello"; do
  index=$((index + 1))
  tree="$WORK/bad$index"
  make_tree "$tree" "$answer"
  status=$(run_tree "$tree")
  problems=""
  [[ "$status" == 1 ]] || problems+=" exit $status"
  untouched "$tree" || problems+=" results changed"
  [[ ! -e "$tree/xcrun.log" ]] || problems+=" launched"
  grep -qF -- "${answer%%$'\n'*}" "$tree/out" || problems+=" no bench-plan output"
  if [[ -z "$problems" ]]; then
    echo "same     refused '${answer//$'\n'/\\n}'"
  else
    echo "DIFFERS  '${answer//$'\n'/\\n}':$problems"
    failed=1
  fi
done

# A run ID is launched, fetched and kept beside the earlier results.
tree="$WORK/good"
make_tree "$tree" "run gles-60-light-apk-02"
mkdir -p "$tree/container/tmp/benchmark/gles-60-light-apk-02"
cat >"$tree/container/tmp/benchmark/gles-60-light-apk-02/run.csv" <<'EOF'
run,valid,invalid_reasons
gles-60-light-apk-02,1,
EOF
status=$(run_tree "$tree")
problems=""
[[ "$status" == 0 ]] || problems+=" exit $status"
grep -q -- "-runBenchmark gles-60-light-apk-02" "$tree/xcrun.log" 2>/dev/null || problems+=" not launched"
[[ -f "$tree/build/benchmark-simulator/gles-60-light-apk-02/run.csv" ]] || problems+=" not fetched"
[[ "$(cat "$tree/build/benchmark-simulator/notes" 2>/dev/null)" == kept ]] || problems+=" results changed"
if [[ -z "$problems" ]]; then
  echo "same     ran gles-60-light-apk-02"
else
  echo "DIFFERS  gles-60-light-apk-02:$problems"
  sed 's/^/  /' "$tree/out"
  failed=1
fi
exit $failed
