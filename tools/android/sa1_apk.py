#!/usr/bin/env python3
"""Stage APK assets with a content version, or append stored native libraries."""
import hashlib
from pathlib import Path
import shutil
import sys
import zipfile

mode, *args = sys.argv[1:]
if mode == 'assets':
    source, destination = map(Path, args)
    destination.mkdir(parents=True, exist_ok=True)
    # A unique staging directory supplied by sa1_build_apk.sh; no tree deletion.
    files = sorted(p for p in source.rglob('*')
                   if p.is_file() and '.pelican' not in p.relative_to(source).parts)
    version = hashlib.sha256(b'pelican-sa1-assets-v1\0')
    names = []
    for path in files:
        name = path.relative_to(source).as_posix()
        if '\n' in name or '\r' in name or '\\' in name or path.is_symlink():
            raise SystemExit(f'unsupported asset path: {path}')
        data = path.read_bytes()
        version.update(name.encode('utf-8') + b'\0' + hashlib.sha256(data).digest())
        target = destination / 'project' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        names.append(name)
    (destination / 'sa1-files.txt').write_text('\n'.join(names) + '\n', encoding='utf-8', newline='\n')
    (destination / 'sa1-version.txt').write_text(version.hexdigest(), encoding='ascii')
    print(f'assets: {len(files)} files, sha256 version {version.hexdigest()}')
elif mode == 'assets-zip':
    apk, directory = map(Path, args)
    # aapt2 -A silently filters dotfiles and other names. Pack the exact
    # staged inventory so extraction can open every entry it advertises.
    with zipfile.ZipFile(apk, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
        for asset in sorted(directory.rglob('*')):
            if asset.is_file():
                archive.write(asset, 'assets/' + asset.relative_to(directory).as_posix())
elif mode == 'libs':
    apk, directory = map(Path, args)
    with zipfile.ZipFile(apk, 'a', compression=zipfile.ZIP_STORED) as archive:
        for library in sorted(directory.glob('*.so')):
            archive.write(library, 'lib/arm64-v8a/' + library.name)
else:
    raise SystemExit('expected assets, assets-zip or libs')
