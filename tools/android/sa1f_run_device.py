"""SA1f device evidence. Only fixed serial, APK install, app lifecycle and app data writes.
Build fixture via sa1f_fixture.py, package with PROJECT=build-android/sa1f-project,
then pass that APK. No logcat or device-setting changes. Leaves failed app running.
"""
from pathlib import Path
import argparse, json, re, struct, subprocess, time
def evaluate_steps(steps, coordinates):
    checks={}
    for label,(x,y) in coordinates.items():
        log=steps.get(label,b'')
        pointers=list(re.finditer(rb'SA1f pointer\(\) x=([-\d.e+]+) y=([-\d.e+]+) inside=(true|false)',log))
        checks[label+'_pointer_xy']=any(float(m[1])==x and float(m[2])==y and m[3]==b'true' for m in pointers)
        edges=list(re.finditer(rb'SA1f UI pointer id=(\d+) event=pointer_(down|up) x=(-?\d+) y=(-?\d+) target=(true|false) effects=(\d+)',log))
        active=set(); paired=bool(edges); last_up=-1
        for m in edges:
            id=int(m[1]); down=m[2]==b'down'
            paired &= (int(m[3]),int(m[4]))==(x,y)
            if down:
                paired &= id not in active; active.add(id)
            else:
                paired &= id in active; active.discard(id); last_up=m.end()
            if label.startswith('ui-'):
                # Down's one effect is Capture; stationary Up has ReleaseCapture+Click.
                paired &= m[5]==b'true' and int(m[6])==(1 if down else 2)
        checks[label+'_down_up_release']=paired and not active
        checks[label+'_pointer_outside']=bool(pointers) and pointers[-1][3]==b'false' and pointers[-1].start()>last_up>=0
    return checks

def run(*argv,check=True):
    result=subprocess.run(adb+list(map(str,argv)),capture_output=True,timeout=60)
    commands.append({'seconds':round(time.monotonic()-started,3),'argv':adb+list(map(str,argv)),'returncode':result.returncode})
    (out/'commands.json').write_text(json.dumps(commands,indent=2),encoding='utf-8')
    if check and result.returncode: raise RuntimeError(result.stderr.decode(errors='replace'))
    return result.stdout
def read(name): return run('exec-out','run-as',pkg,'cat','files/'+name,check=False)
def save(label):
    for name in ('engine.log','pump.log','stderr.log','engine-result.txt'): (out/(label+'-'+name)).write_bytes(read(name))
def wait_for(predicate,label,seconds=15):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate(): return
        time.sleep(.25)
    save('failure'); raise RuntimeError('timeout: '+label)
def action(name): return b'SA1f action=sa1f_'+name.encode()
def check_step(label,argv,needle,xy=None):
    before=len(read('engine.log')); run('shell','input',*map(str,argv))
    wait_for(lambda: needle in read('engine.log')[before:],label)
    if xy and 'tap' in argv:
        # An instantaneous tap may start/end in one engine frame. Keep the tap
        # check and add a short stationary contact to observe public pointer().
        source=['stylus'] if argv[0]=='stylus' else []
        x,y=xy; run('shell','input',*source,'swipe',x,y,x,y,120)
    if xy:
        wait_for(lambda: all(evaluate_steps({label:read('engine.log')[before:]},{label:xy}).values()),label+' complete touch')
    time.sleep(.35); save(label); print(label+': observed',flush=True)
    return read('engine.log')[before:]
