#!/usr/bin/env python3
"""SA1e device evidence; fixed serial, --frames 0, normal exit through BACK.

Run sleep last: a keyguard after WAKEUP stops this script without unlocking.
All host artifacts default to the current worktree's build-android directory.
"""
import argparse
import json
import re
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser()
mode = parser.add_mutually_exclusive_group(required=True)
mode.add_argument('--case', choices=['home', 'startup', 'busy', 'back', 'fault', 'timeout', 'sleep', 'recents'])
mode.add_argument('--manual-sleep-cycles', type=int, metavar='N')
parser.add_argument('--manual-timeout', type=float, default=600, metavar='SECONDS')
parser.add_argument('--label', required=True)
parser.add_argument('--results', type=Path, default=Path('build-android/sa1e-results'))
args = parser.parse_args()
if args.manual_sleep_cycles is not None and args.manual_sleep_cycles < 1:
    parser.error('--manual-sleep-cycles must be positive')
if args.manual_timeout <= 0:
    parser.error('--manual-timeout must be positive')
out = args.results / args.label
out.mkdir(parents=True, exist_ok=False)
adb = ['<作業ディレクトリ>/android/platform-tools/adb.exe', '-s', '<serial>']
pkg = 'org.pelicanengine.sa1b'
component = pkg + '/android.app.NativeActivity'
events = []
entry = time.monotonic()
manual_deadline = None

def run(*command, check=True):
    result = subprocess.run(adb + list(command), capture_output=True)
    events.append({'ms': round((time.monotonic()-entry)*1000, 3), 'argv': adb+list(command), 'returncode': result.returncode})
    (out/'commands.json').write_text(json.dumps(events, indent=2), encoding='utf-8')
    if check and result.returncode:
        raise RuntimeError(f'{command}: {result.stderr.decode(errors="replace")}')
    return result.stdout

def read(name):
    return run('exec-out', 'run-as', pkg, 'cat', 'files/'+name, check=False)

def wait_for(predicate, message, seconds=30):
    deadline = time.monotonic()+seconds
    if manual_deadline is not None:
        deadline = min(deadline, manual_deadline)
    while time.monotonic() < deadline:
        if predicate(): return
        time.sleep(.2)
    snapshot('failure')
    raise RuntimeError('deadline: '+message)

def snapshot(label, screen=False):
    for name in ('engine.log', 'pump.log', 'stderr.log', 'engine-result.txt'):
        (out/(label+'-'+name)).write_bytes(read(name))
    (out/(label+'-window.txt')).write_bytes(run('shell', 'dumpsys', 'window'))
    if screen: (out/(label+'.png')).write_bytes(run('exec-out', 'screencap', '-p'))

def start():
    return run('shell', 'am', 'start', '-n', component)

def presents():
    return len(re.findall(rb'Android surface first present:', read('engine.log')))

def accepted_count(engine):
    counts = re.findall(rb'Android surface present: native_revision=\d+ accepted_count=(\d+)', engine)
    return int(counts[-1]) if counts else 0


def wait_restored(label, before):
    def restored():
        pump, engine = read('pump.log'), read('engine.log')
        if b'surface release TIMEOUT' in pump:
            raise RuntimeError('surface release TIMEOUT')
        if pump.count(b'TERM_WINDOW') != pump.count(b'surface release acked'):
            return False
        revisions = re.findall(rb'native window published revision=(\d+)', pump)
        if not revisions or pump.rfind(b'TERM_WINDOW') > pump.rfind(b'native window published revision='):
            return False
        revision = revisions[-1]
        # A later counter sample for the current revision also proves rendering
        # when sleep did not cause TERM_WINDOW / a new surface epoch.
        counts = re.findall(rb'Android surface present: native_revision=' + revision + rb' accepted_count=(\d+)', engine)
        return bool(counts and int(counts[-1]) > before)
    wait_for(restored, label+' current revision accepted present')
    snapshot(label+'-restored', True)
    print(label+': 復帰後の present を確認', flush=True)


def verify_final():
    engine, pump = read('engine.log'), read('pump.log')
    # The engine writer emits the result with one newline. No stale result
    # survives start(); missing, nonzero and extra content all fail.
    if read('engine-result.txt') != b'0\n':
        raise RuntimeError('engine-result.txt must exist and contain exactly 0 with its writer newline')
    if b'DESTROY engine deadline' in pump:
        raise RuntimeError('DESTROY forced _exit is a failure')
    validation = re.search(rb'Vulkan validation: [^\n]*enabled=true', engine)
    if not validation or not re.search(rb'Android Vulkan validation totals: warnings=\d+ errors=0', engine):
        raise RuntimeError('validation must be enabled with final error total 0; evidence retained')
    if re.search(rb'NATIVE_WINDOW_IN_USE|ErrorNativeWindowInUseKHR|native-window binding invariant failed', engine):
        raise RuntimeError('native window binding error; evidence retained')
    if b'surface release TIMEOUT' in pump or pump.count(b'TERM_WINDOW') != pump.count(b'surface release acked'):
        raise RuntimeError('every TERM_WINDOW must have an ack; evidence retained')
    epochs = list(dict.fromkeys(map(int, re.findall(rb'swapchain epoch (?:initialized|published):[^\n]*surface_epoch=(\d+)', engine))))
    if epochs != list(range(1, len(epochs)+1)):
        raise RuntimeError('surface epoch sequence is not consecutive: '+str(epochs))

def go_home(label):
    before = read('pump.log').count(b'surface release acked')
    run('shell', 'input', 'keyevent', 'KEYCODE_HOME')
    wait_for(lambda: read('pump.log').count(b'surface release acked') > before, 'HOME release ack')
    snapshot(label+'-home')
    prior = accepted_count(read('engine.log'))
    start()
    wait_restored(label, prior)


