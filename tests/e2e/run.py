#!/usr/bin/env python3
"""Milestone chains on the real core, headless (tests/e2e/README.md).

    tests/e2e/run.py --game G [--from M] [--only M ...] [--systems] [--lab] [--planned] [--out DIR] [--check]

G is platinum, diamond or pearl, heartgold or soulsilver, black or white, or emerald, ruby or sapphire. The chain is tests/e2e/<G>/chain.txt (side
systems: systems.txt with --systems); each entry is a milestone directory
holding milestone.toml. Every milestone runs np_gp in serve mode with the
e2e probe on, starts from the previous milestone's end save (or a lab
recipe, or a blank chip), drives the game with its steps (bots.py), saves,
and is judged against its [expect] table. OUT (default build/e2e/<G>) gets
OUT/<milestone>/ (start.sav, end.sav, run.log, steps.txt, shots, the
contact sheet <milestone>.png) and OUT/report.md. Exit status 1 if any
milestone failed, 2 on a usage or setup error.
"""
import argparse
import datetime
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GAMEPLAY = os.path.join(ROOT, "tests", "gameplay")
sys.path.insert(0, HERE)
sys.path.insert(0, GAMEPLAY)

import labc  # noqa: E402
from bots import BOTS  # noqa: E402
from np_e2e import BW_GAMES, GBA_GAMES, HGSS_GAMES, Budget, Dead, HarnessError, Session  # noqa: E402

GAMES = {
    "platinum": ("games/platinum/build/rom/pokeplatinum.us.nds", "build/core-plat"),
    "diamond": ("games/diamond/build/diamond.us/pokediamond.us.nds", "build/core-dp"),
    "pearl": ("games/diamond/build/pearl.us/pokepearl.us.nds", "build/core-dp"),
    # HeartGold/SoulSilver: the pokeheartgold ROM builds (tools/rom_build.sh heartgold|soulsilver) and the HG/SS
    # core (docs/HANDOFF-hgss.md)
    "heartgold": ("games/heartgold/build/heartgold.us/pokeheartgold.us.nds", "build/core-hgss"),
    "soulsilver": ("games/heartgold/build/soulsilver.us/pokesoulsilver.us.nds", "build/core-hgss"),
    # the GBA games: the decomps' own ROM builds (tools/rom_build.sh; docs/HANDOFF-rse.md) and the RSE core
    "emerald": (".cache/gba/pokeemerald/pokeemerald.gba", "build/rse/native"),
    "ruby": (".cache/gba/pokeruby/pokeruby.gba", "build/rse/native"),
    "sapphire": (".cache/gba/pokeruby/pokesapphire.gba", "build/rse/native"),
    # ROM-only recompilations (docs/BW_PLAN.md): the cartridge in the checkout's gitignored roms/, one core for both
    "black": ("roms/Pokemon - Black Version (USA, Europe) (NDSi Enhanced).nds", "build/core-bw"),
    "white": ("roms/Pokemon - White Version (USA, Europe) (NDSi Enhanced).nds", "build/core-bw"),
}
# the GBA games' save tool (tools/gba/gen3_dump.py: `dump ROM SAV`, `gamedata ROM`, np_save4's shapes) and save lab
GEN3_DUMP = os.path.join(ROOT, "tools", "gba", "gen3_dump.py")
GEN3_LAB = os.path.join(ROOT, "tools", "gba", "gen3_lab.py")

TOP_KEYS = {"title", "notes", "status", "priority", "estimate", "refs", "version", "start", "run", "step", "expect",
            "shots"}
START_KEYS = {"from", "recipe", "lab", "blank", "boot", "boost"}
# [start] boost: lab verbs a boost recipe may use on the start save (AUTHORING.md, Boosts). Party strength and
# items only: a boost never writes story state (flags, vars, badges, the map), so the chain's story stays played.
BOOST_VERBS = {"party", "party-move", "party-level", "party-item", "party-iv", "party-ev", "item", "register-item"}
RUN_KEYS = {"frames", "save", "options", "clock", "env"}
EXPECT_KEYS = {"map", "position", "badges", "badge", "flags", "flags_clear", "vars", "party", "party_size", "battles",
               "log", "save"}
