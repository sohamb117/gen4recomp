#!/usr/bin/env python3
"""Check that every save-lab verb an e2e milestone needs does what it says.

    tests/e2e/lab_check.py [--game platinum|diamond|pearl|all]

Per game, mints lab_check/<game>.recipe (one line or more of every verb,
the recipe language shared by games/platinum/pc/src/pc_lab.c and
games/diamond/pc/game/pc_dp_lab.c) and lab_check/<game>_warp.recipe (the
`warp` verb, alone because the last map/warp line is the one that loads)
with tests/gameplay/mint.sh, the way milestones mint theirs: Platinum's lab
boots a new game; Diamond/Pearl's continues the game's new-game save, made
here with tests/dp/<game>_first_save.sched as tests/gameplay/run.sh makes
it. Then `np_save4 dump` reads each save back and every recipe line is
asserted against it: the expectations are the compiled recipe's own numbers
(tests/gameplay/labc.py), so the recipe is the single statement of intent.

One PASS/FAIL line per verb; exit status 1 if anything failed, 2 if the
check could not run (no ROM, no core build, a mint that died).

Needs build/core-plat and/or build/core-dp (build/b.sh plat dp) and the
ROMs; builds np_gp and np_save4 itself. Everything generated goes under
build/e2e/lab_check/.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GAMEPLAY = os.path.join(ROOT, "tests", "gameplay")
OUT = os.path.join(ROOT, "build", "e2e", "lab_check")
RECIPES = os.path.join(HERE, "lab_check")
SAVE4 = os.path.join(ROOT, "build", "features", "np_save4")

sys.path.insert(0, GAMEPLAY)
import labc  # noqa: E402

GAMES = {
    "platinum": {"rom": "games/platinum/build/rom/pokeplatinum.us.nds", "core": "build/core-plat",
                 "recipe": "platinum"},
    "diamond": {"rom": "games/diamond/build/diamond.us/pokediamond.us.nds", "core": "build/core-dp",
                "recipe": "dp"},
    "pearl": {"rom": "games/diamond/build/pearl.us/pokepearl.us.nds", "core": "build/core-dp",
              "recipe": "dp"},
}

# Base stats (HP ATK DEF SPEED SPATK SPDEF, the save's order) of the species
# whose stats the recipes recompute; the same in all three games.
BASE_STATS = {
    393: (53, 51, 53, 40, 61, 56),  # Piplup
    399: (59, 45, 40, 31, 35, 40),  # Bidoof
}


class Blocked(Exception):
    pass


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode != 0:
        raise Blocked("%s failed (%d):\n%s%s" % (" ".join(cmd[:3]), p.returncode, p.stdout[-2000:], p.stderr[-2000:]))
    return p.stdout


def build_tools(core):
    """np_gp for this core build (as run.sh links it) and np_save4."""
    gp = os.path.join(OUT, "np_gp-" + os.path.basename(core))
    libs = sorted(os.path.join(core, n) for n in os.listdir(core) if re.match(r"libnp_guest_.*\.a$", n))
    os.makedirs(OUT, exist_ok=True)
    run(["cc", "-O2", "-std=c11", "-Wall", "-I" + os.path.join(ROOT, "core", "include"),
         "-I" + os.path.join(ROOT, "core", "runtime"), os.path.join(GAMEPLAY, "np_gp.c"),
         os.path.join(core, "np_headless_np_registry.c")] + libs + [os.path.join(core, "libnp_runtime.a"),
                                                                    "-lm", "-lpthread", "-o", gp])
    fb = os.path.join(ROOT, "build", "features")
    if not os.path.exists(os.path.join(fb, "build.ninja")):
        run(["cmake", "-S", os.path.join(ROOT, "features"), "-B", fb, "-G", "Ninja"])
    run(["cmake", "--build", fb, "--target", "np_save4"])
    return gp


def base_save(game, rom, gp, out):
    """Diamond/Pearl's new-game save, made the way run.sh makes it."""
    base = os.path.join(out, "base.sav")
    if os.path.exists(base):
        return base
    sched = os.path.join(ROOT, "tests", "dp", "%s_first_save.sched" % game)
    with open(sched) as f:
        m = re.search(r"^# frames: *(\d+)", f.read(), re.M)
    if not m:
        raise Blocked("%s has no '# frames:' line" % sched)
    log = os.path.join(out, "base.log")
    with open(log, "w") as err, open(os.path.join(out, "base.out"), "w") as so:
        subprocess.run([gp, rom, "--game", game, "--frames", m.group(1), "--save", base + ".tmp",
                        "--schedule", sched], stdout=so, stderr=err)
    with open(log) as f:
        if "stored 524288-byte save" not in f.read():
            raise Blocked("the %s new-game save failed (%s)" % (game, log))
    os.replace(base + ".tmp", base)
    return base


