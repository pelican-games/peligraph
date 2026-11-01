"""SA1f new CPU cases only, using local desktop Catch2 Debug libraries (no downloads).
Run from the repository root with the provisioned Python. --mutations verifies three
single mutations fail at runtime and restores/rebuilds each original source.
Artifacts: build-android/sa1f-cpu; existing Windows regression suites are separate.
"""
from pathlib import Path
import argparse, json, re, subprocess
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(); parser.add_argument('--mutations',action='store_true'); parser.add_argument('--expect-red',action='store_true'); args=parser.parse_args()
build=root/'build-android'; out=build/'sa1f-cpu'; out.mkdir(exist_ok=True)
deps=Path('<作業ディレクトリ>/rc2/build/_deps')
ninja=(build/'build.ninja').read_text(encoding='utf-8')
block=re.search(r'^build src/core/CMakeFiles/pelican_core.dir/os/inputstate.cpp.o:.*\n((?:  .*\n)+)',ninja,re.M).group(1)
includes=re.search(r'^  INCLUDES = (.*)$',block,re.M).group(1).replace('-I','/I')
defines=' '.join(x.replace('-D','/D',1) for x in re.search(r'^  DEFINES = (.*)$',block,re.M).group(1).split() if x.startswith(('-DPELICAN_WITH_','-DPELICAN_ENGINE_BUILD','-DVULKAN_HPP_')))
options=f'cl /nologo /std:c++20 /c /EHsc /utf-8 /MDd /DNOMINMAX /DWIN32 /D_WINDOWS /DPELICAN_SA1F_STANDALONE_CPU /I"{root}/test/android_stubs" {defines} {includes}'
options+=f' /I"{deps}/catch2-src/src" /I"{deps}/catch2-build/generated-includes"'
files=['src/core/os/inputstate.cpp','src/core/os/actionmap.cpp','src/core/os/inputsequence.cpp','src/core/ui/inputrouter.cpp','src/core/ui/types.cpp','src/core/ui/widgetarena.cpp','src/project/jsonrpc.cpp','src/platform/android/inputqueue.cpp','test/android_inputqueue_test.cpp','test/android_inputmap_test.cpp','test/touch_input_test.cpp']
def compile_files(selected):
    commands=['@echo off','set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"','call "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" >nul','if errorlevel 1 exit /b 1']
    for file in selected:
        commands += [f'{options} /Fo"{out/Path(file).with_suffix(".obj").name}" "{root/file}"','if errorlevel 1 exit /b 1']
    objects=' '.join(f'"{out/Path(file).with_suffix(".obj").name}"' for file in files)
    commands += [f'link /nologo /SUBSYSTEM:CONSOLE /OUT:"{out}/sa1f-fix-tests.exe" {objects} "{deps}/catch2-build/src/Catch2d.lib" "{deps}/catch2-build/src/Catch2Maind.lib"','exit /b %errorlevel%']
    batch=out/'compile.cmd'; batch.write_text('\n'.join(commands),encoding='utf-8')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=root,check=True)
def run(label,expect_red=False):
    result=subprocess.run([str(out/'sa1f-fix-tests.exe'),'[sa1f]'],cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/(label+'.log')).write_bytes(result.stdout)
    print(label, 'exit=',result.returncode,flush=True)
    if expect_red:
        if result.returncode==0: raise RuntimeError(label+' mutation survived')
    elif result.returncode: raise RuntimeError(label+' failed; see log')
    return result.returncode
compile_files(files); results={'baseline':run('fix-red' if args.expect_red else 'fix-verified',expect_red=args.expect_red)}
if args.expect_red: raise SystemExit(0)
mutations=[
 ('fix1-polling','src/platform/android/inputqueue.cpp','if (!enabled || !q.polling_enabled) {','if (false) {',['src/platform/android/inputqueue.cpp']),
 ('fix2-contact','src/core/os/actionmap.cpp','members[e.pointer_id] = contacting[e.pointer_id] &&','members[e.pointer_id] = (e.touch_phase == TouchPhase::down || e.touch_phase == TouchPhase::move) &&',['src/core/os/actionmap.cpp']),
 ('fix4-stylus','src/platform/android/inputqueue.cpp',' || source==AINPUT_SOURCE_STYLUS','',['src/platform/android/inputqueue.cpp'])]
if args.mutations:
    for label,file,before,after,selected in mutations:
        path=root/file; original=path.read_bytes(); text=original.decode('utf-8-sig')
        if text.count(before)!=1: raise RuntimeError('mutation anchor not unique: '+label)
        try:
            path.write_bytes(text.replace(before,after).encode('utf-8'))
            compile_files(selected)
            results[label]=run(label,expect_red=True)
        finally:
            path.write_bytes(original)
            compile_files(selected)
        run(label+'-restored')
(out/('results-mutations.json' if args.mutations else 'results.json')).write_text(json.dumps(results,indent=2),encoding='utf-8')
