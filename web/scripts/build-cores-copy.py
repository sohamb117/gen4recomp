#!/usr/bin/env python3
"""Build selected guest cores in a disposable copy; source trees stay read-only."""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path
GAMES=('diamond','pearl','platinum','heartgold','soulsilver','ruby','sapphire','emerald','black','white')
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('games',nargs='*',choices=GAMES,default=list(GAMES))
p.add_argument('--source-root',type=Path)
p.add_argument('--resume-snapshot',type=Path)
p.add_argument('--hg-inputs',type=Path)
p.add_argument('--bw-inputs',type=Path)
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
source=(a.source_root or root).resolve()
commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()
if subprocess.check_output(['git','status','--porcelain','--untracked-files=no','--','games','core','tools'],cwd=source,text=True).strip():
    raise SystemExit('Source input has tracked changes; use a committed source snapshot')
build=root/'build';build.mkdir(exist_ok=True)
snapshot=a.resume_snapshot.resolve() if a.resume_snapshot else Path(tempfile.mkdtemp(prefix='web-source-',dir=build))
if snapshot.parent != build.resolve():raise SystemExit('Snapshot must be directly under this worktree build directory')
def clone(src,dst):
    dst.parent.mkdir(parents=True,exist_ok=True)
    if sys.platform=='darwin':subprocess.run(['cp','-cRp',str(src),str(dst)],check=True)
    else:shutil.copytree(src,dst,symlinks=True) if src.is_dir() else shutil.copy2(src,dst)
if not a.resume_snapshot:
    for name in ('games','core','tools'):clone(source/name,snapshot/name)
    (snapshot/'.cache').mkdir()
    (snapshot/'.cache/toolchains').symlink_to((source/'.cache/toolchains').resolve(),target_is_directory=True)
    if any(g in a.games for g in ('ruby','sapphire','emerald')):clone(source/'.cache/gba',snapshot/'.cache/gba')
    if a.hg_inputs:
        for name in ('build','arm9/build'):
            if not (a.hg_inputs/'games/heartgold'/name).exists():continue
            target=snapshot/'games/heartgold'/name
            if target.exists():shutil.rmtree(target)
            clone(a.hg_inputs/'games/heartgold'/name,target)
    if a.hg_inputs:
        # ROM compilation also generates ignored SDK headers outside build/.
        generated=subprocess.check_output(['git','ls-files','--others','--ignored','--exclude-standard','-z','games/heartgold'],cwd=a.hg_inputs).decode().split('\0')
        for name in generated:
            if not name or '/build/' in name:continue
            src=a.hg_inputs/name;dst=snapshot/name
            if src.is_file() and not dst.exists():
                dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
    if a.bw_inputs:clone(a.bw_inputs,snapshot/'roms')
    # Seeded depfiles must never make an upstream worktree a build target.
    def remap(m):
        path=m.group(0); marker=re.search(r'/(?:games|core|tools|\.cache)/',path)
        return str(snapshot)+path[marker.start():] if marker else path
    for folder in ('diamond','platinum','heartgold','ndsrec'):
        for dep in (snapshot/f'games/{folder}/build').rglob('*.d'):
            dep.write_text(re.sub(r'(?<!\S)/[^\s\\:]+',remap,dep.read_text()))
output=build/'web-guest';output.mkdir(exist_ok=True)
roms=build/'web-roms';roms.mkdir(exist_ok=True)
print(f'Source snapshot: {snapshot} ({commit})',flush=True)
(output/'snapshot.json').write_text(json.dumps({'snapshot':str(snapshot),'sourceCommit':commit},indent=2)+'\n')
failed=[]
for game in a.games:
    jobs=os.environ.get('NP_BUILD_JOBS','4')
    if game in ('ruby','sapphire','emerald'):
        folder='emerald' if game=='emerald' else 'ruby'
        command=[sys.executable,str(snapshot/'games/gba-common/tools/gbabuild.py'),game,'-j',jobs]
        rom=snapshot/f'.cache/gba/{"pokeemerald" if game=="emerald" else "pokeruby"}/poke{game}.gba'
        module=snapshot/f'games/{folder}/build/pc-wasm/poke{game}.wasm'
    else:
        folder='platinum' if game=='platinum' else 'diamond' if game in ('diamond','pearl') else 'heartgold' if game in ('heartgold','soulsilver') else 'ndsrec'
        command=['make','-f','pc/Makefile.wasm',f'-j{jobs}']
        if folder in ('diamond','heartgold'):command.append(f'GAME_VERSION={game.upper()}')
        if folder=='ndsrec':
            matches=list((snapshot/'roms').glob(f'*{game.title()} Version*.nds'))
            if len(matches)!=1:raise SystemExit(f'Expected one {game} input ROM')
            rom=snapshot/f'roms/{game}.nds'
            if not rom.exists():shutil.copy2(matches[0],rom)
            command += [f'VER={game}',f'ROM={rom}']
            module=snapshot/f'games/ndsrec/build/pc-wasm/ndsrec-{game}.wasm'
        else:
            rom=snapshot/f'games/{folder}/build/{"rom" if game=="platinum" else game+".us"}/poke{game}.us.nds'
            module=snapshot/f'games/{folder}/build/pc-wasm/poke{game}.wasm'
    log=snapshot/f'{game}-build.log'
    print(f'Building {game}; log: {log}',flush=True)
    with log.open('w') as stream:
        result=subprocess.run(command,cwd=snapshot/f'games/{folder}',stdout=stream,stderr=subprocess.STDOUT)
    if result.returncode:
        print(f'{game}: build failed\n'+log.read_text()[-2500:],flush=True);failed.append(game);continue
    target=output/f'{game}.wasm';shutil.copy2(module,target)
    shutil.copy2(rom,roms/f'{game}{rom.suffix}')
    (output/f'{game}-source.json').write_text(json.dumps({'snapshot':str(snapshot.relative_to(root)),'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'sourceCommit':commit},indent=2)+'\n')
    print(f'{game}: built {target}',flush=True)
if failed:raise SystemExit('Failed: '+', '.join(failed))
