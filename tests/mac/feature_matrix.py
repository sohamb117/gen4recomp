#!/usr/bin/env python3
"""Player-facing feature matrix on the real macOS app.

    tests/mac/feature_matrix.py [--app PATH/nativeplat.app | --zip ZIP] [--list] [CASE...]

Unzips the packaged app (default build/dist/nativeplat-macos-arm64.zip) into
a temporary folder with portable.txt beside the bundle (portable mode), then
runs one NP_AUTOTEST per case with a real window (Metal renderer, real audio
device), each on its own copy of the user data. Every case leaves
build/evidence/<case>.png (the window), <case>-small.png (960 wide, for
looking at), <case>.log and, for multi-step cases, <case>-<step>.png, and the
table of cases is written to build/evidence/matrix.json.

Inputs come from the build tree and are never committed: the three ROMs,
the Emerald ROM in .cache/gba for Pal Park, Platinum saves minted from
pc_lab recipes (build/evidence/inputs), and fixtures this script authors
(tests/mac/fixtures.py). A case whose input is missing is skipped with the
reason.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import fixtures  # noqa: E402

EVID = os.path.join(ROOT, 'build', 'evidence')
INPUTS = os.path.join(EVID, 'inputs')
ROMS = {
    'diamond': os.path.join(ROOT, 'games/diamond/build/diamond.us/pokediamond.us.nds'),
    'pearl': os.path.join(ROOT, 'games/diamond/build/pearl.us/pokepearl.us.nds'),
    'platinum': os.path.join(ROOT, 'games/platinum/build/rom/pokeplatinum.us.nds'),
}
GBA_ROM = os.path.join(ROOT, '.cache/gba/pokeemerald/pokeemerald.gba')
SAVE4 = os.environ.get('NP_SAVE4', os.path.join(ROOT, 'build', 'shell-stub', 'features', 'np_save4'))

CASES = []


def case(name, feature):
    def reg(fn):
        CASES.append((name, feature, fn))
        return fn
    return reg


def keys(start, names, gap=2):
    """script steps pressing `names` (SDL scancode names) from `start`."""
    out, f = [], start
    for n in names:
        if n.startswith('@'):  # @N: jump to frame N
            f = int(n[1:])
            continue
        out.append('%d:key:%s' % (f, n))
        f += gap
    return ';'.join(out), f


class Ctx:
    def __init__(self, app_dir, name, work=None):
        self.app_dir = app_dir  # holds nativeplat.app and portable.txt
        self.name = name
        self.exe = os.path.join(app_dir, 'nativeplat.app', 'Contents', 'MacOS', 'nativeplat')
        self.ud = os.path.join(app_dir, 'userdata')
        self.runs = []
        self.work = work or os.path.join(EVID, 'work', name)
        if not work:
            shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work, exist_ok=True)

    def second(self):
        """Another install of the same app (its own portable user data), for
        two-station cases: a clone of the bundle beside this one."""
        d = self.app_dir.rstrip('/') + '-b'
        if not os.path.isdir(os.path.join(d, 'nativeplat.app')):
            os.makedirs(d, exist_ok=True)
            subprocess.run(['cp', '-c', '-R', os.path.join(self.app_dir, 'nativeplat.app'), d], check=True)
            shutil.copyfile(os.path.join(self.app_dir, 'portable.txt'), os.path.join(d, 'portable.txt'))
        b = Ctx(d, self.name, work=self.work)
        b.runs = self.runs
        return b

    # user data ---------------------------------------------------------------
    def fresh(self, roms=()):
        shutil.rmtree(self.ud, ignore_errors=True)
        os.makedirs(os.path.join(self.ud, 'roms'))
        for g in roms:
            # APFS clone: a 64-128 MB ROM costs nothing
            subprocess.run(['cp', '-c', ROMS[g], os.path.join(self.ud, 'roms', g + '.nds')], check=True)

    def put_save(self, game, slot, src):
        d = os.path.join(self.ud, 'saves', game)
        os.makedirs(d, exist_ok=True)
        shutil.copyfile(src, os.path.join(d, slot + '.sav'))

    def options(self, text):
        with open(os.path.join(self.ud, 'options.ini'), 'a') as f:
            f.write(text.strip() + '\n')

    # running -----------------------------------------------------------------
    def start(self, at, step='', args=(), env=None, png=None):
        """Starts one app run; `at` without png=."""
        tag = self.name + ('-' + step if step else '')
        png = png or os.path.join(EVID, tag + '.png')
        spec = at + ',png=' + png
        e = dict(os.environ, NP_AUTOTEST=spec)
        e.pop('SDL_VIDEO_DRIVER', None)
        e.pop('SDL_AUDIO_DRIVER', None)
        e.update(env or {})
        p = subprocess.Popen([self.exe] + list(args), env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             cwd=self.work)
        p.np = dict(tag=tag, png=png, spec=spec, at=at, step=step, args=list(args), t0=time.time())
        return p

    def finish(self, p, timeout=900, expect_rc=0):
        """Waits for a started run, records it; returns the log text."""
        out, _ = p.communicate(timeout=timeout)
        n = p.np
        log = out.decode('utf-8', 'replace')
        with open(os.path.join(EVID, n['tag'] + '.log'), 'w') as f:
            f.write('$ NP_AUTOTEST="%s" nativeplat %s\n' % (n['spec'], ' '.join(n['args'])))
            f.write(log)
        small = None
        if os.path.exists(n['png']):
            small = n['png'][:-4] + '-small.png'
            subprocess.run(['sips', '-Z', '960', n['png'], '--out', small], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            if not os.environ.get('NP_KEEP_FULL'):
                os.remove(n['png'])  # 8 MB each at 2x; NP_KEEP_FULL=1 keeps them
        self.runs.append({'step': n['step'], 'autotest': n['at'], 'args': n['args'], 'rc': p.returncode,
                          'png': os.path.relpath(n['png'], ROOT), 'small': small and os.path.relpath(small, ROOT),
                          'secs': round(time.time() - n['t0'], 1),
                          'summary': ([l for l in log.splitlines() if 'autotest: boot=' in l] or [''])[-1]})
        if p.returncode != expect_rc:
            raise RuntimeError('%s: exit %d\n%s' % (n['tag'], p.returncode, log[-3000:]))
        return log

    def run(self, at, step='', args=(), env=None, timeout=900, png=None, expect_rc=0):
        """One app run; `at` without png=. Returns the log text."""
        return self.finish(self.start(at, step, args, env, png), timeout, expect_rc)

    def need(self, *paths):
        for p in paths:
            if not os.path.exists(p):
                raise FileNotFoundError(p)


def sav_input(name):
    return os.path.join(INPUTS, name)


# ---- cases --------------------------------------------------------------------

@case('launcher_import', 'Launcher: import all three ROMs (drop + Import ROM dialog)')
def _(c):
    c.need(*ROMS.values())
    c.fresh()
    # Diamond and Pearl dropped on the window, Platinum through the Import ROM
    # button (clicked) and the file dialog.
    script = ';'.join(['4:drop:' + ROMS['diamond'], '6:drop:' + ROMS['pearl'], '10:down:138:439',
                       '11:up:138:439', '14:dialog:' + ROMS['platinum']])
    log = c.run('boot=app,frames=40,script=' + script)
    for g in ROMS:
        assert os.path.exists(os.path.join(c.ud, 'roms', g + '.nds')), g
    return log


# Title screen -> CONTINUE -> the field, as tests/gameplay/schedules/continue.press.
CONTINUE = '1250:start;1400:a;1500:a;1600:a'
PLAT_CARD = (784, 270)


def click(f, xy):
    return '%d:down:%d:%d;%d:up:%d:%d' % (f, xy[0], xy[1], f + 1, xy[0], xy[1])


def plat_save(c, slot='Sandgem', save='plat-sandgem.sav', opts=''):
    """User data with the Platinum ROM and a save slot in Sandgem Town."""
    c.need(ROMS['platinum'], sav_input(save))
    c.fresh(['platinum'])
    c.put_save('platinum', slot, sav_input(save))
    c.options('[session]\nlast_game = platinum\nlast_slot_platinum = %s\n%s' % (slot, opts))


def play(c, step='', frames=1800, script='', press=CONTINUE, opts='', slot='Sandgem', **kw):
    """The app started as `nativeplat --game platinum --slot <slot>`."""
    at = 'boot=app,frames=%d' % frames
    if press:
        at += ',press=' + press
    # A script turns on live input (hotkeys, speed); a no-op step keeps it on.
    at += ',script=' + (script or '0:move:1:1')
    return c.run(at, step=step, args=['--game', 'platinum', '--slot', slot], **kw)


OPT_LABELS = ['Screen layout', 'Swap screens', 'Battle layout', 'Rotation', 'Scaling', 'Filter', 'Fullscreen',
              'UI scale', 'Reduce motion', 'Effect 1', 'Effect 1 intensity', 'Effect 2', 'Effect 2 intensity',
              'CRT curvature', 'Performance', 'VSync', 'Display FPS cap', 'Logic clock', 'Real-time clock',
              'On startup', 'Speed', 'Fast-forward speed', 'Volume', 'Mute when unfocused', 'Music filter',
              'Music volume', 'Sound effects volume', '3D render scale', 'Widescreen 3D', 'Camera zoom',
              'Camera tilt', 'Instant text', 'Fix cartridge bugs', 'Rewind history', 'GBA cartridge (Pal Park)',
              'GBA save', 'Touch controls', 'Edit touch controls...', 'Rumble on press (gamepad)',
              'Controller skin', 'Local wireless (LAN)', 'LAN port', 'Join by IP:port',
              'Internet relay host:port', 'Room PIN', 'Wireless status', 'Sync folder', 'Sync now',
              'Sync status', 'Controls...', 'Mods...', 'Updates...', 'About...', 'Quit to launcher', 'Close']


def opt_downs(label, in_game=False, sync=False, updates=False):
    """Down presses from the top of Options to `label` (ui.c options_items)."""
    items = [l for l in OPT_LABELS
             if not (l == 'Quit to launcher' and not in_game)
             and not (l in ('Sync now', 'Sync status') and not sync)
             and not (l == 'Updates...' and not updates)]
    return ['Down'] * items.index(label)


@case('slots', 'Save slots: import .sav, new, duplicate, rename, export .sav, delete')
def _(c):
    c.need(ROMS['platinum'], sav_input('plat-sandgem.sav'))
    c.fresh(['platinum'])
    src = os.path.join(c.work, 'Sandgem.sav')
    shutil.copyfile(sav_input('plat-sandgem.sav'), src)
    sdir = os.path.join(c.ud, 'saves', 'platinum')
    # Import: slots page (New, Import) -> Import .sav... -> file dialog.
    s, _ = keys(10, ['Down', 'Return'])
    c.run('boot=app,frames=30,script=%s;%s;14:dialog:%s' % (click(2, PLAT_CARD), s, src), step='1-import')
    assert os.path.getsize(os.path.join(sdir, 'Sandgem.sav')) == 524288
    # Duplicate: rows New, Sandgem, Import -> Sandgem -> slot menu -> Duplicate.
    s, _ = keys(10, ['Down', 'Return', 'Down', 'Down', 'Down', 'Return'])
    c.run('boot=app,frames=30,script=%s;%s' % (click(2, PLAT_CARD), s), step='2-duplicate')
    dups = sorted(os.listdir(sdir))
    assert len([f for f in dups if f.endswith('.sav')]) == 2, dups
    # Rename the copy (rows New, Sandgem, <copy>, Import).
    s, f = keys(10, ['Down', 'Down', 'Return', 'Down', 'Down', 'Return'] + ['Backspace'] * 32)
    s2, _ = keys(f + 2, ['Return'])
    c.run('boot=app,frames=%d,script=%s;%s;%d:text:Route 201 run;%s' % (f + 12, click(2, PLAT_CARD), s, f, s2),
          step='3-rename')
    assert os.path.exists(os.path.join(sdir, 'Route 201 run.sav')), os.listdir(sdir)
    # Export it (sorted: New, Route 201 run, Sandgem, Import).
    out = os.path.join(c.work, 'exported.sav')
    s, f = keys(10, ['Down', 'Return', 'Down', 'Down', 'Down', 'Down', 'Return'])
    c.run('boot=app,frames=%d,script=%s;%s;%d:dialog:%s' % (f + 10, click(2, PLAT_CARD), s, f + 2, out),
          step='4-export')
    assert open(out, 'rb').read() == open(sav_input('plat-sandgem.sav'), 'rb').read()
    # Delete it: the confirmation, then Yes.
    s, f = keys(10, ['Down', 'Return', 'Up', 'Return'])
    c.run('boot=app,frames=%d,script=%s;%s' % (f + 4, click(2, PLAT_CARD), s), step='5-confirm')
    s, f = keys(10, ['Down', 'Return', 'Up', 'Return', 'Right', 'Return'])
    c.run('boot=app,frames=%d,script=%s;%s' % (f + 6, click(2, PLAT_CARD), s), step='6-deleted')
    assert not os.path.exists(os.path.join(sdir, 'Route 201 run.sav')), os.listdir(sdir)
    # New save slot: the default name, OK -> the game boots in it.
    s, f = keys(10, ['Return', 'Return'])
    c.run('boot=app,frames=400,script=%s;%s' % (click(2, PLAT_CARD), s), step='7-new')
    return None


@case('continue', 'Continue: last slot from the launcher, title CONTINUE to the field')
def _(c):
    plat_save(c)
    # Slots page: Continue: Sandgem is the first row.
    s, _ = keys(6, ['Return'])
    c.run('boot=app,frames=1900,press=1260:start;1410:a;1510:a;1610:a,script=%s;%s' % (click(2, PLAT_CARD), s))


@case('games_boot', 'All three cores boot in the packaged app: card -> New save slot -> OK -> title (Start)')
def _(c):
    c.need(*ROMS.values())
    for g, card in (('diamond', (176, 270)), ('pearl', (480, 270)), ('platinum', PLAT_CARD)):
        c.fresh(list(ROMS))
        # Slots page (New, Import): New -> the name page -> OK boots the game at ~iteration 8.
        c.run('boot=app,frames=1700,press=1400:start:10,script=%s;5:key:Return;7:key:Return' % click(2, card),
              step=g)


def editor_open(extra, frames=None, step=''):
    """Launcher -> Platinum -> (Continue, New,) Sandgem -> Edit save..., then `extra` keys."""
    s, f = keys(10, ['Down', 'Down', 'Return', 'Down', 'Return'])
    e, f = keys(f + 6, extra, gap=3)
    return 'boot=app,frames=%d,script=%s;%s%s' % (frames or f + 6, click(2, PLAT_CARD), s, ';' + e if e else '')


@case('editor', 'Save editor: every tab, an edit with undo/redo, Events (Wonder Card added), save')
def _(c):
    plat_save(c)
    c.run(editor_open([]), step='1-trainer')
    for i, tab in enumerate(['party', 'boxes', 'bag', 'pokedex', 'events'], 2):
        c.run(editor_open(['PageDown'] * (i - 1)), step='%d-%s' % (i, tab))
    # Money: Down x4, Enter opens the number, Up adds at the cursor digit, Enter.
    money = ['Down'] * 4 + ['Return', 'Up', 'Up', 'Up', 'Return']
    c.run(editor_open(money), step='7-edit')
    c.run(editor_open(money + ['C']), step='8-undo')        # C = DS X = undo
    c.run(editor_open(money + ['C', 'V']), step='9-redo')   # V = DS Y = redo
    # Events: Add <first event gift>, then save through the close prompt.
    ev = ['PageDown'] * 5 + ['Down'] * 6 + ['Return']
    c.run(editor_open(ev), step='10-gift')
    c.run(editor_open(ev + ['Escape']), step='11-close-prompt')
    c.run(editor_open(ev + ['Escape', 'Return']), step='12-saved')
    bak = os.path.join(c.ud, 'saves', 'platinum', 'Sandgem.sav.bak')
    assert os.path.exists(bak), 'no .bak after saving'
    if os.path.exists(SAVE4):
        out = subprocess.run([SAVE4, 'dump', os.path.join(c.ud, 'saves', 'platinum', 'Sandgem.sav')],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode()
        with open(os.path.join(EVID, 'editor-save4.txt'), 'w') as f:
            f.write(out)


@case('trainer_card', 'Trainer Card and Pokedex diploma PNG export from the editor')
def _(c):
    plat_save(c)
    for kind, downs in (('card', 17), ('diploma', 18)):
        out = os.path.join(EVID, 'trainer_card-%s-export.png' % kind)
        if os.path.exists(out):
            os.remove(out)
        at = editor_open(['Down'] * downs + ['Return'])
        f = int(re.search(r'frames=(\d+)', at).group(1))
        at = at.replace('frames=%d' % f, 'frames=%d' % (f + 10)) + ';%d:dialog:%s' % (f + 2, out)
        c.run(at, step=kind)
        assert os.path.getsize(out) > 1000, out
        subprocess.run(['sips', '-Z', '960', out, '--out', out[:-4] + '-small.png'], stdout=subprocess.DEVNULL)


LAYOUTS = [('vertical', 'layout = vertical'), ('horizontal', 'layout = horizontal'),
           ('hybrid', 'layout = hybrid'), ('top', 'layout = top'), ('bottom', 'layout = bottom'),
           ('swap', 'layout = vertical\nswap = 1'), ('rotation90', 'layout = vertical\nrotation = 90'),
           ('integer-linear', 'layout = horizontal\nscale = integer\nfilter = linear')]


@case('layouts', 'Display layouts, swap, rotation, integer scale/linear filter (in the field)')
def _(c):
    for name, video in LAYOUTS:
        plat_save(c, opts='[video]\n' + video)
        play(c, step=name, frames=1800)


@case('battle_layout', 'Battle layout: hybrid (large top) during a wild battle, field layout outside')
def _(c):
    plat_save(c, opts='[video]\nlayout = vertical\nbattle_layout = hybrid')
    wild = ('1250:start;1400:a;1500:a;1600:a;1700:down:40;1760:left:600;2300:up:20;2330:left:200;'
            '2550:down:16:48:60;2568:up:16:48:60;2588:a:4:48:60')
    play(c, step='field', frames=1900, press=wild)
    play(c, step='battle', frames=3200, press=wild)


@case('effects', 'Effects chain (LCD grid + scanlines, CRT + curvature, Smooth) and performance presets')
def _(c):
    for name, video in (('lcd-scanlines', 'effect1 = lcd\neffect1_intensity = 80\neffect2 = scanlines\n'
                                          'effect2_intensity = 60'),
                        ('crt-curved', 'effect1 = crt\neffect1_intensity = 90\ncrt_curvature = 1'),
                        ('smooth', 'effect1 = smooth\neffect1_intensity = 100'),
                        ('preset-low', 'effect1 = crt\nperformance = low'),
                        ('preset-high', 'effect1 = crt\ncrt_curvature = 1\nperformance = high')):
        plat_save(c, opts='[video]\nlayout = horizontal\n' + video)
        play(c, step=name, frames=1800)
    # The Options page names the preset and what it decides.
    plat_save(c, opts='[video]\nperformance = balanced')
    s, _ = keys(1800, ['F10'] + opt_downs('Performance', in_game=True))
    play(c, step='preset-options', frames=1830, script=s)


@case('render_scale', '3D render scale 1x vs 4x and widescreen 3D')
def _(c):
    for name, game in (('1x', 'render_scale = 1'), ('4x', 'render_scale = 4'),
                       ('4x-widescreen', 'render_scale = 4\nwidescreen = 1')):
        plat_save(c, opts='[video]\nlayout = top\n[game]\n' + game)
        play(c, step=name, frames=1800)


@case('camera', 'Camera zoom (- / =) and tilt (3 / 4) hotkeys, 0 resets')
def _(c):
    plat_save(c, opts='[video]\nlayout = top')
    play(c, step='original', frames=1800)
    s, _ = keys(1750, ['-'] * 6)
    play(c, step='zoom-out', frames=1800, script=s)
    s, _ = keys(1750, ['='] * 4)
    play(c, step='zoom-in', frames=1800, script=s)
    s, _ = keys(1750, ['4'] * 6)
    play(c, step='tilt', frames=1800, script=s)
    s, _ = keys(1750, ['-'] * 6 + ['4'] * 6 + ['0'])
    play(c, step='reset', frames=1800, script=s)


@case('speed', 'Speed hotkey (1 cycles 1x..uncapped), fast-forward hold (F) and toggle (G)')
def _(c):
    plat_save(c)
    # 1700 iterations at 1x, then 100 at 4x ("1" three times).
    s, _ = keys(1700, ['1', '1', '1'], gap=1)
    log = play(c, step='4x', frames=1800, script=s)
    m = re.search(r'iterations=(\d+) guest_frame=(\d+)', log)
    assert m and int(m.group(2)) > int(m.group(1)) + 150, m and m.groups()
    s, _ = keys(1700, ['G'])
    log = play(c, step='ff-toggle', frames=1800, script=s)
    m = re.search(r'iterations=(\d+) guest_frame=(\d+)', log)
    assert m and int(m.group(2)) > int(m.group(1)) + 100, m and m.groups()


@case('quicksave', 'F1 quick save in the field, F2 F2 quick load (reboot from that save)')
def _(c):
    plat_save(c)
    sav = os.path.join(c.ud, 'saves', 'platinum', 'Sandgem.sav')
    before = open(sav, 'rb').read()
    # Walk left a little, F1; the toast shows the result.
    log = play(c, step='f1', frames=1900, press=CONTINUE + ';1700:left:30', script='1760:key:F1')
    after = open(sav, 'rb').read()
    assert after != before, 'F1 did not change the slot'
    log = play(c, step='f2', frames=1900, press=CONTINUE, script='1760:key:F2;1770:key:F2')


@case('snapshots', 'Snapshot slots (F5 take, F6 next slot, F7 restore) and rewind (hold R)')
def _(c):
    plat_save(c, opts='[game]\nrewind_seconds = 30')
    # F5 at 1700, walk left, F7 restores the spot.
    play(c, step='walked', frames=1880, press=CONTINUE + ';1765:left:80')
    play(c, step='restored', frames=1880, press=CONTINUE + ';1765:left:80', script='1760:key:F5;1860:key:F7')
    play(c, step='slot2', frames=1800, press=CONTINUE, script='1760:key:F6;1762:key:F5')
    log = c.run('boot=app,frames=1915,press=%s;1705:left:120,rewind=1880+30,script=0:move:1:1' % CONTINUE,
                step='rewind', args=['--game', 'platinum', '--slot', 'Sandgem'])
    assert 'rewind depth' in log


@case('audio', 'Music/SE volume and the music low-pass filter (measured from the output)')
def _(c):
    for name, opts in (('default', ''), ('bgm0', '[game]\nbgm_volume = 0'), ('se0', '[game]\nse_volume = 0'),
                       ('filter3', '[audio]\nmusic_filter = 3')):
        plat_save(c, opts=opts)
        play(c, step=name, frames=1800)


@case('instant_text', "Instant text: a new game's second text box, 15 frames after A, half printed vs complete")
def _(c):
    c.need(ROMS['platinum'])
    for name, on in (('off', 0), ('on', 1)):
        c.fresh(['platinum'])
        c.options('[game]\ninstant_text = %d' % on)
        # Card -> New -> OK; title Start, NEW GAME, Rowan's intro; A at 2000 starts "Welcome to the world...".
        c.run('boot=app,frames=2015,press=1410:start:10;1560:a:6;1700:a:6;1800:a:6;1900:a:6;2000:a:6,'
              'script=%s;5:key:Return;7:key:Return' % click(2, PLAT_CARD), step=name)


@case('rules', 'Rules: Fix cartridge bugs on (the core runs each fix with PC_NP_RULES_CHECK)')
def _(c):
    plat_save(c, opts='[game]\nfix_bugs = 1')
    s, f = keys(1800, ['F10'] + opt_downs('Fix cartridge bugs', in_game=True))
    log = play(c, step='options', frames=f + 4, script=s, env={'PC_NP_RULES_CHECK': '1'})
    with open(os.path.join(EVID, 'rules-check.txt'), 'w') as f:
        f.write('\n'.join(l for l in log.splitlines() if 'rule' in l.lower()))


@case('controls', 'Controls rebinding: A gets K in its first key column, saved to options.ini')
def _(c):
    c.fresh()
    # Options -> Controls... -> row A (the first action), first key column -> Enter -> K.
    s, f = keys(4, ['F10'] + opt_downs('Controls...') + ['Return'])
    c.run('boot=app,frames=%d,script=%s' % (f + 4, s), step='page')
    s, f = keys(4, ['F10'] + opt_downs('Controls...') + ['Return', 'Return', 'K'])
    c.run('boot=app,frames=%d,script=%s' % (f + 4, s), step='rebound')
    ini = open(os.path.join(c.ud, 'options.ini')).read()
    assert re.search(r'^a = K\b', ini, re.M), ini


@case('touch_editor', 'Touch controls on, and the touch layout editor (move/resize/fade a control)')
def _(c):
    plat_save(c, opts='[input]\ntouch_controls = on')
    play(c, step='pad', frames=1800)
    s, f = keys(1800, ['F10'] + opt_downs('Edit touch controls...', in_game=True) + ['Return'])
    play(c, step='editor', frames=f + 4, script=s)
    e, f2 = keys(f + 2, ['Tab', 'Tab', 'Right', 'Right', 'Right', 'Up', 'Up', '=', '=', '['])
    play(c, step='edited', frames=f2 + 4, script=s + ';' + e)


@case('skin', 'Delta skin: a hand-made .deltaskin dropped on the window, landscape and portrait')
def _(c):
    skin = fixtures.deltaskin(os.path.join(c.work, 'Test.deltaskin'))
    plat_save(c)
    c.run('boot=app,frames=1800,size=1280x592,press=%s,script=1700:drop:%s' % (CONTINUE, skin),
          step='landscape', args=['--game', 'platinum', '--slot', 'Sandgem'])
    assert os.path.isdir(os.path.join(c.ud, 'skins'))
    c.run('boot=app,frames=1800,size=390x844,press=%s,script=0:move:1:1' % CONTINUE, step='portrait',
          args=['--game', 'platinum', '--slot', 'Sandgem'])


@case('mods', 'Mod manager: install the example package (.zip drop; installed packages start enabled), boot with it')
def _(c):
    c.need(ROMS['platinum'])
    z = fixtures.mod_zip(os.path.join(ROOT, 'games/platinum/pc/mods/example_menu_text'),
                         os.path.join(c.work, 'example_menu_text.zip'))
    plat_save(c)
    s, f = keys(4, ['F10'] + opt_downs('Mods...') + ['Return'])
    c.run('boot=app,frames=%d,script=%s;%d:drop:%s' % (f + 10, s, f + 2, z), step='installed')
    order = open(os.path.join(c.ud, 'mods', 'loadorder.txt')).read()
    assert 'example_menu_text' in order, order
    # Boot to the main menu: its text comes from the package.
    c.run('boot=app,frames=1500,press=1250:start:4:40:4', step='menu',
          args=['--game', 'platinum', '--slot', 'Sandgem'])


@case('carts', 'Custom carts: seal the enabled package as a cart, bind it to a slot, boot it')
def _(c):
    c.need(ROMS['platinum'])
    z = fixtures.mod_zip(os.path.join(ROOT, 'games/platinum/pc/mods/example_menu_text'),
                         os.path.join(c.work, 'example_menu_text.zip'))
    plat_save(c)
    s, f = keys(4, ['F10'] + opt_downs('Mods...') + ['Return'])
    c.run('boot=app,frames=%d,script=%s;%d:drop:%s' % (f + 10, s, f + 2, z), step='0-installed')
    # Seal: the third row (after the package and Install).
    s3, f3 = keys(4, ['F10'] + opt_downs('Mods...') + ['Return', 'Down', 'Down', 'Return'])  # package, Install, Seal
    c.run('boot=app,frames=%d,script=%s;%d:text:Menu Cart;%d:key:Return' % (f3 + 10, s3, f3 + 2, f3 + 4),
          step='1-sealed')
    assert glob.glob(os.path.join(c.ud, 'carts', '*.cart')), os.listdir(c.ud)
    # Slots (Continue, New, Sandgem, Import) -> Sandgem -> Cart: none -> Menu Cart.
    s, f = keys(10, ['Down', 'Down', 'Return'] + ['Down'] * 5 + ['Return'])
    c.run('boot=app,frames=%d,script=%s;%s' % (f + 4, click(2, PLAT_CARD), s), step='2-bound')
    c.run('boot=app,frames=1800,press=1450:start:4:40:4,script=0:move:1:1', step='3-boot',
          args=['--game', 'platinum', '--slot', 'Sandgem'])


@case('palpark', 'GBA cartridge (Emerald) in the slot: MIGRATE FROM EMERALD on the main menu')
def _(c):
    c.need(GBA_ROM, SAVE4)
    plat_save(c)
    sav = os.path.join(c.ud, 'saves', 'platinum', 'Sandgem.sav')
    subprocess.run([SAVE4, 'set-national-dex', sav, '1'], check=True, stdout=subprocess.DEVNULL)
    gsav = os.path.join(c.work, 'emerald.sav')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools/gba/gen3_save.py'), gsav], check=True,
                   stdout=subprocess.DEVNULL)
    # Insert through Options (Enter opens the cartridge dialog).
    s, f = keys(4, ['F10'] + opt_downs('GBA cartridge (Pal Park)') + ['Return'])
    c.run('boot=app,frames=%d,script=%s;%d:dialog:%s;%d:key:Down;%d:key:Return;%d:dialog:%s' %
          (f + 12, s, f + 2, GBA_ROM, f + 4, f + 6, f + 8, gsav), step='options')
    c.run('boot=app,frames=1600,press=1250:start:4:40:4,script=0:move:1:1', step='menu',
          args=['--game', 'platinum', '--slot', 'Sandgem'])


def union_press(side, until):
    """tests/link's Union Room schedule for one station, up to frame `until`."""
    steps = []
    for line in open(os.path.join(ROOT, 'tests/link/schedules/trade-%s.sched' % side)):
        line = line.split('#')[0].strip()
        if line and int(line.split(':')[0]) < until:
            steps.append(line)
    return ';'.join(steps)


