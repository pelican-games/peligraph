#!/usr/bin/env python3
"""Capture a finite Windows windowed run through the existing swapchain RPC."""
import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
exe = out / 'player' / 'pelican_player.exe'
exe.parent.mkdir()
shutil.copy2(args.exe, exe)
project = out / 'project'
shutil.copytree(Path(__file__).resolve().parents[2] / 'projects/posegraph_demo',
                project, ignore=shutil.ignore_patterns('.pelican'))
document = json.loads((project / 'project.json').read_text(encoding='utf-8'))
document['basic_config']['window_size'] = {'width': 2688, 'height': 1216}
(project / 'project.json').write_text(json.dumps(document, indent=2), encoding='utf-8')
command = [str(exe), '--rpc', '--project', str(project), '--user-dir', str(out / 'user'),
           '--frames', '300', '--vulkan-validation', 'on', '--log-file', str(out / 'engine.log')]
(out / 'command.json').write_text(json.dumps({
    'command': command, 'source': str(args.exe.resolve()),
    'sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest()}, indent=2))
with (out / 'stderr.log').open('wb') as stderr, (out / 'stdout-other.log').open('w', encoding='utf-8') as other:
    process = subprocess.Popen(command, cwd=out, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=stderr, text=True, encoding='utf-8')

    def call(number, method, params):
        request = {'jsonrpc': '2.0', 'id': number, 'method': method, 'params': params}
        process.stdin.write(json.dumps(request) + '\n')
        process.stdin.flush()
        # The existing Windows loader/VVL can print diagnostics on stdout.
        # Preserve them separately; never confuse them with an RPC response.
        while line := process.stdout.readline():
            try:
                reply = json.loads(line)
            except json.JSONDecodeError:
                other.write(line)
                continue
            if not isinstance(reply, dict) or reply.get('id') != number:
                other.write(line)
                continue
            (out / f'response-{number}.json').write_text(line, encoding='utf-8')
            if 'error' in reply:
                raise RuntimeError(reply)
            return reply['result']
        raise RuntimeError('player exited before replying')

    try:
        call(1, 'get_status', {})
        time.sleep(0.5)
        call(2, 'capture', {'path': str(out / 'windowed.png')})
        call(3, 'get_status', {})
    finally:
        process.stdin.close()
        other.write(process.stdout.read())
        # --frames ends the process. No terminate/kill, including on failure.
        code = process.wait(timeout=30)
        (out / 'exit.txt').write_text(str(code))
print(str(out))
