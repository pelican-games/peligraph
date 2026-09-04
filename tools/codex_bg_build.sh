#!/usr/bin/env bash
# Start a build (or any long command) detached and return immediately.
# Usage: tools/codex_bg_build.sh <tag> <command...>
# Status: tools/codex_bg_status.sh <tag>   (prints RUNNING / DONE:<exit> and the log tail)
# Why: coding-agent shell tools are cut at ~120 s; a full build takes longer.
set -u
tag=$1; shift
dir=${CODEX_BG_DIR:-build/.codex_bg}; mkdir -p "$dir"
log="$dir/$tag.log"; status="$dir/$tag.status"
rm -f "$status"
nohup bash -c "$(printf '%q ' "$@"); echo DONE:\$? > \"$status\"" > "$log" 2>&1 &
echo "started $tag (pid $!) -> $log"