STEP_COMMON = {"do", "max", "shot", "note"}
STEP_KEYS = {
    "press": {"keys", "hold", "gap", "times", "until"},
    "tap": {"x", "y", "hold", "gap", "times"},
    "wait_frames": {"n"},
    "wait_map": {"map"},
    "wait_field": set(),
    "slide": {"dirs", "on_battle", "on_text", "move"},
    "wait_battle": set(),
    "wait_reset": {"keys", "press", "gap"},
    "schedule": {"file", "frames"},
    "save": set(),
    "advance_text": {"through_battle", "map", "key"},
    "auto_battle": {"move", "wait", "flee", "send", "snap"},
    "walk_to": {"x", "z", "via", "map", "face", "interact", "run", "on_battle", "on_text", "move", "surf", "hm", "avoid",
                "hold", "dive"},
    "walk_to_door": {"pattern", "doors", "wait", "map", "face", "interact", "run", "on_battle", "on_text", "move"},
    "talk_to": {"id", "on_battle", "on_text", "move", "surf", "hm", "avoid"},
    "heal": {"x", "z", "on_battle", "map", "surf", "hm"},
    "grind": {"x", "z", "level", "heal", "move"},
    "fly": {"map", "slot", "block", "start", "dig"},
    "steps": {"route", "run", "on_battle", "on_text", "move", "face", "interact"},
    "moves": {"dirs", "on_battle", "on_text", "move", "face", "interact"},
    "hatch": {"x", "z"},
    "field_move": {"move", "slot", "text"},
    "fish": {"casts"},
    "pace": {"x", "z", "until", "every"},
    "dump": {"expr"},
    "menu": {"choose", "count"},
    "push": {"dir", "on_battle"},
    "smash": {"dir", "on_battle"},
    "repeat": {"until", "steps", "max_rounds"},
    "walk_onto": {"behavior", "max", "on_battle", "on_text", "run"},
    "rail": {"keys", "x", "z", "near", "map", "run", "script", "on_battle"},
}
STEP_REQUIRED = {"press": {"keys"}, "push": {"dir"}, "smash": {"dir"}, "repeat": {"until", "steps"}, "walk_onto": {"behavior"}, "tap": {"x", "y"}, "wait_map": {"map"}, "schedule": {"file"}, "slide": {"dirs"},
                 "walk_to": {"x", "z"}, "talk_to": {"id"}, "walk_to_door": {"pattern", "doors"},
                 "heal": {"x", "z"}, "grind": {"x", "z", "level"}, "fly": {"map"},
                 "steps": {"route"}, "moves": {"dirs"}, "hatch": {"x", "z"}, "field_move": {"move"},
                 "pace": {"x", "z", "until"}, "dump": {"expr"}}
NAME_KEYS = {"map"}  # step keys that take a game name
# np_gp -o options every run gets first ([run] options come after and win): message boxes print at once, so
# story scenes and battles cost their animations, not the text crawl. A recorded press schedule depends on the
# real text timing and sets "text_instant=0".
DEFAULT_OPTIONS = ["text_instant=1"]


def die(msg):
    print("run.py: %s" % msg, file=sys.stderr)
    sys.exit(2)


# ------------------------------------------------------------------ setup
class Game:
    def __init__(self, name):
        self.name = name
        rom, core = GAMES[name]
        self.rom = os.environ.get("NP_ROM") or os.path.join(ROOT, rom)
        self.core = os.environ.get("NP_CORE_BUILD") or os.path.join(ROOT, core)
        self.dir = os.path.join(HERE, name)
        self._resolve = None

    def resolve(self, token, where="milestone"):
        """A game name (MAP_*, FLAG_*, SPECIES_*, ...) or a number, as the lab recipes resolve it. Black/White have
        no name tables: numbers only (zone ids, flag and var ids, species as docs/BW_RAM.md gives them)."""
        if isinstance(token, int):
            return token
        if self.name in BW_GAMES:
            try:
                return int(str(token), 0)
            except ValueError:
                raise HarnessError("%s: %s has no names; give %r as a number" % (where, self.name, token))
        if self._resolve is None:
            self._resolve = labc.make_resolver(self.name)
        try:
            return self._resolve(str(token), where)
        except SystemExit as e:
            raise HarnessError(str(e))

    def tools(self):
        """np_gp for this core build (rebuilt when stale) and the save tool, a command prefix: np_save4 (D/P/Pt),
        np_save5 (Black/White), or tools/gba/gen3_dump.py (GBA)."""
        if not os.path.isfile(self.rom):
            die("no ROM at %s (set NP_ROM)" % self.rom)
        libs = sorted(glob.glob(os.path.join(self.core, "libnp_guest_*.a")))
        if not os.path.isfile(os.path.join(self.core, "libnp_guest_%s.a" % self.name)):
            die("no %s core build in %s" % (self.name, self.core))
        gp = os.path.join(ROOT, "build", "gameplay", "np_gp-%s" % os.path.basename(self.core))
        srcs = [os.path.join(GAMEPLAY, "np_gp.c"), os.path.join(ROOT, "core", "include", "np_e2e.h")] + libs + [
            os.path.join(self.core, "libnp_runtime.a")]
        if not os.path.isfile(gp) or os.path.getmtime(gp) < max(os.path.getmtime(p) for p in srcs):
            os.makedirs(os.path.dirname(gp), exist_ok=True)
            tmp = "%s.%d" % (gp, os.getpid())
            cmd = ["cc", "-O2", "-std=c11", "-Wall", "-I" + os.path.join(ROOT, "core", "include"),
                   "-I" + os.path.join(ROOT, "core", "runtime"), os.path.join(GAMEPLAY, "np_gp.c"),
                   os.path.join(self.core, "np_headless_np_registry.c")] + libs + [
                os.path.join(self.core, "libnp_runtime.a"), "-lm", "-lpthread", "-o", tmp]
            if subprocess.call(cmd) != 0:
                die("np_gp did not build")
            os.replace(tmp, gp)
        if self.name in GBA_GAMES:
            self.gp, self.save4 = gp, [sys.executable, GEN3_DUMP]
            return
        # np_save5 (features/save5) reads Black/White's saves with np_save4's commands and JSON shapes
        tool = "np_save5" if self.name in BW_GAMES else "np_save4"
        save4 = os.path.join(ROOT, "build", "features", tool)
        src4 = os.path.join(ROOT, "features", "tools", tool + ".c")
        if not os.path.isfile(save4) or os.path.getmtime(save4) < os.path.getmtime(src4):
            build = os.path.join(ROOT, "build", "features")
            if subprocess.call(["cmake", "-S", os.path.join(ROOT, "features"), "-B", build, "-G", "Ninja"],
                               stdout=subprocess.DEVNULL) != 0 or subprocess.call(
                    ["cmake", "--build", build, "--target", tool], stdout=subprocess.DEVNULL) != 0:
                die("%s did not build" % tool)
        self.gp, self.save4 = gp, [save4]


