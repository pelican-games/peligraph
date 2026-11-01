#!/usr/bin/env bash
# SA0: push the Android test executables and their data to a device and run
# them there. Run from Git Bash at the repository root after building
# build-android (target pelican_sa0_tests). Everything on the device stays
# under $REMOTE.
#   SERIAL=<adb serial> bash tools/android/sa0_run_device.sh [test-target ...]
set -uo pipefail
export MSYS_NO_PATHCONV=1
ADB=${ADB:-<作業ディレクトリ>/android/platform-tools/adb.exe}
SERIAL=${SERIAL:-<serial>}
BUILD=${BUILD:-build-android}
REMOTE=${REMOTE:-/data/local/tmp/sa0}
OUT=${OUT:-$BUILD/device_results}
PUSH_DATA=${PUSH_DATA:-1}
adb() { "$ADB" -s "$SERIAL" "$@"; }

mkdir -p "$OUT"
adb shell "mkdir -p $REMOTE/bin $REMOTE/src $REMOTE/work"

if [ "$PUSH_DATA" = 1 ]; then
  # The source-tree mirror that PELICAN_TEST_SOURCE_DIR names on the device.
  tar -cf "$BUILD/sa0_data.tar" test/fixtures projects src/core/resources src/project
  adb push "$BUILD/sa0_data.tar" "$REMOTE/sa0_data.tar" >/dev/null
  adb shell "cd $REMOTE/src && tar -xf $REMOTE/sa0_data.tar && rm $REMOTE/sa0_data.tar"
fi

if [ $# -gt 0 ]; then
  tests=("$@")
else
  mapfile -t tests < <(tr -d '\r' < "$BUILD/sa0_tests.txt" | sed '/^$/d')
fi

summary="$OUT/summary.tsv"
: > "$summary"
for t in "${tests[@]}"; do
  exe=$(find "$BUILD" -maxdepth 1 -name "$t" -type f | head -1)
  if [ -z "$exe" ]; then
    printf '%s\tNOT_BUILT\t-\t-\n' "$t" | tee -a "$summary"
    continue
  fi
  adb push "$exe" "$REMOTE/bin/$t" >/dev/null
  adb shell "chmod 755 $REMOTE/bin/$t"
  case "$t" in
    pelican_test_project_execution_leaf|pelican_test_project_library_boundary|pelican_test_render_graph_ir_boundary)
      args="" ;;
    *) args="-w NoAssertions" ;;
  esac
  # Each test runs in a fresh work dir with TMPDIR inside the sandbox.
  adb shell "rm -rf $REMOTE/work/$t && mkdir -p $REMOTE/work/$t && cd $REMOTE/work/$t && TMPDIR=$REMOTE/work/$t HOME=$REMOTE/work/$t timeout 900 $REMOTE/bin/$t $args; echo SA0_EXIT=\$?" \
    > "$OUT/$t.log" 2>&1
  code=$(grep -o 'SA0_EXIT=[0-9]*' "$OUT/$t.log" | tail -1 | cut -d= -f2)
  totals=$(grep -E '^(test cases|All tests passed)' "$OUT/$t.log" | tail -1 | tr -d '\r')
  printf '%s\t%s\t%s\n' "$t" "${code:-?}" "${totals:-}" | tee -a "$summary"
done