def mint(game, rom, gp, recipe, sav, base):
    env = dict(os.environ, NP_GAME=game, NP_ROM=rom, NP_GP=gp)
    env.pop("PC_RTC", None)
    if base:
        env["NP_BASE_SAVE"] = base
    p = subprocess.run(["sh", os.path.join(GAMEPLAY, "mint.sh"), recipe, sav], env=env,
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise Blocked("minting %s failed:\n%s" % (recipe, (p.stdout + p.stderr)[-3000:]))
    return json.loads(run([SAVE4, "dump", rom, sav]))


def ops_of(recipe, game):
    """The recipe's lines as (verb, args) with every name resolved, and the
    environment it asks for (labc.compile_recipe)."""
    inline, env = labc.compile_recipe(recipe, game)
    ops = []
    for line in inline[len("inline:"):].split(";"):
        if not line:
            continue
        verb, *args = line.split()
        ops.append((verb, args if verb in labc.TEXT_VERBS else [int(a) for a in args]))
    return ops, env


def pocket_of(item):
    """Gen 4 item id -> the bag pocket np_save4 names (item data pocket
    field, by id range: these ranges are contiguous in all three games)."""
    if 1 <= item <= 16:
        return "balls"
    if 17 <= item <= 54:
        return "medicine"
    if 149 <= item <= 212:
        return "berries"
    if 328 <= item <= 427:
        return "tms_hms"
    if 428 <= item <= 467:
        return "key_items"
    return "items"


def expected_stats(mon):
    """The game's CalcStats on the dumped IVs/EVs/level/nature."""
    base = BASE_STATS.get(mon["species"])
    if base is None:
        return None
    lvl = mon["level"]
    nature = int(mon["pid"], 16) % 25
    up, down = nature // 5 + 1, nature % 5 + 1  # 1..5 = ATK DEF SPEED SPATK SPDEF
    out = []
    for i, b in enumerate(base):
        v = (2 * b + mon["ivs"][i] + mon["evs"][i] // 4) * lvl // 100
        if i == 0:
            out.append(v + lvl + 10)
            continue
        v += 5
        if up != down and i == up:
            v = v * 110 // 100
        elif up != down and i == down:
            v = v * 90 // 100
        out.append(v)
    return out


def check(ops, env, s):
    """{verb: [failure, ...]} for every verb in ops (empty list = PASS)."""
    res = {}

    def need(verb, ok, why):
        res.setdefault(verb, [])
        if not ok:
            res[verb].append(why)

    t, party, bag, dex = s["trainer"], s["party"], s["bag"], s["pokedex"] or {}
    flags, vars_ = set(s["flags"]), s["vars"]
    badge_mask, items, flag_state, var_state, party_n = 0, {}, {}, {}, 0
    statted, party_want = set(), {}
    for verb, a in ops:
        if verb == "name":
            need(verb, t["name"] == a[0], "name %r, want %r" % (t["name"], a[0]))
        elif verb == "gender":
            want = "female" if a[0] else "male"
            need(verb, t["gender"] == want, "gender %s, want %s" % (t["gender"], want))
        elif verb == "trainer-id":
            want = (a[0] & 0xFFFF, a[0] >> 16)
            need(verb, (t["tid"], t["sid"]) == want, "tid/sid %d/%d, want %d/%d" % ((t["tid"], t["sid"]) + want))
        elif verb == "money":
            need(verb, t["money"] == a[0], "money %d, want %d" % (t["money"], a[0]))
        elif verb == "badge":
            badge_mask |= 1 << a[0]
            need(verb, t["badge_mask"] >> a[0] & 1, "badge %d not set (mask %d)" % (a[0], t["badge_mask"]))
        elif verb == "party":
            party_want[party_n] = list(a)  # species, level, item; party-level / party-item lines amend it
            party_n += 1
        elif verb in ("party-level", "party-item"):
            if a[0] not in party_want:
                need(verb, False, "slot %d has no party line before it" % a[0])
                continue
            party_want[a[0]][1 if verb == "party-level" else 2] = a[1]
            m = party[a[0]] if a[0] < len(party) else {}
            got = m.get("level") if verb == "party-level" else m.get("held_item", {}).get("id")
            need(verb, got == a[1], "slot %d %s %s, want %d" % (a[0], verb[6:], got, a[1]))
            if verb == "party-level":
                statted.add(a[0])
        elif verb == "party-move":
            ids = [mv["id"] for mv in party[a[0]]["moves"]] if a[0] < len(party) else []
            need(verb, a[2] in ids, "slot %d moves %s lack %d" % (a[0], ids, a[2]))
        elif verb in ("party-iv", "party-ev"):
            key = "ivs" if verb == "party-iv" else "evs"
            got = party[a[0]][key][a[1]] if a[0] < len(party) else None
            need(verb, got == a[2], "slot %d %s[%d] = %s, want %d" % (a[0], key, a[1], got, a[2]))
            statted.add(a[0])
        elif verb in ("dex-seen", "dex-caught"):
            lst = dex.get("seen_list" if verb == "dex-seen" else "caught_list", [])
            need(verb, a[0] in lst, "species %d not %s" % (a[0], verb[4:]))
            if verb == "dex-caught":
                need(verb, a[0] in dex.get("seen_list", []), "caught species %d not also seen" % a[0])
        elif verb == "item":
            items[a[0]] = items.get(a[0], 0) + a[1]
        elif verb == "register-item":
            need(verb, bag.get("registered") == a[0], "registered %s, want %d" % (bag.get("registered"), a[0]))
        elif verb in ("flag", "clear-flag"):
            flag_state[a[0]] = verb == "flag"
        elif verb == "var":
            var_state[a[0]] = a[1]
        elif verb == "national-dex":
            if a[0]:
                need(verb, t["national_dex"], "trainer national_dex false")
                need(verb, dex.get("national"), "pokedex national false")
        elif verb == "pokedex":
            if a[0]:
                need(verb, dex.get("obtained"), "pokedex obtained false")
        elif verb == "poketch":
            p = s["poketch"]
            need(verb, p["given"], "poketch not given")
            need(verb, a[0] in p["apps"], "app %d not in %s" % (a[0], p["apps"]))
        elif verb == "map":
            loc = s["location"]
            got = (loc["map"], loc["x"], loc["z"], loc["dir"])
            need(verb, got == tuple(a), "location %s, want %s" % (got, tuple(a)))
        elif verb == "warp":
            need(verb, s["location"]["map"] == a[0], "map %d, want %d" % (s["location"]["map"], a[0]))
        else:
            need(verb, False, "lab_check has no assertion for this verb")
    for k, want in party_want.items():
        if k >= len(party):
            need("party", False, "no party slot %d" % k)
            continue
        m = party[k]
        got = (m["species"], m.get("level"), m["held_item"]["id"])
        need("party", got == tuple(want), "slot %d species/level/item %s, want %s" % (k, got, tuple(want)))
    if badge_mask:
        need("badge", t["badge_mask"] == badge_mask, "mask %d, want exactly %d" % (t["badge_mask"], badge_mask))
    for item, qty in items.items():
        want = pocket_of(item)
        where = [(pk, e["qty"]) for pk, lst in bag.items() if isinstance(lst, list) for e in lst if e["item"] == item]
        need("item", where == [(want, qty)], "item %d in %s, want [(%r, %d)]" % (item, where, want, qty))
    for flag, on in flag_state.items():
        verb = "flag" if on else "clear-flag"
        need(verb, (flag in flags) == on, "flag %d is %s" % (flag, "clear" if on else "set"))
    for var, value in var_state.items():
        got = vars_.get(str(var), 0)
        need("var", got == value, "var 0x%X = %d, want %d" % (var, got, value))
    for slot in sorted(statted):
        m = party[slot]
        want = expected_stats(m)
        need("stats", want is not None, "no base stats for species %d in lab_check.BASE_STATS" % m["species"])
        if want:
            need("stats", m["stats"] == want, "slot %d stats %s, want %s" % (slot, m["stats"], want))
            need("stats", m["hp"] == m["stats"][0], "slot %d hp %d of %d" % (slot, m["hp"], m["stats"][0]))
    if "PC_RTC" in env:
        # The field writes the RTC into the save's game time every minute,
        # so the saved clock is the recipe's plus the minute or two the run
        # took before the save.
        date, clock = env["PC_RTC"].split()
        gt = s["game_time"]
        hms = lambda x: sum(int(p) * m for p, m in zip(x.split(":"), (3600, 60, 1)))  # noqa: E731
        late = hms(gt["time"]) - hms(clock)
        need("clock", gt["date"] == date and 0 <= late <= 600,
             "saved game time %s %s, want %s within 10 minutes after %s" % (gt["date"], gt["time"], date, clock))
    return res


def check_game(game):
    g = GAMES[game]
    rom = os.path.join(ROOT, g["rom"])
    core = os.path.join(ROOT, g["core"])
    if not os.path.exists(rom):
        raise Blocked("no ROM at %s" % rom)
    if not os.path.exists(os.path.join(core, "libnp_guest_%s.a" % game)):
        raise Blocked("no %s core build in %s (build/b.sh %s)" % (game, core, "plat" if game == "platinum" else "dp"))
    out = os.path.join(OUT, game)
    os.makedirs(out, exist_ok=True)
    gp = build_tools(core)
    base = None if game == "platinum" else base_save(game, rom, gp, out)
    results = {}
    for stem in (g["recipe"], g["recipe"] + "_warp"):
        recipe = os.path.join(RECIPES, stem + ".recipe")
        ops, env = ops_of(recipe, game)
        s = mint(game, rom, gp, recipe, os.path.join(out, stem + ".sav"), base)
        for verb, fails in check(ops, env, s).items():
            results.setdefault(verb, []).extend(fails)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", default="all", choices=["platinum", "diamond", "pearl", "all"])
    game = ap.parse_args().game
    games = list(GAMES) if game == "all" else [game]
    status = 0
    for game in games:
        try:
            results = check_game(game)
        except Blocked as e:
            print("FAIL %-8s %-14s %s" % (game, "(mint)", e))
            status = 2
            continue
        for verb, fails in results.items():
            print("%s %-8s %-14s %s" % ("FAIL" if fails else "PASS", game, verb, "; ".join(fails)))
            if fails:
                status = max(status, 1)
    sys.exit(status)


if __name__ == "__main__":
    main()
