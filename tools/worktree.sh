#!/usr/bin/env bash
# Configure/build/test one worktree with isolated artifacts and read-only
# dependency sources from the primary worktree.  WP385 intentionally makes the
# environment explicit on every invocation; do not replace this with a shell
# profile side effect.
set -euo pipefail

usage() { echo "usage: $0 {configure|build|test} <worktree> [cmake args...]" >&2; exit 2; }
[[ $# -ge 2 ]] || usage
action=$1; worktree=$2; shift 2
worktree=$(cd "$worktree" && pwd)
build="$worktree/build"
manifest="$build/pelican_worktree_manifest.json"
export MSBUILDDISABLENODEREUSE=1
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-16}"
# Machine-specific locations; override through the environment.
uv_python="${PELICAN_WORKTREE_PYTHON:-$(cygpath -m "$APPDATA")/uv/python/cpython-3.12-windows-x86_64-none/python.exe}"
qt_prefix="${PELICAN_WORKTREE_QT_PREFIX:-$(cygpath -m "$USERPROFILE")/Qt/6.10.3/msvc2022_64}"
manifest_python="$(cygpath -u "$uv_python")"

primary=$(git -C "$worktree" worktree list --porcelain | awk '/^worktree / { print substr($0,10); exit }')
deps="$primary/build/_deps"

configure_args=(
  -DCMAKE_PREFIX_PATH="$qt_prefix"
  -DPELICAN_WITH_SPIRV_LINK=ON -DSKIP_DEVSTUDIO=ON
  -DPython3_EXECUTABLE="$uv_python"
)
for dep in argparse battery-embed catch2 glfw glm imgui_vendor miniaudio nlohmann openxr_sdk picosha2 quill spirv_headers spirv_reflect spirv_tools stb tinyexr tinygltf vma-hpp; do
  source_dir="$deps/$dep-src"
  [[ -d "$source_dir" ]] || continue
  option=$(printf '%s' "$dep" | tr '[:lower:]' '[:upper:]')
  configure_args+=("-DFETCHCONTENT_SOURCE_DIR_${option}=$source_dir")
done
if [[ "${PELICAN_WORKTREE_GENERATOR:-}" == Ninja ]]; then configure_args=(-G Ninja "${configure_args[@]}"); fi
# Only the wrapper-controlled arguments can be recomputed at build/test time;
# extra arguments passed to `configure` stay in feature_fingerprint.
wrapper_fingerprint_input=$(printf '%s\n' "${configure_args[@]}")

# The manifest records what `configure` actually established.  `build` and
# `test` recompute the wrapper-controlled half of it and refuse, by name, when
# it no longer matches -- a manifest written for another generator, another
# dependency source root, another feature set, or without node reuse disabled.
write_manifest() {
  "$manifest_python" - "$manifest" "$deps" "${PELICAN_WORKTREE_GENERATOR:-default}" \
    write "$wrapper_fingerprint_input" "$@" <<'PY'
import hashlib, json, os, pathlib, sys
path, deps, generator, _mode, wrapper, *args = sys.argv[1:]
payload = {"schema_version": 1, "generator": generator, "config": "Debug",
 "dependency_source_root": deps, "node_reuse_disabled": os.environ.get("MSBUILDDISABLENODEREUSE") == "1",
 "wrapper_fingerprint": hashlib.sha256(wrapper.encode()).hexdigest(),
 "feature_fingerprint": hashlib.sha256("\0".join(args).encode()).hexdigest()}
pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
pathlib.Path(path).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
PY
}
check_manifest() {
  [[ -f "$manifest" ]] || {
    echo "WP385: worktree manifest missing: $manifest; run 'tools/worktree.sh configure' first" >&2
    exit 2
  }
  "$manifest_python" - "$manifest" "$deps" "${PELICAN_WORKTREE_GENERATOR:-default}" \
    check "$wrapper_fingerprint_input" <<'PY'
import hashlib, json, os, sys
path, deps, generator, _mode, wrapper = sys.argv[1:]
manifest = json.load(open(path, encoding="utf-8"))
expected = {
  "generator": generator,
  "dependency_source_root": deps,
  "wrapper_fingerprint": hashlib.sha256(wrapper.encode()).hexdigest(),
}
bad = [f"{k}: manifest={manifest.get(k)!r} now={v!r}"
       for k, v in expected.items() if manifest.get(k) != v]
if not manifest.get("node_reuse_disabled") or os.environ.get("MSBUILDDISABLENODEREUSE") != "1":
    bad.append("node_reuse_disabled: MSBUILDDISABLENODEREUSE=1 is required on both sides")
if bad:
    raise SystemExit("WP385: worktree manifest mismatch; reconfigure through "
                     "tools/worktree.sh configure\n  " + "\n  ".join(bad))
PY
}

case "$action" in
configure)
  if [[ "${PELICAN_WORKTREE_GENERATOR:-}" == Ninja ]]; then
    # Git Bash cannot retain vcvars64's environment.  Ask cmd.exe to establish
    # it and invoke CMake in the same process, using the Ninja shipped with VS.
    vcvars='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    command_line="call \"$vcvars\" >nul && cmake -S \"$worktree\" -B \"$build\""
    for arg in "${configure_args[@]}" "$@"; do command_line+=" \"$arg\""; done
    cmd.exe /d /c "$command_line"
  else
    cmake -S "$worktree" -B "$build" "${configure_args[@]}" "$@"
  fi
  write_manifest "${configure_args[@]}" "$@"
  ;;
build)
  check_manifest
  cmake --build "$build" --config Debug "$@"
  ;;
test)
  check_manifest
  ctest --test-dir "$build" -C Debug "$@"
  ;;
*) usage ;;
esac
