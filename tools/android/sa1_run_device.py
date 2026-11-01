#!/usr/bin/env python3
"""SA1b device run: only the named device and app-private files are used."""
import argparse
import subprocess
import time
import struct
import zlib
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--apk', type=Path)
parser.add_argument('--label', required=True)
parser.add_argument('--results', type=Path, default=Path('<作業ディレクトリ>/android/results/sa1b'))
args = parser.parse_args()
args.results.mkdir(parents=True, exist_ok=True)
if any(args.results.glob(args.label + '-*')) or (args.results / (args.label + '.png')).exists():
    raise SystemExit('result label already exists; choose a new label to preserve evidence')
adb = ['<作業ディレクトリ>/android/platform-tools/adb.exe', '-s', '<serial>']
pkg = 'org.pelicanengine.sa1b'
def run(*command):
    result = subprocess.run(adb + list(command), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f'{command}: {result.stderr.decode(errors="replace")}')
    return result.stdout
if args.apk:
    (args.results / (args.label + '-install.txt')).write_bytes(run('install', '-r', str(args.apk.resolve())))
run('shell', 'am', 'force-stop', pkg)
start = run('shell', 'am', 'start', '-n', pkg + '/android.app.NativeActivity')
(args.results / (args.label + '-start.txt')).write_bytes(start)
# No logger process to terminate; dump finite buffers after normal app exit.
for attempt in range(120):
    time.sleep(1)
    pid = subprocess.run(adb + ['shell', 'pidof', pkg], capture_output=True)
    if not pid.stdout.strip():
        break
else:
    raise RuntimeError('app still running after 120 seconds; left running')
(args.results / (args.label + '-logcat.txt')).write_bytes(run('logcat', '-d', '-v', 'threadtime', 'pelican:V', '*:S'))
(args.results / (args.label + '-stderr.txt')).write_bytes(run('exec-out', 'run-as', pkg, 'cat', 'files/stderr.log'))
try:
    (args.results / (args.label + '-engine.log')).write_bytes(run('exec-out', 'run-as', pkg, 'cat', 'files/engine.log'))
except RuntimeError as error:
    print(error)
failures = []
try:
    result = run('exec-out', 'run-as', pkg, 'cat', 'files/engine-result.txt')
    (args.results / (args.label + '-exit.txt')).write_bytes(result)
    if result.strip() != b'0':
        failures.append('engine returned ' + result.decode().strip())
    png = run('exec-out', 'run-as', pkg, 'cat', 'files/sa1_posegraph.png')
    (args.results / (args.label + '.png')).write_bytes(png)
    if not png.startswith(b'\x89PNG\r\n\x1a\n'):
        raise RuntimeError('invalid PNG signature')
    offset, chunks, compressed, header = 8, [], bytearray(), None
    while offset < len(png):
        length = struct.unpack_from('>I', png, offset)[0]
        kind = png[offset + 4:offset + 8]
        data = png[offset + 8:offset + 8 + length]
        crc = struct.unpack_from('>I', png, offset + 8 + length)[0]
        if zlib.crc32(kind + data) & 0xffffffff != crc:
            raise RuntimeError('invalid PNG CRC')
        chunks.append(kind)
        if kind == b'IHDR': header = struct.unpack('>IIBBBBB', data)
        if kind == b'IDAT': compressed.extend(data)
        offset += length + 12
        if kind == b'IEND': break
    if not header or chunks[0] != b'IHDR' or chunks[-1] != b'IEND' or offset != len(png):
        raise RuntimeError('incomplete PNG')
    width, height, depth, color, compression, filtering, interlace = header
    if not width or not height or depth != 8 or color not in (2, 6) or compression or filtering or interlace:
        raise RuntimeError('unsupported or invalid PNG header')
    decoded = zlib.decompress(compressed)
    stride = 1 + width * (3 if color == 2 else 4)
    if len(decoded) != height * stride or any(decoded[i] > 4 for i in range(0, len(decoded), stride)):
        raise RuntimeError('invalid PNG pixels')
except (RuntimeError, struct.error, zlib.error) as error:
    failures.append(str(error))
(args.results / (args.label + '-files.txt')).write_bytes(run('shell', 'run-as', pkg, 'ls', '-laR', 'files'))
print(args.label + ': app exited, artifacts saved')
if failures:
    raise SystemExit('; '.join(failures))
