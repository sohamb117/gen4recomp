#!/usr/bin/env python3
"""Two-instance link runs of the real cores (np_headless --lockstep).

Two np_headless processes run in frame lockstep on loopback (station A id
0x111111, B 0x222222), each with its own save and press schedule, so a run
repeats exactly. --fork-at checkpoints both at a frame and forks one child
pair per job, so a scenario's tail is replayed in seconds instead of its walk
into the Union Room.

  linkpair.py serve DIR --frame F --a-sched S --b-sched S --a-save SAV --b-save SAV
                        [--a-game G] [--b-game G]
      start the pair; at frame F it waits for jobs (Ctrl-C / `stop` ends it)
  linkpair.py job DIR NAME FRAMES EVERY A_SCHED B_SCHED
      run one child pair from the checkpoint to FRAMES, dumping both screens
      every EVERY frames into DIR/NAME/{a,b}; saves land in DIR/NAME/{A,B}.sav
  linkpair.py stop DIR
  linkpair.py sheet RUN OUT.png FRAME...
      side-by-side A|B contact sheet of dumped frames
  linkpair.py party SAV [G]
      species of the save's party (np_save4 dump; np_save5 for black/white)

  linkpair.py mint [--game G] RECIPE OUT.sav
      mint a save from a lab recipe (tests/link/recipes) with this core
  linkpair.py mint-bw GAME SIDE OUT.sav
      a Black/White station's save inside Striaton's Pokemon Center, in front
      of the Union Room counter (mint_bw)
  linkpair.py mint-hgss GAME SIDE OUT.sav
      a HeartGold/SoulSilver station's save in Violet City, near the Pokemon
      Center (mint_hgss)

G is platinum (the default), diamond, pearl, black, white, heartgold or
soulsilver, per station: a Diamond and a Platinum station are two different
cores (build/core-dp, build/core-plat) on the same lockstep wire; Black and
White share build/core-bw, HeartGold and SoulSilver build/core-hgss.

The regression scenarios themselves are in run_link_tests.py. Environment:
NP_HEADLESS (Platinum's core), NP_DP_HEADLESS (Diamond/Pearl's),
NP_BW_HEADLESS (Black/White's), NP_HGSS_HEADLESS (HeartGold/SoulSilver's),
NP_SAVE4, NP_PLAT_ROM, NP_DIAMOND_ROM, NP_PEARL_ROM, NP_BLACK_ROM,
NP_WHITE_ROM, NP_HG_ROM, NP_SS_ROM override the default build paths;
NP_DP_BASE_SAVE_<GAME> (DIAMOND, PEARL) names a ready new-game save for
the D/P mint, NP_HGSS_LINK_BASE_<GAME> (HEARTGOLD, SOULSILVER) the chain
save the HG/SS mint starts from. --relay/--pin/--drop on serve put the
game's datagrams through server/relay.
"""
import json
import os
import random
import re
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SAVE4 = os.environ.get('NP_SAVE4', os.path.join(ROOT, 'build', 'features', 'np_save4'))
SAVE5 = os.environ.get('NP_SAVE5', os.path.join(ROOT, 'build', 'features', 'np_save5'))
GAMES = {
    'platinum': (os.environ.get('NP_HEADLESS', os.path.join(ROOT, 'build', 'core-plat', 'np_headless')),
                 os.environ.get('NP_PLAT_ROM', os.path.join(ROOT, 'games', 'platinum', 'build', 'rom',
                                                            'pokeplatinum.us.nds'))),
    'diamond': (os.environ.get('NP_DP_HEADLESS', os.path.join(ROOT, 'build', 'core-dp', 'np_headless')),
                os.environ.get('NP_DIAMOND_ROM', os.path.join(ROOT, 'games', 'diamond', 'build', 'diamond.us',
                                                              'pokediamond.us.nds'))),
    'pearl': (os.environ.get('NP_DP_HEADLESS', os.path.join(ROOT, 'build', 'core-dp', 'np_headless')),
              os.environ.get('NP_PEARL_ROM', os.path.join(ROOT, 'games', 'diamond', 'build', 'pearl.us',
                                                          'pokepearl.us.nds'))),
    'black': (os.environ.get('NP_BW_HEADLESS', os.path.join(ROOT, 'build', 'core-bw', 'np_headless')),
              os.environ.get('NP_BLACK_ROM', os.path.join(ROOT, 'roms',
                                                           'Pokemon - Black Version (USA, Europe) (NDSi Enhanced).nds'))),
    'white': (os.environ.get('NP_BW_HEADLESS', os.path.join(ROOT, 'build', 'core-bw', 'np_headless')),
              os.environ.get('NP_WHITE_ROM', os.path.join(ROOT, 'roms',
                                                           'Pokemon - White Version (USA, Europe) (NDSi Enhanced).nds'))),
    'heartgold': (os.environ.get('NP_HGSS_HEADLESS', os.path.join(ROOT, 'build', 'core-hgss', 'np_headless')),
                  os.environ.get('NP_HG_ROM', os.path.join(ROOT, 'games', 'heartgold', 'build', 'heartgold.us',
                                                           'pokeheartgold.us.nds'))),
    'soulsilver': (os.environ.get('NP_HGSS_HEADLESS', os.path.join(ROOT, 'build', 'core-hgss', 'np_headless')),
                   os.environ.get('NP_SS_ROM', os.path.join(ROOT, 'games', 'heartgold', 'build', 'soulsilver.us',
                                                            'pokesoulsilver.us.nds'))),
}
IDS = {'a': '0x111111', 'b': '0x222222'}
BW_GAMES = ('black', 'white')
HGSS_GAMES = ('heartgold', 'soulsilver')