def manual_sleep_cycles():
    global manual_deadline
    manual_deadline = time.monotonic() + args.manual_timeout
    for i in range(1, args.manual_sleep_cycles + 1):
        label = f'manual-{i}'
        offset = len(read('pump.log'))
        print(f'{i}/{args.manual_sleep_cycles}: 電源で消灯 → 点灯 → 手でロック解除 → app に戻ってください', flush=True)
        wait_for(lambda: b' PAUSE' in read('pump.log')[offset:], label+' PAUSE', args.manual_timeout)
        print(f'{i}/{args.manual_sleep_cycles}: PAUSE を検出、復帰を待っています', flush=True)
        wait_for(lambda: b' RESUME' in read('pump.log')[offset:].split(b' PAUSE', 1)[-1],
                 label+' RESUME', args.manual_timeout)
        prior = accepted_count(read('engine.log'))
        wait_restored(label, prior)
        cycle = read('pump.log')[offset:]
        (out/(label+'-commands.log')).write_bytes(cycle)
        terms = cycle.count(b'TERM_WINDOW')
        print(f'{i}/{args.manual_sleep_cycles}: 完了 TERM_WINDOW={terms}, ack={cycle.count(b"surface release acked")}, INIT_WINDOW={cycle.count(b"INIT_WINDOW")}', flush=True)
    manual_deadline = None
    print('全回完了、BACK を送って正常終了を確認します', flush=True)

def finish():
    before = time.monotonic()
    run('shell', 'input', 'keyevent', 'KEYCODE_BACK')
    wait_for(lambda: not run('shell', 'pidof', pkg, check=False).strip(), 'BACK process exit', 8)
    (out/'exit-ms.txt').write_text(str((time.monotonic()-before)*1000))
    snapshot('final')
    (out/'pipeline_cache.bin').write_bytes(read('cache/pipeline_cache.bin'))
    (out/'files.txt').write_bytes(run('shell', 'run-as', pkg, 'ls', '-laR', 'files'))
    verify_final()

window = run('shell', 'dumpsys', 'window')
(out/'initial-window.txt').write_bytes(window)
if re.search(rb'isKeyguardShowing=true|mShowingLockscreen=true|keyguardShowing=true', window):
    (out/'USER_ACTION_REQUIRED.txt').write_text('利用者の操作が要る。ロック画面を検出したため停止。', encoding='utf-8')
    raise SystemExit(3)
run('shell', 'am', 'force-stop', pkg)
run('shell', 'run-as', pkg, 'rm', '-f', 'files/engine.log', 'files/pump.log', 'files/engine-result.txt')
if args.case == 'startup':
    run('shell', 'run-as', pkg, 'rm', '-rf', 'files/project/.pelican/shader_cache')
extra = '--frames 0 --timing-frames 30'
if args.case == 'busy': extra += ' --sa1-test-build-delay'
if args.case == 'fault': extra += ' --sa1-test-no-maintenance'
if args.case == 'timeout': extra += ' --sa1-test-short-acquire'
(out/'start.txt').write_bytes(run('shell', 'am', 'start', '-n', component, '--es', 'pelican_args', '"'+extra+'"'))
if args.case in ('startup', 'busy'):
    marker = b'Android shader compile begin:' if args.case == 'startup' else b'Android build worker submitted: kind=swapchain'
    wait_for(lambda: marker in read('engine.log'), 'startup marker')
    (out/'trigger-engine.log').write_bytes(read('engine.log'))
    go_home('1')
else:
    wait_for(lambda: presents() > 0, 'first present')
    snapshot('initial', True)
    if args.case == 'home':
        for i in range(1, 4): go_home(str(i))
    elif args.case == 'fault':
        run('shell', 'input', 'keyevent', 'KEYCODE_HOME')
        wait_for(lambda: not run('shell', 'pidof', pkg, check=False).strip(), 'fault finish', 8)
        snapshot('final')
        verify_final()
        if b'presentation_completion_unavailable' not in read('engine.log'):
            raise RuntimeError('fault did not take the intended finish path')
        raise SystemExit(0)
    elif args.case == 'timeout':
        wait_for(lambda: b'result=Timeout' in read('engine.log'), 'real finite acquire VK_TIMEOUT')
        time.sleep(.5)
        snapshot('timeout', True)
    elif args.case == 'recents':
        run('shell', 'input', 'keyevent', 'KEYCODE_APP_SWITCH')
        time.sleep(1)
        snapshot('recents', True)
        # Leave the task visible for inspecting the device's split-screen UI.
        raise SystemExit(0)
    elif args.case == 'sleep':
        for i in range(1, 4):
            run('shell', 'input', 'keyevent', 'KEYCODE_SLEEP')
            time.sleep(1)
            snapshot(str(i)+'-sleep')
            run('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')
            time.sleep(.5)
            window = run('shell', 'dumpsys', 'window')
            (out/(str(i)+'-wake-window.txt')).write_bytes(window)
            if re.search(rb'isKeyguardShowing=true|mShowingLockscreen=true|keyguardShowing=true', window):
                snapshot(str(i)+'-locked', True)
                (out/'USER_ACTION_REQUIRED.txt').write_text('利用者の操作が要る。点灯後のロック画面を検出したため停止。PIN等は入力していない。', encoding='utf-8')
                raise SystemExit(3)
            start()
            prior = accepted_count(read('engine.log'))
            wait_restored(str(i), prior)
    elif args.manual_sleep_cycles is not None:
        manual_sleep_cycles()
finish()
print(str(out))
