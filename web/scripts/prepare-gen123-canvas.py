#!/usr/bin/env python3
"""Prepare an isolated experimental engine; never overwrite the production bundle."""
from pathlib import Path
import shutil, zipfile, re, json, subprocess
root=Path(__file__).resolve().parents[2]
source=root/'web/public/engines/gen123'
output=root/'build/gen123-canvas'
adapter=root/'web/integrations/gen123-canvas'
output.mkdir(parents=True,exist_ok=True)
if not (adapter/'node_modules/glsl-transpiler/index.js').exists():
    raise SystemExit('Run npm ci --prefix web/integrations/gen123-canvas first')
shutil.copyfile(adapter/'GLSL-TRANSPILER-NOTICE.txt',output/'GLSL-TRANSPILER-NOTICE.txt')
for name in ['love.js','love.wasm','cache.js','GEN123-LICENSE.txt','LOVE-LICENSE.txt','manifest.json']:
    shutil.copyfile(source/name,output/name)
with zipfile.ZipFile(source/'game.love') as src:
    shaders=[]
    for name in src.namelist():
        if not name.endswith('.lua'): continue
        for match in re.finditer(r'\[(=*)\[(.*?)\]\1\]',src.read(name).decode(),re.S):
            if 'vec4 effect(' in match[2]: shaders.append({'source':match[2],'path':name})
    (output/'shader-sources.json').write_text(json.dumps(shaders))
subprocess.run(['node',str(root/'web/scripts/compile-canvas-shaders.mjs')],check=True)
with zipfile.ZipFile(source/'game.love') as src,zipfile.ZipFile(output/'game.love','w',zipfile.ZIP_DEFLATED) as out:
    for item in src.infolist():
        data=src.read(item.filename)
        if item.filename=='conf.lua':
            data=data.decode().replace("  t.window.title = 'pokeweb'", "  t.window.title = 'pokeweb'")
            data=data.replace('  t.modules.thread = false','  t.window = false\n  t.modules.window = false\n  t.modules.graphics = false\n  t.modules.thread = false').encode()
        elif item.filename=='main.lua':
            data=b"require('canvas_graphics')\n"+data
            data=data.replace(b"if game and not paused then game:update(math.min(dt, 0.1)) end", b"""if game and not paused then game:update(math.min(dt, 0.1)) end
  _canvasDebug=(_canvasDebug or 0)+1
  if _canvasDebug%120==0 then
    local top=game and game.stack and game.stack:top()
    print('CANVAS_STATE:'..Json.encode({dt=dt,paused=paused,screen=top and top.screenId,phase=top and top.phase,timer=top and top.timer}))
  end""")
        out.writestr(item,data)
    out.writestr('canvas_shaders.lua',(output/'canvas_shaders.lua').read_bytes())
    out.writestr('canvas_graphics.lua',(adapter/'graphics.lua').read_bytes())
shutil.copyfile(adapter/'renderer.js',output/'renderer.js')
shutil.copyfile(adapter/'gba.js',output/'gba.js')
player=(source/'player.js').read_text()
begin=player.index('    // LÖVE otherwise')
end=player.index('    const manifestResponse',begin)
player=player[:begin]+player[end:]
# Experimental asset hash differs; update the local-only manifest below.
player=player.replace('  started = false,','  started = false,\n  canvasRenderer,')
player=player.replace('      print(line) {','''      print(line) {
        if (line.startsWith("POKEWEB_CANVAS:")) {
          canvasRenderer ||= new CanvasLoveRenderer(document.getElementById("canvas"), instance.FS);
          window.canvasRenderer = canvasRenderer;
          canvasRenderer.batch(JSON.parse(line.slice(15)));
          return;
        }''')
(output/'player.js').write_text(player)
html=(source/'player.html').read_text().replace('<script src="player.js">','<script src="shaders.js"></script><script src="gba.js"></script><script src="renderer.js"></script><script src="player.js">')
(output/'player.html').write_text(html)
import hashlib,json
manifest=json.loads((output/'manifest.json').read_text())
for name in ['game.love','player.js','player.html','renderer.js','gba.js','shaders.js','GLSL-TRANSPILER-NOTICE.txt']:
    data=(output/name).read_bytes();manifest['files'][name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
(output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(output)