class Milestone:
    def __init__(self, game, entry):
        self.entry = entry
        self.dir = os.path.normpath(os.path.join(game.dir, entry))
        self.name = os.path.basename(self.dir)
        path = os.path.join(self.dir, "milestone.toml")
        try:
            with open(path, "rb") as f:
                self.data = tomllib.load(f)
        except (OSError, tomllib.TOMLDecodeError) as e:
            raise HarnessError("%s: %s" % (path, e))
        self.title = self.data.get("title", self.name)

    def check(self, game):
        """Every problem --check finds without running anything."""
        d, problems = self.data, []
        for k in d:
            if k not in TOP_KEYS:
                problems.append("unknown top-level key %r" % k)
        for table, keys in (("start", START_KEYS), ("run", RUN_KEYS), ("expect", EXPECT_KEYS)):
            if not isinstance(d.get(table, {}), dict):
                problems.append("[%s] is not a table" % table)
                continue
            for k in d.get(table, {}):
                if k not in keys:
                    problems.append("unknown key %s.%s" % (table, k))
        st = d.get("start", {})
        if sum(1 for k in ("recipe", "blank") if st.get(k)) > 1:
            problems.append("[start] names both recipe and blank")
        for k in ("recipe", "lab"):
            if k in st and not os.path.isfile(os.path.join(self.dir, st[k])):
                problems.append("[start] %s file %s is missing" % (k, st[k]))
        for k in ("recipe", "lab"):
            if k in st and os.path.isfile(os.path.join(self.dir, st[k])):
                try:
                    labc.compile_inline(os.path.join(self.dir, st[k]), game.name)
                except SystemExit as e:
                    problems.append("[start] %s: %s" % (k, e))
        if "boost" in st:
            path = os.path.join(self.dir, st["boost"])
            if not os.path.isfile(path):
                problems.append("[start] boost file %s is missing" % st["boost"])
            else:
                try:
                    inline, env = recipe_env(path, game)
                except SystemExit as e:
                    problems.append("[start] boost: %s" % e)
                else:
                    allowed = addmon_boost_verbs(game) if game.name in ADDMON_BOOST_GAMES else BOOST_VERBS
                    bad = sorted({op.split()[0] for op in inline[len("inline:"):].split(";") if op} - allowed)
                    if bad or env:
                        problems.append("[start] boost %s: only %s, not %s" % (
                            st["boost"], " ".join(sorted(allowed)), " ".join(bad + sorted(env))))
        if "frames" not in d.get("run", {}) and d.get("status") != "planned":
            problems.append("[run] frames (the frame budget) is required")
        for i, s in enumerate(d.get("step", [])):
            where = "step %d (%s)" % (i + 1, s.get("do"))
            if s.get("do") not in STEP_KEYS:
                problems.append("%s: unknown bot %r" % (where, s.get("do")))
                continue
            for k in s:
                if k not in STEP_COMMON | STEP_KEYS[s["do"]]:
                    problems.append("%s: unknown key %r" % (where, k))
            for k in STEP_REQUIRED.get(s["do"], ()):
                if k not in s:
                    problems.append("%s: needs %r" % (where, k))
            for k in NAME_KEYS & set(s):
                try:
                    game.resolve(s[k], where)
                except HarnessError as e:
                    problems.append("%s: %s" % (where, e))
            if s["do"] == "schedule" and "file" in s and not os.path.isfile(os.path.join(self.dir, s["file"])):
                problems.append("%s: schedule %s is missing" % (where, s["file"]))
        ex = d.get("expect", {})
        names = []
        if "map" in ex:
            names.append(ex["map"])
        names += list(ex.get("badge", [])) + list(ex.get("flags", [])) + list(ex.get("flags_clear", []))
        names += list(ex.get("vars", {}).keys()) + list(ex.get("party", []))
        for n in names:
            try:
                game.resolve(n, "[expect]")
            except HarnessError as e:
                problems.append("[expect]: %s" % e)
        for expr in ex.get("save", []):
            try:
                compile(expr, "<expect.save>", "eval")
            except SyntaxError as e:
                problems.append("[expect] save expression %r: %s" % (expr, e))
        for rx in ex.get("log", []):
            try:
                re.compile(rx)
            except re.error as e:
                problems.append("[expect] log regex %r: %s" % (rx, e))
        return problems


def read_list(path):
    out = []
    if os.path.isfile(path):
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    out.append(line)
    return out


# ------------------------------------------------------------------ minting
def dp_base_save(game, out):
    """Diamond/Pearl's new-game save, the base its lab recipes apply to (as tests/gameplay/run.sh makes it)."""
    base = os.path.join(out, "base.sav")
    if os.path.isfile(base):
        return base
    sched = os.path.join(ROOT, "tests", "dp", "%s_first_save.sched" % game.name)
    frames = None
    with open(sched) as f:
        for line in f:
            m = re.match(r"# frames: *(\d+)", line)
            if m:
                frames = int(m.group(1))
    tmp = base + ".tmp"
    with open(os.path.join(out, "base.log"), "w") as log:
        subprocess.call([game.gp, game.rom, "--game", game.name, "--frames", str(frames), "--save", tmp,
                         "--schedule", sched], stdout=subprocess.DEVNULL, stderr=log)
    if "stored 524288-byte save" not in open(os.path.join(out, "base.log")).read():
        raise HarnessError("the D/P new-game base save failed (%s/base.log)" % out)
    os.replace(tmp, base)
    return base


