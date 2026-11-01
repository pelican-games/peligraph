# Creates an APK-only fixture. Never changes the demo project or desktop defaults.
import json, shutil
from pathlib import Path
root=Path(__file__).resolve().parents[2]
target=root/'build-android/sa1f-project'
shutil.copytree(root/'projects/posegraph_demo',target,ignore=shutil.ignore_patterns('.pelican'),dirs_exist_ok=True)
def write(name,data): (target/name).write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
project=json.loads((target/'project.json').read_text(encoding='utf-8'))
project['basic_config'].update(input_actions_json='sa1f-actions.json',input_profiles={'sa1f':'sa1f-profile.json'},input_profile='sa1f',ui_config_json='sa1f-ui.json')
write('project.json',project)
names=['kbd','pad','region','tap']
write('sa1f-actions.json',{'schema':'pelican.input_actions','version':1,'action_sets':[{'name':'sa1f','actions':[{'name':'sa1f_'+n,'type':'button'} for n in names]}]})
bindings=[{'action':'sa1f_kbd','binding':'kbd:space'},{'action':'sa1f_pad','binding':'pad:a'},
          {'action':'sa1f_region','binding':'touch:region','region':[0.0,0.25,0.5,1.0]},
          {'action':'sa1f_tap','binding':'touch:tap','region':[0.5,0.25,1.0,1.0]}]
write('sa1f-profile.json',{'schema':'pelican.input_profile','version':1,'name':'sa1f','bindings':bindings})
write('sa1f-ui.json',{'schema':'pelican.ui','version':1,'key':'sa1f_ui','root':{'id':'root','type':'panel','children':[
    {'id':'button','type':'button','color':[64,128,255,255],'layout':{'x':{'mode':'fixed','value':300},'y':{'mode':'fixed','value':100},'offsets':[50,160,0,0]}}
]}})
print(target)