def core(game):
    """The np_headless command head for one station: binary, game, ROM."""
    headless, rom = GAMES[game]
    return [headless, game, rom]


def start_pair(dirpath, frame, scheds, saves, ctls, relay=None, pin='4242', drop=0, games=None):
    """Starts both instances with --fork-at frame:ctl; returns the Popens.
    With relay (HOST:PORT) the game's datagrams take the relay and lose
    `drop` percent on the way out, the frame clocks still in lockstep."""
    games = games or {'a': 'platinum', 'b': 'platinum'}
    os.makedirs(dirpath, exist_ok=True)
    port = random.randrange(20000, 40000)
    ports = {'a': (port, port + 1), 'b': (port + 1, port)}
    procs = {}
    for i, side in enumerate('ab'):
        cmd = core(games[side]) + ['--frames', str(frame + 1), '--save', saves[side],
               '--lockstep', '%d:%d' % ports[side], '--net-id', IDS[side],
               '--fork-at', '%d:%s' % (frame, ctls[side])]
        if relay:
            cmd += ['--net', str(port + 10 + i), '--net-relay', relay, '--net-pin', pin,
                    '--net-drop', str(drop)]
        if scheds[side]:
            cmd += ['--schedule', scheds[side]]
        log = open(os.path.join(dirpath, 'checkpoint-%s.log' % side), 'w')
        procs[side] = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    return procs


def ctl_line(run, side, frames, every, sched):
    d = os.path.join(run, side)
    os.makedirs(d, exist_ok=True)
    return '%s %d %s %d %s %s\n' % (os.path.abspath(sched) if sched != '-' else '-', frames, d, every,
                                    os.path.join(run, side.upper() + '.sav'), os.path.join(run, side + '.log'))