def gba_base_save(game, out):
    """A Ruby/Sapphire/Emerald new game saved in the house 1F (tests/rse/first_battle.sh's quicksave leg: the boot
    schedule tests/rse/littleroot.sched, the game's own save at frame 10000), the base GBA lab recipes apply to."""
    base = os.path.join(out, "base.sav")
    if os.path.isfile(base):
        return base
    tmp = base + ".tmp"
    if os.path.exists(tmp):
        os.remove(tmp)
    log_path = os.path.join(out, "base.log")
    with open(log_path, "w") as log:
        subprocess.call([game.gp, game.rom, "--game", game.name, "--frames", "10600", "--save", tmp, "--schedule",
                         os.path.join(ROOT, "tests", "rse", "littleroot.sched"), "-o", "10000:quicksave_seq=1"],
                        stdout=subprocess.DEVNULL, stderr=log)
    if not re.search(r"quicksave_result 0 -> 1", open(log_path).read()) or not os.path.isfile(tmp):
        raise HarnessError("the %s new-game base save failed (%s)" % (game.name, log_path))
    os.replace(tmp, base)
    return base



def recipe_env(path, game):
    """(inline recipe, env) from a recipe file; env carries a `clock` line as PC_RTC."""
    if hasattr(labc, "compile_recipe"):
        return labc.compile_recipe(path, game.name)
    return labc.compile_inline(path, game.name), {}


# HG/SS and B/W boosts (no save lab for them): `party SPECIES LEVEL` adds a Pokemon behind the party as the game's
# own gift makes it, with the `party-move SLOT INDEX MOVE` lines that name its slot as its moves (np_save4/np_save5
# add-mon). HG/SS also edits the members the save's party already holds: `party-move` there sets that move with
# full PP (np_save4 set-move), and `party-level SLOT LEVEL` sets the level, stats recalculated and HP full (np_save4
# set-level); `party-heal` restores the whole party as a Pokemon Center does (np_save4 heal-party), standing in for
# the Full Restores and Revives a player uses where no Center is reachable (the Elite Four's rooms), since no bot
# uses items. B/W edits the members its save holds with np_save5: `party-level SLOT LEVEL` (set-level, as HG/SS's),
# and `party-set SLOT SPECIES LEVEL [MOVE...]` (set-mon) replaces that member with a Pokemon made as add-mon makes
# one, carrying those moves: an HM carrier (Fly, Surf, Strength) for a full party, which add-mon cannot grow.
# Nothing else is edited.
ADDMON_BOOST_VERBS = {"party", "party-move"}
HGSS_BOOST_VERBS = ADDMON_BOOST_VERBS | {"party-level", "party-heal"}
# np_save5 set-level / set-mon: a full party (6 from B/W milestone 13 on) is strengthened in place
BW_BOOST_VERBS = ADDMON_BOOST_VERBS | {"party-level", "party-set"}
ADDMON_BOOST_GAMES = HGSS_GAMES + BW_GAMES


def addmon_boost_verbs(game):
    return HGSS_BOOST_VERBS if game.name in HGSS_GAMES else BW_BOOST_VERBS


def addmon_boost(game, inline, sav, log):
    """Applies an HG/SS or B/W boost recipe to `sav` with the save tool's add-mon: each `party` line becomes an
    add-mon after the save's party, carrying the moves its `party-move` lines give it (slot = its party slot)."""
    ops = [op.split() for op in inline[len("inline:"):].split(";") if op]
    out = subprocess.run(game.save4 + ["dump", game.rom, sav], capture_output=True, text=True)
    if out.returncode != 0:
        raise HarnessError("boost: the save tool cannot read %s" % sav)
    first = len(json.loads(out.stdout).get("party", []))
    adds, sets, levels, replaces, heal = [], [], [], [], False
    allowed = addmon_boost_verbs(game)
    for verb, *args in ops:
        if verb not in allowed:
            raise HarnessError("boost: %s boosts take only %s, not %s" % (
                game.name, " ".join(sorted(allowed)), verb))
        if verb == "party":
            adds.append((int(args[0]), int(args[1]), {}))
            continue
        if verb == "party-heal":
            heal = True
            continue
        if verb == "party-level":
            slot, level = int(args[0]), int(args[1])
            if not 0 <= slot < first or not 1 <= level <= 100:
                raise HarnessError("boost: party-level %d %d: not a slot the save's party holds, or no such level"
                                   % (slot, level))
            levels.append((slot, level))
            continue
        if verb == "party-set":
            slot, species, level = (int(a) for a in args[:3])
            moves = args[3:]
            if not 0 <= slot < first or not 1 <= level <= 100 or len(moves) > 4:
                raise HarnessError("boost: party-set %d %d %d: not a slot the save's party holds, no such level, or "
                                   "more than 4 moves" % (slot, species, level))
            replaces.append([str(slot), str(species), str(level)] + moves)
            continue
        slot, index, move = (int(a) for a in args[:3])
        if not 0 <= index < 4 or not 0 <= slot < first + len(adds):
            raise HarnessError("boost: party-move %d %d: no such party slot or move index" % (slot, index))
        if slot < first:
            if game.name not in HGSS_GAMES:
                raise HarnessError("boost: party-move %d on a member the save holds: np_save4 (HG/SS) only" % slot)
            sets.append((slot, index, move))
        else:
            adds[slot - first][2][index] = move
    if first + len(adds) > 6:
        raise HarnessError("boost: %d Pokemon after a party of %d" % (len(adds), first))
    with open(log, "w") as f:
        edits = [["set-move", str(slot), str(index), str(move)] for slot, index, move in sets]
        edits += [["set-mon"] + args for args in replaces]
        edits += [["set-level", str(slot), str(level)] for slot, level in levels]
        edits += [["heal-party"]] if heal else []
        for verb, *args in edits:
            cmd = game.save4 + [verb, sav, game.rom] + args
            f.write("$ %s\n" % " ".join(cmd))
            f.flush()
            if subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT) != 0:
                raise HarnessError("boost: %s %s failed (%s)" % (os.path.basename(game.save4[-1]), verb, log))
        for species, level, moves in adds:
            if sorted(moves) != list(range(len(moves))):
                raise HarnessError("boost: the moves of slot %d must be indices 0..n-1" % first)
            cmd = game.save4 + ["add-mon", sav, game.rom, str(species), str(level)] + [str(moves[i]) for i in
                                                                                      range(len(moves))]
            f.write("$ %s\n" % " ".join(cmd))
            f.flush()
            if subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT) != 0:
                raise HarnessError("boost: %s add-mon failed (%s)" % (os.path.basename(game.save4[-1]), log))