@case('lan', 'LAN play: two installs on this Mac meet and talk in the Union Room (tests/link trade schedules, free-running)')
def _(c):
    c.need(ROMS['platinum'], sav_input('union-a.sav'), sav_input('union-b.sav'))
    b = c.second()
    for st, side, port, peer, sid in ((c, 'a', 41001, 41002, '00AAAA'), (b, 'b', 41002, 41001, '00BBBB')):
        st.fresh(['platinum'])
        st.put_save('platinum', 'Union', sav_input('union-%s.sav' % side))
        st.options('[wireless]\nenabled = 1\nport = %d\npeer = 127.0.0.1:%d\nstation_id = %s' % (port, peer, sid))
    frames = 16000
    procs = [st.start('boot=app,realtime=1,frames=%d,shots=1000,press=%s' % (frames, union_press(side, frames)),
                      step='station-' + side, args=['--game', 'platinum', '--slot', 'Union'])
             for st, side in ((c, 'a'), (b, 'b'))]
    for st, p in zip((c, b), procs):
        st.finish(p, timeout=1800)
    for f in glob.glob(os.path.join(EVID, 'lan-station-*-0*.png')):
        subprocess.run(['sips', '-Z', '960', f, '--out', f], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if os.path.exists(SAVE4):
        with open(os.path.join(EVID, 'lan-parties.txt'), 'w') as out:
            for st, side in ((c, 'a'), (b, 'b')):
                d = subprocess.run([SAVE4, 'dump', os.path.join(st.ud, 'saves', 'platinum', 'Union.sav')],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode()
                out.write('== station %s\n%s\n' % (side, d))


@case('relay', 'Relay: host:port and PIN typed in Options, LAN on; a second install on the same relay+PIN is in range')
def _(c):
    c.fresh()
    binary = os.path.join(c.work, 'relay')
    subprocess.run(['go', 'build', '-o', binary, '.'], cwd=os.path.join(ROOT, 'server', 'relay'), check=True)
    relay = subprocess.Popen([binary, '-listen', '127.0.0.1:%d' % RELAY_PORT], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        s1, f = keys(4, ['F10'] + opt_downs('Internet relay host:port') + ['Return'])
        s1 += ';%d:text:127.0.0.1:%d;%d:key:Return' % (f, RELAY_PORT, f + 2)
        s2, f = keys(f + 4, opt_downs('Room PIN') + ['Return'])
        s2 += ';%d:text:4242;%d:key:Return' % (f, f + 2)
        s3, f = keys(f + 4, opt_downs('Local wireless (LAN)') + ['Return', 'Down', 'Down', 'Down', 'Down', 'Down'])
        # Station B: another install already set to the same relay and PIN.
        b = c.second()
        b.fresh()
        b.options('[wireless]\nenabled = 1\nrelay = 127.0.0.1:%d\npin = 4242\nstation_id = 00BBBB' % RELAY_PORT)
        sb, _ = keys(4, ['F10'] + opt_downs('Wireless status'))
        pb = b.start('boot=app,realtime=1,frames=%d,script=%s' % (f + 420, sb), step='station-b')
        c.run('boot=app,realtime=1,frames=%d,script=%s;%s;%s' % (f + 360, s1, s2, s3), step='options')
        b.finish(pb)
    finally:
        relay.kill()
    ini = open(os.path.join(c.ud, 'options.ini')).read()
    assert 'relay = 127.0.0.1:%d' % RELAY_PORT in ini and 'pin = 4242' in ini, ini


RELAY_PORT = 47999


@case('sync', 'Folder sync: a slot mirrored to the folder, a conflict, the chooser, resolution')
def _(c):
    plat_save(c)
    folder = os.path.join(c.work, 'Sync')
    os.makedirs(folder)
    c.options('[sync]\nfolder = ' + folder)
    c.run('boot=app,frames=10', step='1-synced')
    mirrored = os.path.join(folder, 'platinum', 'Sandgem.sav')
    assert os.path.exists(mirrored), os.listdir(folder)
    # Both sides change: this device and "another device" via the folder.
    other = bytearray(open(sav_input('union-a.sav'), 'rb').read())
    with open(mirrored, 'wb') as f:
        f.write(other)
    time.sleep(1.1)
    local = os.path.join(c.ud, 'saves', 'platinum', 'Sandgem.sav')
    data = bytearray(open(local, 'rb').read())
    data[-1] ^= 0xFF
    with open(local, 'wb') as f:
        f.write(data)
    c.run('boot=app,frames=10', step='2-conflict')
    c.run('boot=app,frames=10,script=4:key:Right;6:key:Return', step='3-resolved')


@case('updater', 'Updater: check a local test release server, download and verify the zip')
def _(c):
    c.fresh()
    payload = b'PK\x05\x06' + b'\0' * 18  # an empty zip
    with fixtures.ReleaseServer('nativeplat/nativeplat', 'v9.9.9', payload) as srv:
        c.options('[updates]\nrepo = nativeplat/nativeplat\napi = ' + srv.api)
        s, f = keys(4, ['F10'] + opt_downs('Updates...', updates=True) + ['Return', 'Return'])
        c.run('boot=app,frames=%d,script=%s' % (f + 60, s), step='checked')
        s2, f2 = keys(f + 60, ['Return'])
        c.run('boot=app,frames=%d,script=%s;%s' % (f2 + 90, s, s2), step='downloaded')
    got = glob.glob(os.path.join(c.ud, '*macos*.zip'))
    assert got and open(got[0], 'rb').read() == payload, os.listdir(c.ud)
    with fixtures.ReleaseServer('nativeplat/nativeplat', 'v9.9.9', payload, bad_digest=True) as srv:
        c.fresh()
        c.options('[updates]\nrepo = nativeplat/nativeplat\napi = ' + srv.api)
        c.run('boot=app,frames=%d,script=%s;%s' % (f2 + 90, s, s2), step='bad-digest')
    assert not glob.glob(os.path.join(c.ud, '*macos*.zip')), 'a zip with a wrong digest was kept'


@case('standalone_editor', 'Standalone editor: nativeplat --editor --save <file>')
def _(c):
    c.need(ROMS['platinum'])
    c.fresh(['platinum'])
    f = os.path.join(c.work, 'backup.sav')
    shutil.copyfile(sav_input('plat-sandgem.sav'), f)
    c.run('boot=app,frames=20', args=['--editor', '--save', f])


@case('launch', 'Launch flags (--game/--slot, --launcher) and the nativeplat:// URL')
def _(c):
    plat_save(c, opts='[session]\nstartup = continue')
    c.options('[session]\nstartup = continue')
    c.run('boot=app,frames=300', step='flags', args=['--game', 'platinum', '--slot', 'Sandgem'])
    c.run('boot=app,frames=20', step='launcher-flag', args=['--launcher'])
    c.run('boot=app,frames=20', step='bad-slot', args=['--game', 'platinum', '--slot', 'Nope'])
    c.run('boot=app,frames=300,script=4:drop:nativeplat://launch?game=platinum&slot=Sandgem', step='url',
          args=['--launcher'])


@case('ui_scale', 'UI scale (2x, 6x) and reduced motion (steady caret)')
def _(c):
    for name, opts in (('2x', 'ui_scale = 2'), ('6x', 'ui_scale = 6')):
        c.fresh()
        c.options('[interface]\n' + opts)
        s, f = keys(4, ['F10'])
        c.run('boot=app,frames=%d,script=%s' % (f + 4, s), step=name)
    c.fresh(['platinum'])
    c.options('[interface]\nreduce_motion = 1')
    s, f = keys(10, ['Return'])
    c.run('boot=app,frames=%d,script=%s;%s' % (f + 40, click(2, PLAT_CARD), s), step='reduce-motion')


@case('about', 'About / credits page')
def _(c):
    c.fresh()
    c.run('boot=app,frames=10,script=2:down:592:439;3:up:592:439')


@case('url_open', "nativeplat:// URL through LaunchServices: open -a nativeplat.app 'nativeplat://launch?...'")
def _(c):
    plat_save(c)
    png = os.path.join(EVID, 'url_open.png')
    log = os.path.join(EVID, 'url_open.log')
    for f in (png, log):
        if os.path.exists(f):
            os.remove(f)
    at = 'boot=app,frames=600,script=0:move:1:1,png=' + png
    url = 'nativeplat://launch?game=platinum&slot=Sandgem'
    t0 = time.time()
    subprocess.run(['open', '-n', '-W', '--env', 'NP_AUTOTEST=' + at, '--stdout', log, '--stderr', log,
                    '-a', os.path.join(c.app_dir, 'nativeplat.app'), url], check=True, timeout=600)
    text = open(log).read() if os.path.exists(log) else ''
    small = png[:-4] + '-small.png'
    subprocess.run(['sips', '-Z', '960', png, '--out', small], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    c.runs.append({'step': '', 'autotest': at, 'args': ['open -n -W -a nativeplat.app ' + url], 'rc': 0,
                   'png': os.path.relpath(png, ROOT), 'small': os.path.relpath(small, ROOT),
                   'secs': round(time.time() - t0, 1),
                   'summary': ([l for l in text.splitlines() if 'autotest: boot=' in l] or [''])[-1]})
    assert 'view=game' in text, text[-2000:]


@case('portable', 'Portable mode: portable.txt beside nativeplat.app keeps data in userdata/')
def _(c):
    c.fresh(['platinum'])
    log = c.run('boot=app,frames=10')
    assert 'userdata/ (portable)' in log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip', default=os.path.join(ROOT, 'build/dist/nativeplat-macos-arm64.zip'))
    ap.add_argument('--app', help='use this nativeplat.app instead of unzipping --zip')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('cases', nargs='*')
    a = ap.parse_args()
    if a.list:
        for n, f, _ in CASES:
            print('%-24s %s' % (n, f))
        return 0
    os.makedirs(EVID, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='np-macmatrix.')
    if a.app:
        subprocess.run(['ditto', a.app, os.path.join(tmp, 'nativeplat.app')], check=True)
    else:
        subprocess.run(['ditto', '-x', '-k', a.zip, tmp], check=True)
        inner = glob.glob(os.path.join(tmp, '*', 'nativeplat.app'))
        if inner:
            os.rename(inner[0], os.path.join(tmp, 'nativeplat.app'))
    with open(os.path.join(tmp, 'portable.txt'), 'w') as f:
        f.write('feature matrix\n')
    print('app copy:', tmp)
    table_path = os.path.join(EVID, 'matrix.json')
    table = json.load(open(table_path)) if os.path.exists(table_path) else {}
    rc = 0
    for name, feature, fn in CASES:
        if a.cases and name not in a.cases:
            continue
        c = Ctx(tmp, name)
        row = {'feature': feature}
        try:
            fn(c)
            row['result'] = 'ok'
        except FileNotFoundError as e:
            row['result'] = 'skip: missing %s' % e
        except Exception as e:  # noqa: BLE001
            row['result'] = 'FAIL: %s' % str(e).splitlines()[0]
            rc = 1
        row['runs'] = c.runs
        table[name] = row
        print('%-24s %s' % (name, row['result']))
        for r in c.runs:
            print('    %s  %s' % (r['small'] or r['png'], r['summary'][-160:]))
        with open(table_path, 'w') as f:
            json.dump(table, f, indent=1)
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.rmtree(tmp + '-b', ignore_errors=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
