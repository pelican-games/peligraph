#!/usr/bin/env bash
# Usage: tools/codex_bg_status.sh <tag> [tail-lines]
set -u
tag=$1; n=${2:-5}
dir=${CODEX_BG_DIR:-build/.codex_bg}; log="$dir/$tag.log"; status="$dir/$tag.status"
if [ -f "$status" ]; then cat "$status"; else echo RUNNING; fi
[ -f "$log" ] && { echo "--- tail $log"; tail -n "$n" "$log"; }