def mint(game, recipe, out_sav, base, workdir):
    """Applies a lab recipe: on a new game (Platinum, base None) or on the save `base`. Returns the env it used."""
    inline, env = recipe_env(recipe, game)
    log = out_sav + ".log"
    work = out_sav + ".mint"
    if os.path.exists(work):
        os.remove(work)
    run_env = dict(os.environ, PC_LAB=inline, PC_LAB_AT="1800", **env)
    if game.name in ADDMON_BOOST_GAMES:
        if base is None:
            # no HG/SS or B/W save lab (Platinum's pc_lab.c / D/P's pc_dp_lab.c have no twin there): the chain runs
            # from the previous milestone's end save
            raise HarnessError("minting %s: %s has no save lab yet; start from the previous milestone's end save"
                               % (os.path.basename(recipe), game.name))
        shutil.copyfile(base, work)
        addmon_boost(game, inline, work, log)
        os.replace(work, out_sav)
        return env
    if game.name == "platinum":
        cmd = [game.gp, game.rom, "--frames", "6000", "--save", work]
        if base:
            shutil.copyfile(base, work)
        else:
            run_env["PC_INPUT"] = "inline:" + ";".join(
                "%d keys A;%d keys none" % (f, f + 12) for f in range(120, 1500, 40))
    elif game.name in GBA_GAMES:
        # the GBA lab edits the save itself (tools/gba/gen3_lab.py): the game reads it back on CONTINUE
        shutil.copyfile(base or gba_base_save(game, workdir), work)
        with open(log, "w") as f:
            rc = subprocess.call([sys.executable, GEN3_LAB, game.rom, work, work, inline], stdout=f,
                                 stderr=subprocess.STDOUT)
        if rc != 0:
            raise HarnessError("minting %s failed: %s" % (os.path.basename(recipe), open(log).read().strip()[-300:]))
        os.replace(work, out_sav)
        return env
    else:
        shutil.copyfile(base or dp_base_save(game, workdir), work)
        cmd = [game.gp, game.rom, "--game", game.name, "--frames", "9000", "--save", work, "--schedule",
               os.path.join(GAMEPLAY, "dp", "schedules", "continue.press")]
    with open(log, "w") as f:
        subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, env=run_env)
    text = open(log).read()
    if not re.search(r"pc_lab: applied .* save ok", text):
        tail = [line for line in text.splitlines() if "pc_lab" in line][-3:]
        raise HarnessError("minting %s failed: %s" % (os.path.basename(recipe), " | ".join(tail) or "see " + log))
    os.replace(work, out_sav)
    return env


# ------------------------------------------------------------------ one milestone
class Ctx:
    def __init__(self, game, ms):
        self.game = game.name
        self.dir = ms.dir
        self.rom, self.save4 = game.rom, game.save4
        self._game = game

    def resolve(self, token):
        return self._game.resolve(token)


# Frames before the title takes START: D/P/Pt's title is up by 1250; Black/White's opening movie runs to about
# frame 4750 (docs/BW_PLAN.md) and the proven CONTINUE run pressed START at 5000.
TITLE_FRAMES = {"black": 5000, "white": 5000}


def boot_continue(s):
    """Title screen, then CONTINUE, until the player is free (tests/gameplay/schedules/continue.press timings)."""
    if s.game in GBA_GAMES:
        return gba_boot_continue(s)
    if s.run(TITLE_FRAMES.get(s.game, 1250), until="field_ready=1"):
        return
    s.run(4, "start")
    if s.run(146, until="field_ready=1"):
        return
    for _ in range(40):
        s.run(4, "a", until="field_ready=1")
        if s.run(96, until="field_ready=1"):
            return
    raise HarnessError("CONTINUE never reached a free player")


def gba_boot_continue(s):
    """Ruby/Sapphire/Emerald: START past the intro and the title, then A on the main menu, whose cursor starts on
    CONTINUE, until the player is free (tests/rse/e-1-home.sched's 400 / 700 / 900 / 1000; free at 1051)."""
    s.run(400)
    s.run(5, "start")
    s.run(295)
    s.run(5, "start")
    s.run(195)
    for _ in range(8):
        s.run(5, "a", until="field_ready=1")
        if s.run(95, until="field_ready=1"):
            return
    if s.run(600, until="field_ready=1"):
        return
    raise HarnessError("CONTINUE never reached a free player")