def serve(argv):
    import argparse
    p = argparse.ArgumentParser(prog='linkpair.py serve')
    p.add_argument('dir')
    p.add_argument('--frame', type=int, required=True)
    p.add_argument('--a-sched')
    p.add_argument('--b-sched')
    p.add_argument('--a-save', required=True)
    p.add_argument('--b-save', required=True)
    p.add_argument('--a-game', choices=sorted(GAMES), default='platinum')
    p.add_argument('--b-game', choices=sorted(GAMES), default='platinum')
    p.add_argument('--relay', help='HOST:PORT of server/relay')
    p.add_argument('--pin', default='4242')
    p.add_argument('--drop', type=int, default=0)
    a = p.parse_args(argv)
    d = os.path.abspath(a.dir)
    os.makedirs(d, exist_ok=True)
    ctls, saves = {}, {}
    for side in 'ab':
        ctls[side] = os.path.join(d, 'ctl-' + side)
        if os.path.exists(ctls[side]):
            os.remove(ctls[side])
        os.mkfifo(ctls[side])
        # The checkpoint's own save is a copy; the game may write it.
        saves[side] = os.path.join(d, 'base-%s.sav' % side)
        with open(getattr(a, side + '_save'), 'rb') as s, open(saves[side], 'wb') as o:
            o.write(s.read())
    jobs = os.path.join(d, 'jobs')
    if os.path.exists(jobs):
        os.remove(jobs)
    os.mkfifo(jobs)
    procs = start_pair(d, a.frame, {'a': a.a_sched, 'b': a.b_sched}, saves, ctls, a.relay, a.pin, a.drop,
                       {'a': a.a_game, 'b': a.b_game})
    w = {side: open(ctls[side], 'w') for side in 'ab'}  # blocks until each reaches the checkpoint
    print('checkpoint at frame %d ready' % a.frame, flush=True)
    try:
        while True:
            with open(jobs) as j:
                for line in j:
                    f = line.split()
                    if not f:
                        continue
                    if f[0] == 'stop':
                        raise KeyboardInterrupt
                    name, frames, every, sa, sb = f
                    run = os.path.join(d, name)
                    for side, s in (('a', sa), ('b', sb)):
                        w[side].write(ctl_line(run, side, int(frames), int(every), s))
                        w[side].flush()
    except KeyboardInterrupt:
        pass
    for side in 'ab':
        w[side].close()
    for side in 'ab':
        procs[side].wait()


