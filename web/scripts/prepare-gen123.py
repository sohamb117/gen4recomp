#!/usr/bin/env python3
"""Package a pinned GPL engine, without its proprietary launcher or any ROMs."""
import hashlib, json, pathlib, shutil, subprocess, zipfile, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "integrations/gen123"))
from patches import patch
ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'build/gen1recomp-upstream'
LOVE = ROOT / 'build/lovejs-upstream'
OUT = ROOT / 'web/public/engines/gen123'
PIN = '340e2567ec93d8ac74659d7091ebbec8925d7295'
LOVE_PIN = 'c4f04e185033a7c9fbefa9be3bec88c41a90421b'
dependencies = [
    (SOURCE, PIN, 'https://github.com/bryanthaboi/gen1recomp.git'),
    (LOVE, LOVE_PIN, 'https://github.com/Davidobot/love.js.git'),
    (ROOT / 'build/bit-upstream', '30b6828b298b46411b548202d40524caba06a99c', 'https://github.com/davidm/lua-bit-numberlua.git'),
]
for path, pin, url in dependencies:
    if not path.exists():
        if '--fetch' not in sys.argv:
            raise SystemExit(f'Missing {path}. Rerun with --fetch to obtain pinned sources.')
        path.mkdir(parents=True)
        subprocess.run(['git', 'init', str(path)], check=True)
        subprocess.run(['git', '-C', str(path), 'fetch', '--depth=1', url, pin], check=True)
        subprocess.run(['git', '-C', str(path), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
    assert subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip() == pin
    assert not subprocess.check_output(['git', '-C', str(path), 'status', '--porcelain'], text=True).strip(), 'Source checkout must be clean'
EXCLUDED = {'src/import/LauncherView.lua', 'src/import/LauncherSettings.lua',
    'src/import/OnlinePanel.lua', 'src/import/CartLabelArt.lua', 'src/import/CartShape.lua',
    'src/mods/LauncherMods.lua', 'src/import/RomImporter.lua'}
OUT.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(OUT / 'game.love', 'w', zipfile.ZIP_DEFLATED) as package:
    def write_bytes(name, data):
        info = zipfile.ZipInfo(name, (2026, 10, 4, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        package.writestr(info, data)
    def write_file(path, name):
        write_bytes(name, path.read_bytes())
    for directory in ['src', 'data', 'assets', 'tools']:
        for path in sorted((SOURCE / directory).rglob('*')):
            if not path.is_file(): continue
            assert path.suffix.lower() not in {'.gb', '.gbc', '.gba', '.nds', '.sav', '.pws'}, 'Private game data in source package'
            rel = path.relative_to(SOURCE).as_posix()
            if rel in EXCLUDED or rel.startswith(('assets/launcher/', 'assets/labels/', 'assets/logo')): continue
            if directory == 'tools' and not path.name.startswith('rom_manifest'): continue
            if path.suffix == '.lua':
                write_bytes(rel, patch(rel, path.read_text()))
            else:
                write_file(path, rel)
    write_file(SOURCE / 'LICENSE.MD', 'LICENSE.MD')
    write_bytes('build-info.json', json.dumps({'gitCommitFull': PIN}))
    write_file(ROOT / 'build/bit-upstream/lmod/bit/numberlua.lua', 'bit_numberlua.lua')
    write_file(ROOT / 'build/bit-upstream/COPYRIGHT', 'BIT-LICENSE.txt')
    write_bytes('bit.lua', 'return require("bit_numberlua").bit\n')
    for name in ['main.lua', 'conf.lua', 'yield.lua']:
        write_file(ROOT / 'web/integrations/gen123' / name, name)
js = (LOVE / 'src/compat/love.js').read_text()
needle = 'Module["FS_createPath"]=FS.createPath;'
assert js.count(needle) == 2
bridge = 'Module["FS"]=FS;Module["resumeAudio"]=function(){var c=AL.currentCtx&&AL.currentCtx.audioCtx;if(c&&c.state==="suspended")c.resume().catch(function(){});};Module["audioState"]=function(){return AL.currentCtx&&AL.currentCtx.audioCtx.state;};'
js = js.replace(needle, bridge + needle, 1)
assert js.count('if(typeof ENVIRONMENT_IS_PTHREAD==="undefined"||!ENVIRONMENT_IS_PTHREAD){Module.addRunDependency("IDBFS_sync")') == 1
js = js.replace('if(typeof ENVIRONMENT_IS_PTHREAD==="undefined"||!ENVIRONMENT_IS_PTHREAD){Module.addRunDependency("IDBFS_sync")', 'if(false){Module.addRunDependency("IDBFS_sync")')
(OUT / 'love.js').write_text(js)
shutil.copyfile(LOVE / 'src/compat/love.wasm', OUT / 'love.wasm')
shutil.copyfile(LOVE / 'LICENSE', OUT / 'LOVE-LICENSE.txt')
shutil.copyfile(SOURCE / 'LICENSE.MD', OUT / 'GEN123-LICENSE.txt')
for name in ['player.html', 'player.js', 'cache.js']:
    shutil.copyfile(ROOT / 'web/integrations/gen123' / name, OUT / name)
files = {p.name: {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
         for p in OUT.iterdir() if p.is_file() and p.name != 'manifest.json'}
(OUT / 'manifest.json').write_text(json.dumps({'version': 1, 'source': PIN, 'love': LOVE_PIN, 'cacheVersion': PIN + ':' + hashlib.sha256((ROOT / 'web/integrations/gen123/patches.py').read_bytes()).hexdigest(), 'files': files}, indent=2)+'\n')
print('Prepared Gen 1–3 runtime:', OUT)