def main():
    global args, out, adb, pkg, commands, started
    p=argparse.ArgumentParser(); p.add_argument('--apk',type=Path,required=True); p.add_argument('--label',required=True); args=p.parse_args()
    out=Path('build-android/sa1f-results')/args.label; out.mkdir(parents=True,exist_ok=False)
    adb=['<作業ディレクトリ>/android/platform-tools/adb.exe','-s','<serial>']; pkg='org.pelicanengine.sa1b'
    commands=[]; started=time.monotonic()
    try:
        window=run('shell','dumpsys','window'); (out/'before-window.txt').write_bytes(window)
        if re.search(rb'isKeyguardShowing=true|mShowingLockscreen=true|keyguardShowing=true',window): raise RuntimeError('device locked; no unlock attempted')
        run('install','-r',str(args.apk)); run('shell','am','force-stop',pkg)
        run('shell','run-as',pkg,'rm','-f','files/engine.log','files/pump.log','files/engine-result.txt')
        run('shell','am','start','-n',pkg+'/android.app.NativeActivity','--es','pelican_args','"--frames 0 --sa1-test-input --timing-frames 120"')
        wait_for(lambda: b'Android first present:' in read('engine.log'),'first accepted present',180)
        save('startup')
        screenshot=run('exec-out','screencap','-p'); (out/'startup.png').write_bytes(screenshot)
        width,height=struct.unpack('>II',screenshot[16:24]); print(f'screen={width}x{height}',flush=True)
        if width<height: raise RuntimeError('expected landscape activity')
        check_step('keyboard-space',['keyboard','keyevent','KEYCODE_SPACE'],action('kbd')+b' pressed=true')
        check_step('gamepad-a',['gamepad','keyevent','--longpress','KEYCODE_BUTTON_A'],action('pad')+b' pressed=true')
        xleft,xright,y=int(width*.25),int(width*.75),int(height*.65)
        coordinates={'touch-tap':(xright,y),'touch-region':(xleft,y),'ui-tap':(200,210),'ui-hold':(200,210)}
        steps={}
        steps['touch-tap']=check_step('touch-tap',['tap',xright,y],action('tap')+b' pressed=true held=false released=true',coordinates['touch-tap'])
        steps['touch-region']=check_step('touch-region',['swipe',xleft,y,xleft,y,800],action('region')+b' pressed=true held=true',coordinates['touch-region'])
        steps['ui-tap']=check_step('ui-tap',['tap',200,210],b'SA1f UI pointer id=1 event=pointer_down',coordinates['ui-tap'])
        steps['ui-hold']=check_step('ui-hold',['swipe',200,210,200,210,800],b'SA1f UI pointer id=1 event=pointer_down',coordinates['ui-hold'])
        coordinates['stylus-tap']=(xright,y)
        steps['stylus-tap']=check_step('stylus-tap',['stylus','tap',xright,y],action('tap')+b' pressed=true held=false released=true',coordinates['stylus-tap'])
        for label,log in steps.items(): (out/(label+'-interval.log')).write_bytes(log)
        run('shell','input','keyevent','KEYCODE_BACK')
        wait_for(lambda: read('engine-result.txt')==b'0\n','engine normal result',30)
        wait_for(lambda: b'DESTROY engine done' in read('pump.log'),'DESTROY complete',15)
        save('final'); engine,pump=read('engine.log'),read('pump.log')
        checks={
            'keyboard_action': action('kbd')+b' pressed=true' in engine,
            'gamepad_action': action('pad')+b' pressed=true' in engine and action('pad')+b' pressed=false held=false released=true' in engine,
            'tap_pulse': action('tap')+b' pressed=true held=false released=true' in engine,
            'region_pressed_held': action('region')+b' pressed=true held=true' in engine,
            'region_released': action('region')+b' pressed=false held=false released=true' in engine,
            **evaluate_steps(steps,coordinates),
            'validation_enabled': bool(re.search(rb'Vulkan validation: [^\n]*enabled=true',engine)),
            'validation_zero': bool(re.search(rb'Android Vulkan validation totals: warnings=\d+ errors=0',engine)),
            'destroy': b'DESTROY engine done' in pump and b'DESTROY engine deadline' not in pump,
            'normal_result': read('engine-result.txt')==b'0\n',
        }
        (out/'checks.json').write_text(json.dumps(checks,indent=2),encoding='utf-8')
        if not all(checks.values()): raise RuntimeError('failed checks: '+str([k for k,v in checks.items() if not v]))
        print('all device checks passed',flush=True)
    except BaseException:
        save('failure')
        raise

if __name__ == "__main__": main()
