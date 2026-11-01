#!/usr/bin/env python3
"""Finite SA1c/d foreground run, byte-preserving capture and app file logs."""
import argparse
import json
import re
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--apk', type=Path)
parser.add_argument('--label', required=True)
parser.add_argument('--args', default='--frames 600 --timing-frames 120')
parser.add_argument('--headless', action='store_true')
parser.add_argument('--results', type=Path, default=Path('<作業ディレクトリ>/sa1/build-android/sa1cd-results'))
args = parser.parse_args()
out = args.results / args.label
out.mkdir(parents=True, exist_ok=False)
adb = ['<作業ディレクトリ>/android/platform-tools/adb.exe', '-s', '<serial>']
pkg = 'org.pelicanengine.sa1b'


def run(*command, check=True):
    result = subprocess.run(adb + list(command), capture_output=True, timeout=45)
    if check and result.returncode:
        raise RuntimeError(f'{command}: {result.stderr.decode(errors="replace")}')
    return result.stdout


if args.apk:
    (out / 'install.txt').write_bytes(run('install', '-r', str(args.apk.resolve())))
run('shell', 'am', 'force-stop', pkg)
run('shell', 'run-as', pkg, 'rm', '-f', 'files/engine.log', 'files/engine-result.txt')
(out / 'thermal-before.txt').write_bytes(run('shell', 'dumpsys', 'thermalservice'))
intent_args = args.args + (' --headless' if args.headless else '')
# adb shell joins argv; quote the single string extra for the Android shell.
if "'" in intent_args:
    raise ValueError('single quote is unsupported in intent args')
command = ['shell', 'am', 'start', '-n', pkg + '/android.app.NativeActivity',
           '--es', 'pelican_args', "'" + intent_args + "'"]
(out / 'command.json').write_text(json.dumps(adb + command, indent=2))
(out / 'start.txt').write_bytes(run(*command))
started = time.monotonic()
captured = False
seen_pid = False
while time.monotonic() - started < 120:
    pid = run('shell', 'pidof', pkg, check=False).strip()
    seen_pid |= bool(pid)
    log = run('exec-out', 'run-as', pkg, 'cat', 'files/engine.log', check=False)
    # Capture while the app is drawing, without leaving the foreground.
    if not args.headless and not captured and b'Android first present:' in log:
        png = run('exec-out', 'screencap', '-p')
        if not png.startswith(b'\x89PNG\r\n\x1a\n'):
            raise RuntimeError('screencap did not return PNG')
        (out / 'screen.png').write_bytes(png)
        (out / 'capture-seconds.txt').write_text(str(time.monotonic() - started))
        captured = True
    if seen_pid and not pid:
        break
    time.sleep(0.25)
else:
    raise RuntimeError('120 seconds elapsed; app left running')
(out / 'thermal-after.txt').write_bytes(run('shell', 'dumpsys', 'thermalservice'))
for name in ('engine.log', 'stderr.log', 'engine-result.txt'):
    (out / name).write_bytes(run('exec-out', 'run-as', pkg, 'cat', 'files/' + name))
(out / 'logcat.txt').write_bytes(run('logcat', '-d', '-v', 'threadtime', 'pelican:V', '*:S'))
if args.headless:
    (out / 'headless.png').write_bytes(run('exec-out', 'run-as', pkg, 'cat', 'files/sa1_posegraph.png'))
elif not captured:
    raise RuntimeError('no first present observed before exit')
if (out / 'engine-result.txt').read_text().strip() != '0':
    raise RuntimeError('engine returned failure; see saved logs')
log_text = (out / 'engine.log').read_text(encoding='utf-8')
if 'available=true, enabled=true, synchronization=true' not in log_text or \
        'validation file callback enabled=true' not in log_text:
    raise RuntimeError('validation layer or file callback was not enabled')
totals = re.search(r'Android Vulkan validation totals: warnings=(\d+) errors=(\d+)', log_text)
if not totals or int(totals[2]) != 0:
    raise RuntimeError('validation totals missing or error count is nonzero')
print(str(out))
