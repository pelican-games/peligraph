#!/usr/bin/env bash
# Official Khronos Android binary, pinned to this SDK release.
set -euo pipefail
export MSYS_NO_PATHCONV=1
DEPS=${ANDROID_DEPS:-<作業ディレクトリ>/android/deps}
URL=https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-1.4.309.0/android-binaries-1.4.309.0.zip
ARCHIVE="$DEPS/android-binaries-1.4.309.0.zip"
DEST="$DEPS/vvl-vulkan-sdk-1.4.309.0"
PYTHON=${PYTHON:-<home>/AppData/Roaming/uv/python/cpython-3.12-windows-x86_64-none/python.exe}
mkdir -p "$DEST"
if [[ ! -f "$ARCHIVE" ]]; then
  curl --fail --location --connect-timeout 15 --max-time 180 "$URL" -o "$ARCHIVE.part"
  mv "$ARCHIVE.part" "$ARCHIVE"
fi
"$PYTHON" - "$ARCHIVE" "$DEST" "$URL" <<'PY'
import hashlib, json, sys, zipfile
from pathlib import Path
archive, dest, url = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
with zipfile.ZipFile(archive) as z:
    matches = [n for n in z.namelist() if n.endswith('arm64-v8a/libVkLayer_khronos_validation.so')]
    if len(matches) != 1:
        raise SystemExit(f'expected one arm64-v8a library: {matches}')
    data = z.read(matches[0])
    target = dest / 'arm64-v8a/libVkLayer_khronos_validation.so'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    provenance = {'tag': 'vulkan-sdk-1.4.309.0', 'url': url,
                  'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                  'library_sha256': hashlib.sha256(data).hexdigest(), 'member': matches[0]}
    (dest / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print(json.dumps(provenance, indent=2))
PY