def wait_done(run, frames, timeout):
    """Both child logs have their closing status line (or a failure)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        done = 0
        for side in 'ab':
            try:
                txt = open(os.path.join(run, side + '.log')).read()
            except OSError:
                txt = ''
            if '\nstatus ' in txt or 'FAILED' in txt:
                done += 1
        if done == 2:
            return True
        time.sleep(0.5)
    return False


def job(argv):
    d, name, frames, every, sa, sb = argv
    d = os.path.abspath(d)
    run = os.path.join(d, name)
    if os.path.exists(run):
        subprocess.run(['rm', '-rf', run])
    with open(os.path.join(d, 'jobs'), 'w') as j:
        j.write('%s %s %s %s %s\n' % (name, frames, every, os.path.abspath(sa), os.path.abspath(sb)))
    if not wait_done(run, int(frames), 3600):
        sys.exit('timeout')
    for side in 'ab':
        print(side, open(os.path.join(run, side + '.log')).read().strip().splitlines()[-1])


def readppm(p):
    data = open(p, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def sheet(run, out, frames, cols=2):
    """A|B pairs of dumped frames, `cols` pairs per row, written as PNG. Each
    frame number picks the nearest dump at or before it."""
    W, H = 256, 384
    have = sorted(int(n[6:12]) for n in os.listdir(os.path.join(run, 'a')) if n.endswith('.ppm'))
    frames = [max([h for h in have if h <= fr] or have[:1]) for fr in frames]
    rows = (len(frames) + cols - 1) // cols
    SW, SH = cols * (2 * W + 8), rows * (H + 8)
    buf = bytearray(b'\x40' * (SW * SH * 3))
    for i, fr in enumerate(frames):
        r, c = divmod(i, cols)
        for s, side in enumerate('ab'):
            p = os.path.join(run, side, 'frame_%06d.ppm' % fr)
            if not os.path.exists(p):
                continue
            w, h, px = readppm(p)
            x0, y0 = c * (2 * W + 8) + s * W, r * (H + 8)
            for y in range(h):
                o = ((y0 + y) * SW + x0) * 3
                buf[o:o + w * 3] = px[y * w * 3:(y + 1) * w * 3]
    tmp = out + '.ppm'
    with open(tmp, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (SW, SH))
        f.write(buf)
    subprocess.run(['sips', '-s', 'format', 'png', tmp, '--out', out], capture_output=True)
    os.remove(tmp)


def party(sav, game='platinum'):
    tool = SAVE5 if game in BW_GAMES else SAVE4
    out = subprocess.run([tool, 'dump', sav], capture_output=True, text=True, check=True).stdout
    return [m['species'] for m in json.loads(out)['party']]


# Black/White have no lab. A station's save is a chain milestone's (tests/e2e,
# build/e2e/<game>/<BW_BASE>/end.sav, or NP_BW_LINK_BASE_<GAME>): it has the
# C-Gear and flag 0x73, which opens the Union Room counter (scr 0855 script 2).
# np_save5 renames it per side and puts the player outside Striaton's Pokemon
# Center (zone 6, the door at (781,587)); the game walks in and saves there, so
# the save holds the Center's own objects (a save moved straight into zone 8
# would CONTINUE with the old map's objects, and no attendant); np_save5 then
# moves the player to (4,3,6), in front of the Union Room attendant.
BW_BASE = '10-route3-cheren-wellspring-plasma'
BW_SIDE = {'a': ('LINKA', '11111', '1111'), 'b': ('LINKB', '22222', '2222')}


def bw_base_save(game):
    return os.environ.get('NP_BW_LINK_BASE_' + game.upper(),
                          os.path.join(ROOT, 'build', 'e2e', game, BW_BASE, 'end.sav'))


def mint_bw(game, side, out_sav):
    base = bw_base_save(game)
    name, tid, sid = BW_SIDE[side]
    door = out_sav + '.door'
    for args in (['set-name', base, name, '-o', door], ['set-ids', door, tid, sid],
                 ['set-location', door, '6', '781', '0', '588']):
        subprocess.run([SAVE5] + args, check=True, capture_output=True)
    p = subprocess.run(core(game) + ['--frames', '7800', '--save', door, '--schedule',
                                     os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  'schedules', 'bw-pc-save.sched')],
                       capture_output=True, text=True)
    loc = json.loads(subprocess.run([SAVE5, 'dump', door], capture_output=True, text=True,
                                    check=True).stdout)['location']
    if p.returncode != 0 or loc['map'] != 8:
        sys.exit('mint-bw %s %s: the in-Center save did not happen (rc %d, map %d)\n%s'
                 % (game, side, p.returncode, loc['map'], p.stdout[-2000:]))
    subprocess.run([SAVE5, 'set-location', door, '8', '4', '3', '6', '-o', out_sav], check=True,
                   capture_output=True)
    for leftover in (door, door + '.bak'):
        if os.path.exists(leftover):
            os.remove(leftover)
    return out_sav


# HeartGold/SoulSilver have no lab either. A station's save is the e2e
# chain's milestone 04 (tests/e2e, build/e2e/<game>/<HGSS_BASE>/end.sav, or
# NP_HGSS_LINK_BASE_<GAME>): the game's own save in Violet City, a few steps
# from the Pokemon Center, so CONTINUE has the town's people (np_save4's
# set-location would drop them). np_save4 renames it per side and adds a
# second Pokemon to the party (a link trade never takes the last one): A
# SENTRET, B HOOTHOOT, both level 8, behind the chain's CYNDAQUIL.
HGSS_BASE = '04-route30-31-violet'
HGSS_SIDE = {'a': ('LINKA', '11111', '1111', 161), 'b': ('LINKB', '22222', '2222', 163)}


def hgss_base_save(game):
    return os.environ.get('NP_HGSS_LINK_BASE_' + game.upper(),
                          os.path.join(ROOT, 'build', 'e2e', game, HGSS_BASE, 'end.sav'))


def mint_hgss(game, side, out_sav):
    name, tid, sid, species = HGSS_SIDE[side]
    for args in (['set-name', hgss_base_save(game), name, '-o', out_sav], ['set-ids', out_sav, tid, sid],
                 ['add-mon', out_sav, GAMES[game][1], str(species), '8']):
        subprocess.run([SAVE4] + args, check=True, capture_output=True)
    if os.path.exists(out_sav + '.bak'):
        os.remove(out_sav + '.bak')
    return out_sav


def dp_base_save(game, workdir):
    """Diamond/Pearl's lab works on a save being continued: the game's own
    new-game save, played by tests/dp/<game>_first_save.sched once per work
    directory (or NP_DP_BASE_SAVE_<GAME>)."""
    given = os.environ.get('NP_DP_BASE_SAVE_' + game.upper())
    if given:
        return given
    out = os.path.join(workdir, '%s-new-game.sav' % game)
    if os.path.exists(out):
        return out
    sched = os.path.join(ROOT, 'tests', 'dp', '%s_first_save.sched' % game)
    frames = [l.split(':', 1)[1].strip() for l in open(sched) if l.startswith('# frames:')][0]
    tmp = out + '.tmp'
    if os.path.exists(tmp):
        os.remove(tmp)
    p = subprocess.run(core(game) + ['--frames', frames, '--save', tmp, '--schedule', sched],
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) != 512 * 1024:
        sys.exit('%s new-game save failed:\n%s%s' % (game, p.stdout, p.stderr))
    os.rename(tmp, out)
    return out


def mint(recipe, out_sav, game='platinum', base_dir=None):
    """A save from a lab recipe, minted by the same core np_headless runs.
    Platinum: names resolved by pc/tests/pc_lab.py; the lab applies the
    recipe at frame 1800 under the lab-settle input and the run ends at 4000.
    Diamond/Pearl: names resolved by tests/gameplay/labc.py; the new-game
    save is CONTINUEd and D's lab (games/diamond/pc/game/pc_dp_lab.c)
    applies the recipe once the field is free from frame 1800, then saves."""
    if game != 'platinum':
        return mint_dp(recipe, out_sav, game, base_dir or os.path.dirname(os.path.abspath(out_sav)))
    plat = os.path.join(ROOT, 'games', 'platinum')
    sys.path.insert(0, os.path.join(plat, 'pc', 'tests'))
    import pc_lab
    gen = os.path.join(plat, 'build', 'pc', 'geninclude', 'generated')
    if not os.path.isdir(gen):
        gen = os.path.join(plat, 'build', 'pc-wasm', 'geninclude', 'generated')
    pc_lab.GENINCLUDE = gen
    lab = pc_lab.compile_recipe(recipe, out_sav + '.lab')
    # The guest sees no host paths, so both scripts go inline.
    ops = [l.strip() for l in open(lab) if l.strip() and not l.startswith('#')]
    os.remove(lab)
    settle = [l.strip() for l in open(os.path.join(plat, 'pc', 'replays', 'lab-settle.txt'))
              if l.strip() and not l.startswith('#')]
    env = dict(os.environ, PC_LAB='inline:' + ';'.join(ops), PC_LAB_AT='1800',
               PC_INPUT='inline:' + ';'.join(settle))
    if os.path.exists(out_sav):
        os.remove(out_sav)
    p = subprocess.run(core('platinum') + ['--frames', '4000', '--save', out_sav], env=env,
                       capture_output=True, text=True)
    if not os.path.exists(out_sav) or 'status 0' not in p.stdout + p.stderr:
        sys.exit('mint %s failed:\n%s%s' % (recipe, p.stdout, p.stderr))
    return out_sav


def mint_dp(recipe, out_sav, game, base_dir):
    sys.path.insert(0, os.path.join(ROOT, 'tests', 'gameplay'))
    import labc
    base = dp_base_save(game, base_dir)
    with open(base, 'rb') as s, open(out_sav, 'wb') as o:
        o.write(s.read())
    env = dict(os.environ, PC_LAB=labc.compile_inline(recipe, game), PC_LAB_AT='1800')
    p = subprocess.run(core(game) + ['--frames', '9000', '--save', out_sav, '--schedule',
                                     os.path.join(ROOT, 'tests', 'gameplay', 'dp', 'schedules', 'continue.press')],
                       env=env, capture_output=True, text=True)
    if not re.search(r'pc_lab: applied .* save ok', p.stdout + p.stderr):
        sys.exit('mint %s (%s) failed:\n%s%s' % (recipe, game, p.stdout, p.stderr))
    return out_sav


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, rest = sys.argv[1], sys.argv[2:]
    if cmd == 'serve':
        serve(rest)
    elif cmd == 'job':
        job(rest)
    elif cmd == 'stop':
        with open(os.path.join(rest[0], 'jobs'), 'w') as j:
            j.write('stop\n')
    elif cmd == 'sheet':
        sheet(rest[0], rest[1], [int(x) for x in rest[2:]])
    elif cmd == 'mint':
        game = 'platinum'
        if rest[:1] == ['--game']:
            game, rest = rest[1], rest[2:]
        print(mint(rest[0], rest[1], game))
    elif cmd == 'mint-bw':
        print(mint_bw(rest[0], rest[1], rest[2]))
    elif cmd == 'mint-hgss':
        print(mint_hgss(rest[0], rest[1], rest[2]))
    elif cmd == 'party':
        print(party(rest[0], rest[1] if len(rest) > 1 else 'platinum'))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
