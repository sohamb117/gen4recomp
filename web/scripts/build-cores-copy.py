#!/usr/bin/env python3
"""Build selected guest cores in a disposable source copy, never in the input trees."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('games', nargs='*', choices=('diamond', 'pearl', 'platinum'), default=['diamond', 'pearl', 'platinum'])
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
build = root / 'build'
build.mkdir(exist_ok=True)
snapshot = Path(tempfile.mkdtemp(prefix='web-source-', dir=build))
(snapshot / 'games').mkdir()
(snapshot / '.cache').mkdir()
for name in ('games/diamond', 'games/platinum', 'core', 'tools'):
    source, target = root / name, snapshot / name
    if sys.platform == 'darwin':
        subprocess.run(['cp', '-cRp', str(source), str(target)], check=True)
    else:
        shutil.copytree(source, target, symlinks=True)
(snapshot / '.cache/toolchains').symlink_to(root / '.cache/toolchains', target_is_directory=True)
# Seeded compiler dependencies can refer to other worktrees. Rewrite each
# nativeplat root in their absolute paths so make never builds an input tree.
import re
def remap_path(match):
    path = match.group(0)
    marker = re.search(r"/(?:games|core|tools|\.cache)/", path)
    return str(snapshot) + path[marker.start():] if marker else path

for folder in ('diamond', 'platinum'):
    for dep in (snapshot / f'games/{folder}/build/pc-wasm').rglob('*.d'):
        text = re.sub(r'/[^\s\\:]+', remap_path, dep.read_text())
        dep.write_text(text)
output = build / 'web-guest'
output.mkdir(exist_ok=True)
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
print(f'Source snapshot: {snapshot}', flush=True)
for game in args.games:
    folder = 'platinum' if game == 'platinum' else 'diamond'
    log = snapshot / f'{game}-build.log'
    command = ['make', '-f', 'pc/Makefile.wasm', f'-j{os.environ.get("NP_BUILD_JOBS", "4")}']
    if game != 'platinum':
        command.append(f'GAME_VERSION={game.upper()}')
    print(f'Building {game}; log: {log}', flush=True)
    with log.open('w') as stream:
        result = subprocess.run([str(snapshot / 'tools/heavy.sh'), *command], cwd=snapshot / f'games/{folder}', stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text()[-8000:], file=sys.stderr)
        sys.exit(result.returncode)
    source = snapshot / f'games/{folder}/build/pc-wasm/poke{game}.wasm'
    shutil.copyfile(source, output / f'{game}.wasm.tmp')
    (output / f'{game}.wasm.tmp').replace(output / f'{game}.wasm')
    (output / f'{game}-source.json').write_text(json.dumps({
        'snapshot': str(snapshot.relative_to(root)),
        'sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'sourceCommit': commit,
    }, indent=2) + '\n')
    print(f'{game}: built {output / f"{game}.wasm"}', flush=True)
