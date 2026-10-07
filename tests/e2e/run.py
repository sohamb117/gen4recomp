#!/usr/bin/env python3
"""Milestone chains on the real core, headless (tests/e2e/README.md).

    tests/e2e/run.py --game G [--from M] [--only M ...] [--systems] [--lab] [--planned] [--out DIR] [--check]

G is platinum, diamond or pearl. The chain is tests/e2e/<G>/chain.txt (side
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
from np_e2e import Budget, Dead, HarnessError, Session  # noqa: E402

GAMES = {
    "platinum": ("games/platinum/build/rom/pokeplatinum.us.nds", "build/core-plat"),
    "diamond": ("games/diamond/build/diamond.us/pokediamond.us.nds", "build/core-dp"),
    "pearl": ("games/diamond/build/pearl.us/pokepearl.us.nds", "build/core-dp"),
}

TOP_KEYS = {"title", "notes", "status", "priority", "estimate", "refs", "version", "start", "run", "step", "expect",
            "shots"}
START_KEYS = {"from", "recipe", "lab", "blank", "boot"}
RUN_KEYS = {"frames", "save", "options", "clock", "env"}
EXPECT_KEYS = {"map", "position", "badges", "badge", "flags", "flags_clear", "vars", "party", "party_size", "battles",
               "log", "save"}
STEP_COMMON = {"do", "max", "shot", "note"}
STEP_KEYS = {
    "press": {"keys", "hold", "gap", "times"},
    "tap": {"x", "y", "hold", "gap", "times"},
    "wait_frames": {"n"},
    "wait_map": {"map"},
    "wait_field": set(),
    "wait_battle": set(),
    "schedule": {"file", "frames"},
    "save": set(),
    "advance_text": {"through_battle"},
    "auto_battle": {"move", "wait"},
    "walk_to": {"x", "z", "map", "face", "interact", "run", "on_battle", "on_text"},
}
STEP_REQUIRED = {"press": {"keys"}, "tap": {"x", "y"}, "wait_map": {"map"}, "schedule": {"file"},
                 "walk_to": {"x", "z"}}
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
        """A game name (MAP_*, FLAG_*, SPECIES_*, ...) or a number, as the lab recipes resolve it."""
        if isinstance(token, int):
            return token
        if self._resolve is None:
            self._resolve = labc.make_resolver(self.name)
        try:
            return self._resolve(str(token), where)
        except SystemExit as e:
            raise HarnessError(str(e))

    def tools(self):
        """np_gp for this core build (rebuilt when stale) and np_save4."""
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
        save4 = os.path.join(ROOT, "build", "features", "np_save4")
        src4 = os.path.join(ROOT, "features", "tools", "np_save4.c")
        if not os.path.isfile(save4) or os.path.getmtime(save4) < os.path.getmtime(src4):
            build = os.path.join(ROOT, "build", "features")
            if subprocess.call(["cmake", "-S", os.path.join(ROOT, "features"), "-B", build, "-G", "Ninja"],
                               stdout=subprocess.DEVNULL) != 0 or subprocess.call(
                    ["cmake", "--build", build, "--target", "np_save4"], stdout=subprocess.DEVNULL) != 0:
                die("np_save4 did not build")
        self.gp, self.save4 = gp, save4


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


def recipe_env(path, game):
    """(inline recipe, env) from a recipe file; env carries a `clock` line as PC_RTC."""
    if hasattr(labc, "compile_recipe"):
        return labc.compile_recipe(path, game.name)
    return labc.compile_inline(path, game.name), {}


def mint(game, recipe, out_sav, base, workdir):
    """Applies a lab recipe: on a new game (Platinum, base None) or on the save `base`. Returns the env it used."""
    inline, env = recipe_env(recipe, game)
    log = out_sav + ".log"
    work = out_sav + ".mint"
    if os.path.exists(work):
        os.remove(work)
    run_env = dict(os.environ, PC_LAB=inline, PC_LAB_AT="1800", **env)
    if game.name == "platinum":
        cmd = [game.gp, game.rom, "--frames", "6000", "--save", work]
        if base:
            shutil.copyfile(base, work)
        else:
            run_env["PC_INPUT"] = "inline:" + ";".join(
                "%d keys A;%d keys none" % (f, f + 12) for f in range(120, 1500, 40))
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
        self._game = game

    def resolve(self, token):
        return self._game.resolve(token)


def boot_continue(s):
    """Title screen, then CONTINUE, until the player is free (tests/gameplay/schedules/continue.press timings)."""
    if s.run(1250, until="field_ready=1"):
        return
    s.run(4, "start")
    if s.run(146, until="field_ready=1"):
        return
    for _ in range(40):
        s.run(4, "a", until="field_ready=1")
        if s.run(96, until="field_ready=1"):
            return
    raise HarnessError("CONTINUE never reached a free player")


def ppm_to_png(path):
    png = path[:-4] + ".png"
    if subprocess.call(["sips", "-s", "format", "png", path, "--out", png], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL) == 0:
        os.remove(path)
        return png
    return path


def contact_sheet(d, name, shots):
    if not shots:
        return None
    sheet = os.path.join(d, name + ".png")
    args = ["montage"]
    for label, path in shots:
        args += ["-label", label, path]
    args += ["-tile", "6x", "-geometry", "256x384+3+3", "-pointsize", "11", "-fill", "#e0e0e0",
             "-background", "#202020", sheet]
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
        if subprocess.call([game.save4, "dump", game.rom, end_save], stdout=f, stderr=subprocess.STDOUT) != 0:
            return "np_save4 cannot read the end save"
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
        env.update({str(k): str(v) for k, v in run.get("env", {}).items()})  # extra guest env (PC_LAB_BATTLE, ...)
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
            if step.get("shot"):
                path = os.path.join(d, "s%02d-%s.ppm" % (i, re.sub(r"[^\w.-]", "_", str(step["shot"]))))
                s.dump(path)
                shots.append(("%d %s f%d" % (i, step["shot"], s.frame), path))
        if run.get("save", "quick") == "quick":
            f0 = s.frame
            BOTS["save"](s, {}, ctx)
            res.steps.append((len(res.steps) + 1, "save (end)", s.frame - f0, "ok"))
        shot = os.path.join(d, "s99-end.ppm")
        s.dump(shot)
        shots.append(("end f%d" % s.frame, shot))
        res.frames = s.frame
        end_state = (s.map_id, s.probe())
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
    res.sheet = contact_sheet(d, ms.name, shots)
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