def ppm_to_png(path):
    png = path[:-4] + ".png"
    if subprocess.call(["sips", "-s", "format", "png", path, "--out", png], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL) == 0:
        os.remove(path)
        return png
    return path


def contact_sheet(d, name, shots, gba=False):
    if not shots:
        return None
    sheet = os.path.join(d, name + ".png")
    args = ["montage"]
    for label, path in shots:
        args += ["-label", label, path]
    args += ["-tile", "6x", "-geometry", "240x160+3+3" if gba else "256x384+3+3", "-pointsize", "11", "-fill",
             "#e0e0e0", "-background", "#202020", sheet]
    if subprocess.call(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
        return None
    return sheet


class Result:
    def __init__(self, name, title):
        self.name, self.title = name, title
        self.status = "FAIL"
        self.reason = ""
        self.frames = 0
        self.seconds = 0.0
        self.steps = []   # (n, do, frames, result)
        self.sheet = None
        self.meta = {}
        self.started = ""


def start_save(game, ms, prev, args, out, d, res):
    """Places start.sav (or nothing, for a blank chip), then applies the [start] boost to it; returns the clock
    env carried in."""
    env, start = place_start(game, ms, prev, args, out, d, res)
    boost = ms.data.get("start", {}).get("boost")
    if boost:
        if not start:
            raise HarnessError("[start] boost needs a start save")
        mint(game, os.path.join(ms.dir, boost), start, start, out)
        res.started += " + boost %s" % boost
    return env, start


def place_start(game, ms, prev, args, out, d, res):
    """Places start.sav (or nothing, for a blank chip); returns the clock env carried in."""
    st = ms.data.get("start", {})
    start = os.path.join(d, "start.sav")
    env = {}
    src = None
    if st.get("blank"):
        res.started = "blank chip"
        return env, None
    use_lab = "lab" in st and args.lab
    frm = st.get("from", "prev" if prev is not None and "recipe" not in st else None)
    if frm and not use_lab:
        src_name = prev if frm == "prev" else frm
        cand = os.path.join(out, src_name, "end.sav") if src_name else None
        if cand and os.path.isfile(cand):
            src = cand
            res.started = "end save of %s" % src_name
            clock = os.path.join(out, src_name, "end.clock")
            if os.path.isfile(clock):
                env["PC_RTC"] = open(clock).read().strip()
        elif "lab" in st:
            use_lab = True
        elif "recipe" not in st:
            raise HarnessError("no end save from %s (run it first, or give [start] lab)" % (src_name or "a previous milestone"))
    if use_lab:
        res.started = "lab recipe %s" % st["lab"]
        env.update(mint(game, os.path.join(ms.dir, st["lab"]), start, None, out))
        if "recipe" in st:
            env.update(mint(game, os.path.join(ms.dir, st["recipe"]), start, start, out))
            res.started += " + recipe %s" % st["recipe"]
        return env, start
    if "recipe" in st:
        env.update(mint(game, os.path.join(ms.dir, st["recipe"]), start, src, out))
        res.started = ("recipe %s" % st["recipe"]) + (" on the " + res.started if src else "")
        return env, start
    if src:
        shutil.copyfile(src, start)
        return env, start
    res.started = "blank chip"
    return env, None


def judge(game, ms, end_state, log_path, d, end_save):
    """[expect] against the run's end state: (map id, probe) taken before np_gp quit, its log, the end save
    np_gp wrote on exit. Returns the first unmet expectation, or ''."""
    ex = ms.data.get("expect", {})
    log = open(log_path, errors="replace").read()
    map_id, p = end_state
    if "map" in ex and map_id != game.resolve(ex["map"]):
        return "ended on map %d, expected %s (%d)" % (map_id, ex["map"], game.resolve(ex["map"]))
    if "position" in ex:
        if p is None or [p.x, p.z] != list(ex["position"]):
            return "ended at %s, expected %s" % ((p.x, p.z) if p else "?", tuple(ex["position"]))
    if "battles" in ex:
        n = len(re.findall(r"\[status\] frame \d+: in_battle 0 -> 1", log))
        if n < int(ex["battles"]):
            return "%d battle(s), expected at least %d" % (n, int(ex["battles"]))
    for rx in ex.get("log", []):
        if not re.search(rx, log, re.M):
            return "log lacks /%s/" % rx
    save_keys = {"badges", "badge", "flags", "flags_clear", "vars", "party", "party_size", "save"}
    if not save_keys & set(ex):
        return ""
    if not end_save or not os.path.isfile(end_save):
        return "no end save to check"
    dump = os.path.join(d, "end.json")
    with open(dump, "w") as f:
        if subprocess.call(game.save4 + ["dump", game.rom, end_save], stdout=f, stderr=subprocess.STDOUT) != 0:
            return "the save tool cannot read the end save"
    sj = json.load(open(dump))
    tr = sj.get("trainer", {})
    if "badges" in ex and tr.get("badges") != int(ex["badges"]):
        return "%s badge(s), expected %d" % (tr.get("badges"), int(ex["badges"]))
    for b in ex.get("badge", []):
        if not tr.get("badge_mask", 0) >> game.resolve(b) & 1:
            return "badge %s missing (mask %#x)" % (b, tr.get("badge_mask", 0))
    flags = set(sj.get("flags", []))
    for fl in ex.get("flags", []):
        if game.resolve(fl) not in flags:
            return "flag %s (%d) not set" % (fl, game.resolve(fl))
    for fl in ex.get("flags_clear", []):
        if game.resolve(fl) in flags:
            return "flag %s (%d) set" % (fl, game.resolve(fl))
    vars_ = sj.get("vars", {})
    for name, want in ex.get("vars", {}).items():
        got = vars_.get(str(game.resolve(name)), 0)
        if got != game.resolve(want):
            return "var %s = %d, expected %s" % (name, got, want)
    party = [m.get("species") for m in sj.get("party", [])]
    for sp in ex.get("party", []):
        if game.resolve(sp) not in party:
            return "%s not in the party %s" % (sp, [m.get("species_name") for m in sj.get("party", [])])
    if "party_size" in ex and len(party) != int(ex["party_size"]):
        return "party of %d, expected %d" % (len(party), int(ex["party_size"]))
    for expr in ex.get("save", []):
        try:
            ok = eval(expr, {"s": sj})
        except Exception as e:  # a missing key is a failed expectation, said as such
            return "save: not (%s) (%s: %s)" % (expr, type(e).__name__, e)
        if not ok:
            return "save: not (%s)" % expr
    return ""


def run_milestone(game, ms, prev, args, out):
    res = Result(ms.name, ms.title)
    res.meta = {k: ms.data[k] for k in ("priority", "estimate", "refs", "version") if k in ms.data}
    d = os.path.join(out, ms.name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    version = ms.data.get("version", "both")
    planned = ms.data.get("status") == "planned" and not args.planned
    if planned or version not in ("both", game.name):
        res.status = "SKIP"
        res.reason = "planned" if planned else "%s only" % version
        return res
    t0 = time.time()
    run = ms.data.get("run", {})
    s = None
    shots = []
    end = os.path.join(d, "end.sav")
    try:
        env, start = start_save(game, ms, prev, args, out, d, res)
        extra = run.get("env", {})  # extra guest env: a table, or a list of "NAME=VALUE"
        if isinstance(extra, list):
            extra = dict(e.split("=", 1) for e in extra)
        env.update({str(k): str(v) for k, v in extra.items()})
        if "clock" in run:
            env["PC_RTC"] = run["clock"]
        if start:
            shutil.copyfile(start, end)
        s = Session(game.gp, game.rom, game.name, end, os.path.join(d, "run.log"), int(run["frames"]),
                    options=DEFAULT_OPTIONS + run.get("options", []), env=env)
        s.shot_dir = d
        for tbl in ms.data.get("shots", []):
            s.shot_frames += [int(f) for f in tbl.get("frames", [])]
        s.shot_frames.sort()
        s.note("milestone %s: started from %s" % (ms.name, res.started))
        boot = ms.data.get("start", {}).get("boot", "continue" if start else "none")
        if boot == "continue":
            boot_continue(s)
        if s.frame > 0:  # a blank chip has no frame yet
            shot = os.path.join(d, "s00-start.ppm")
            s.dump(shot)
            shots.append(("start f%d" % s.frame, shot))
        ctx = Ctx(game, ms)
        for i, step in enumerate(ms.data.get("step", []), 1):
            if s.ended:
                raise HarnessError("step %d (%s): the game already ended (%s)" % (i, step["do"], s.ended))
            f0 = s.frame
            s.note("step %d: %s %s" % (i, step["do"], {k: v for k, v in step.items() if k != "do"}))
            try:
                BOTS[step["do"]](s, step, ctx)
            except HarnessError as e:
                res.steps.append((i, step["do"], s.frame - f0, "FAIL: %s" % e))
                fail = os.path.join(d, "s%02d-fail.ppm" % i)
                s.dump(fail)
                shots.append(("step %d FAIL f%d" % (i, s.frame), fail))
                raise HarnessError("step %d (%s): %s" % (i, step["do"], e))
            res.steps.append((i, step["do"], s.frame - f0, "ok"))
            if step.get("shot") and not s.ended:
                path = os.path.join(d, "s%02d-%s.ppm" % (i, re.sub(r"[^\w.-]", "_", str(step["shot"]))))
                s.dump(path)
                shots.append(("%d %s f%d" % (i, step["shot"], s.frame), path))
        # a step list that ends the game (bots.bot_wait_reset: the guest rebooted) takes no end save or probe: the
        # save is the one the game wrote
        if run.get("save", "quick") == "quick" and not s.ended:
            f0 = s.frame
            BOTS["save"](s, {}, ctx)
            res.steps.append((len(res.steps) + 1, "save (end)", s.frame - f0, "ok"))
        shot = os.path.join(d, "s99-end.ppm")
        s.dump(shot)
        shots.append(("end f%d" % s.frame, shot))
        res.frames = s.frame
        end_state = (s.map_id, None if s.ended else s.probe())
        if env.get("PC_RTC"):
            with open(os.path.join(d, "end.clock"), "w") as f:
                f.write(env["PC_RTC"] + "\n")
        s.quit()  # np_gp exits: the end save is on disk
        why = judge(game, ms, end_state, s.log_path, d, end)
        if s.defects:
            why = why or s.defects[0]
        if why:
            raise HarnessError(why)
        res.status = "PASS"
    except (HarnessError, Budget, Dead) as e:
        res.reason = str(e)
        if s is not None:
            res.frames = s.frame
    finally:
        if s is not None:
            s.kill()
    if res.status != "PASS" and os.path.isfile(end):
        # the next milestone must not continue a failed run: it falls back to its lab recipe
        os.replace(end, os.path.join(d, "failed.sav"))
    res.seconds = time.time() - t0
    for path in sorted(glob.glob(os.path.join(d, "frame_*.ppm"))):
        shots.append((os.path.basename(path)[6:-4].lstrip("0") or "0", path))
    shots = [(label, ppm_to_png(p)) for label, p in shots if os.path.isfile(p)]
    res.sheet = contact_sheet(d, ms.name, shots, gba=game.name in GBA_GAMES)
    with open(os.path.join(d, "steps.txt"), "w") as f:
        f.write("milestone %s: %s (started from %s)\n" % (ms.name, res.status, res.started))
        for n, do, frames, result in res.steps:
            f.write("%3d %-14s %7d  %s\n" % (n, do, frames, result))
        if res.reason:
            f.write("reason: %s\n" % res.reason)
    return res


# ------------------------------------------------------------------ report
def git_rev():
    try:
        return subprocess.check_output(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "?"


def write_report(game, out, results, list_name):
    path = os.path.join(out, "report.md")
    passed = sum(r.status == "PASS" for r in results)
    failed = sum(r.status == "FAIL" for r in results)
    with open(path, "w") as f:
        f.write("# e2e %s: %s\n\n" % (game.name, list_name))
        f.write("%s, rev %s: %d passed, %d failed, %d skipped.\n\n" % (
            datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), git_rev(), passed, failed,
            len(results) - passed - failed))
        f.write("| milestone | result | frames | seconds | started from | reason |\n|---|---|---:|---:|---|---|\n")
        for r in results:
            f.write("| %s | %s | %d | %.0f | %s | %s |\n" % (r.name, r.status, r.frames, r.seconds, r.started or "-",
                                                            r.reason.replace("|", "/") or "-"))
        for r in results:
            if r.status == "SKIP":
                continue
            f.write("\n## %s: %s (%s)\n\n" % (r.name, r.title, r.status))
            if r.meta:
                f.write("%s\n\n" % ", ".join("%s: %s" % kv for kv in r.meta.items()))
            if r.steps:
                f.write("| step | bot | frames | result |\n|---:|---|---:|---|\n")
                for n, do, frames, result in r.steps:
                    f.write("| %d | %s | %d | %s |\n" % (n, do, frames, result.replace("|", "/")))
                f.write("\n")
            if r.sheet:
                f.write("![%s](%s/%s)\n" % (r.name, r.name, os.path.basename(r.sheet)))
    return path


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True, choices=sorted(GAMES))
    ap.add_argument("--from", dest="from_", metavar="M", help="start the chain at milestone M")
    ap.add_argument("--only", nargs="+", metavar="M", help="run only these milestones (any directory name)")
    ap.add_argument("--planned", action="store_true", help="also run milestones still marked status = \"planned\"")
    ap.add_argument("--systems", action="store_true", help="run systems.txt instead of chain.txt")
    ap.add_argument("--lab", action="store_true", help="start each milestone from its [start] lab recipe")
    ap.add_argument("--out", help="output directory (default build/e2e/<game>)")
    ap.add_argument("--check", action="store_true", help="parse and check every milestone, run nothing")
    args = ap.parse_args()

    game = Game(args.game)
    list_name = "systems.txt" if args.systems else "chain.txt"
    entries = read_list(os.path.join(game.dir, list_name))
    if args.check:
        everything = entries + [e for e in read_list(os.path.join(game.dir, "systems.txt" if not args.systems
                                                                  else "chain.txt")) if e not in entries]
        bad = 0
        for e in everything:
            try:
                problems = Milestone(game, e).check(game)
            except HarnessError as err:
                problems = [str(err)]
            print("%-28s %s" % (e, "ok" if not problems else "; ".join(problems)))
            bad += bool(problems)
        sys.exit(1 if bad else 0)
    if args.only:
        by_name = {os.path.basename(os.path.normpath(e)): e for e in entries + read_list(
            os.path.join(game.dir, "systems.txt"))}
        entries = [by_name.get(m, m) for m in args.only]
    elif args.from_:
        names = [os.path.basename(os.path.normpath(e)) for e in entries]
        if args.from_ not in names:
            die("%s is not in %s" % (args.from_, list_name))
        entries = entries[names.index(args.from_):]
    if not entries:
        die("nothing to run (%s is empty?)" % os.path.join(game.dir, list_name))

    out = os.path.abspath(args.out or os.path.join(ROOT, "build", "e2e", game.name))
    os.makedirs(out, exist_ok=True)
    game.tools()
    all_entries = read_list(os.path.join(game.dir, list_name))
    all_names = [os.path.basename(os.path.normpath(e)) for e in all_entries]
    results = []
    for e in entries:
        try:
            ms = Milestone(game, e)
        except HarnessError as err:
            r = Result(os.path.basename(e), e)
            r.reason = str(err)
            results.append(r)
            continue
        # "prev" is the entry before this one in the list (also under --only / --from)
        prev = None
        if ms.name in all_names and all_names.index(ms.name) > 0:
            prev = all_names[all_names.index(ms.name) - 1]
        r = run_milestone(game, ms, prev, args, out)
        print("%-28s %-4s %7d frames %5.0fs  %s" % (ms.name, r.status, r.frames, r.seconds,
                                                   r.reason or (r.sheet or "")), flush=True)
        results.append(r)
    report = write_report(game, out, results, list_name)
    print("report: %s" % report)
    sys.exit(1 if any(r.status == "FAIL" for r in results) else 0)


if __name__ == "__main__":
    main()
