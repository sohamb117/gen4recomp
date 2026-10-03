#!/usr/bin/env python3
"""Boot every map the warp graph reaches, and say what happened on each.

    $ python3 pc/tests/pc_sweep.py            # everything not yet recorded
    $ python3 pc/tests/pc_sweep.py --limit 50
    $ python3 pc/tests/pc_sweep.py --report   # the triage table so far

Where the coordinates come from, because guessing them is why a sweep like
this is usually not worth running. `res/field/events/*.json` holds every warp
in the game as `(x, z) here -> (dest map, dest warp id)`. Inverting that gives,
for each destination map, the warp indices something arrives through, and the
lab's `warp` verb takes an index rather than a tile, so the game's own warp
table resolves it to the spot a player coming through that door lands on. No
tile here is a guess, and a map nothing warps into is simply not in the list.

One boot, many maps. The first version of this minted a save and started a
process per map and cost about a minute each, nearly all of it re-booting the
game to throw the boot away. The boot is the expensive part and none of it is
per-map, so the port now walks the list inside a single run (`--sweep`) at
about two seconds a map. That is the difference between eight hours and twenty
minutes, which is the difference between a sweep that gets run and one that
does not.

The input script is part of the measurement. It presses A for the whole run,
not just through the opening scene. With the short script 41 maps came back as
refusals and a quarter of them were not; the player's own house among them,
because arriving there starts a scene that waits for a button. A sweep whose
refusals are its own input script is worse than no sweep.

RESUMABLE, and it has to be: the one thing a single run cannot survive is a
map that takes the process down. The port flushes each verdict as it writes it,
so the last line names the map before the one that did it; this restarts after
that map, records it as whatever killed the run, and carries on.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EVENTS = os.path.join(ROOT, "res", "field", "events")
BINARY = os.path.join(ROOT, "build", "pc", "pokeplatinum")
SWEEP = os.path.join(ROOT, "build", "pc", "sweep")
RESULTS = os.path.join(SWEEP, "results.json")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pc_lab   # noqa: E402

# How A row was measured, and what A change to that can move.
#
# Twice in one task the measurement method turned out to be wrong, and both
# times the whole 469-map table was re-run. The second time that was waste: the
# change was "stop pressing A once the map is up", and less input cannot turn a
# map that loaded into one that does not, only a crash could become a pass.
# Re-running 432 clean rows to learn nothing cost half an hour.
#
# So each row records the method that produced it, and each method records
# which recorded outcomes it could plausibly move. A row whose outcome is not
# in that set is not re-measured, and `affects=None` means "could move
# anything", which is what an honest bump says when it does not know.
METHODS = {
    1: ("a save and a process per map; A only through the opening scene", None),
    2: ("one boot for the whole list; A pressed for the entire run", None),
    3: ("A held idle once the map is up, so a battle started by the sweep is "
        "not filed against the map underneath it",
        ("trap", "signal", "black", "no-frame")),
    4: ("sweep recipe gives a party, so the Hall of Fame display is not asked "
        "to sprite an empty party",
        ("trap", "signal")),
}
METHOD = max(METHODS)

# Written reason for every map that is not clean. The sweep's exit is that
# nothing is left in "unknown": a row is either clean or it names why.
REASONS = {
    "MAP_HEADER_DYNAMIC":
        "placeholder header, not a map",
    "MAP_HEADER_UNION_ROOM":
        "wireless room; the comm model is still open",
    "MAP_HEADER_UNUSED_JUBILIFE_CITY_HOUSE_3":
        "header preloads archive member 20 of a 16-member NARC; pret named it UNUSED",
    "MAP_HEADER_UNUSED_JUBILIFE_CITY_HOUSE_4":
        "header preloads archive member 20 of a 16-member NARC; pret named it UNUSED",
    "MAP_HEADER_POKEMON_LEAGUE_CHAMPION_ROOM":
        "arrival script starts Cynthia's battle; the field task never goes idle",
    "MAP_HEADER_STARK_MOUNTAIN_ROOM_1":
        "arrival script starts a trainer battle; the field task never goes idle",
    "MAP_HEADER_POKEMON_LEAGUE_HALL_OF_FAME":
        "arrival script is the ending; with a party the display no longer faults, "
        "the scene never idles (credits then OS_ResetSystem)",
    "MAP_HEADER_POKEMON_LEAGUE_HALLWAY_TO_HALL_OF_FAME":
        "arrival script walks you into the Hall of Fame",
    "MAP_HEADER_FIGHT_AREA":
        "first-visit rival scene never idles; FieldMap_ChangeZone remaps the header "
        "to EVERYWHERE",
    "MAP_HEADER_BATTLE_FRONTIER_GATE_TO_FIGHT_AREA":
        "first-visit attendant scene; the questions menu loops on A (entry 0)",
    "MAP_HEADER_CONTEST_HALL_LOBBY":
        "first-visit Mom/Keira scene never finishes in the settle window",
    "MAP_HEADER_LAKE_VERITY_LOW_WATER":
        "first-visit Cyrus scene never finishes in the settle window",
    "MAP_HEADER_SANDGEM_TOWN_POKEMON_RESEARCH_LAB":
        "first-visit Pokedex scene never finishes in the settle window",
    "MAP_HEADER_STARK_MOUNTAIN_ROOM_3":
        "first-visit Heatran scene never finishes in the settle window",
    "MAP_HEADER_RESORT_AREA_RIBBON_SYNDICATE_1F":
        "on-frame CheckEntry warps you back to the Resort Area without 10 ribbons",
    "MAP_HEADER_VISTA_LIGHTHOUSE_ELEVATOR":
        "arrival script is the elevator; it warps onward before the field goes idle",
    "MAP_HEADER_HEARTHOME_CITY_NORTHEAST_HOUSE_ELEVATOR":
        "arrival script is the elevator; it warps onward before the field goes idle",
    "MAP_HEADER_HEARTHOME_CITY_SOUTHEAST_HOUSE_ELEVATOR":
        "arrival script is the elevator; it warps onward before the field goes idle",
    "MAP_HEADER_CANALAVE_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_CELESTIC_TOWN_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_ETERNA_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_FIGHT_AREA_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_FLOAROMA_TOWN_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_HEARTHOME_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_JUBILIFE_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_OREBURGH_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_PASTORIA_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_POKEMON_LEAGUE_NORTH_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_POKEMON_LEAGUE_SOUTH_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_RESORT_AREA_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_SANDGEM_TOWN_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_SNOWPOINT_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_SOLACEON_TOWN_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_SUNYSHORE_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_SURVIVAL_AREA_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
    "MAP_HEADER_VEILSTONE_CITY_POKECENTER_B1F":
        "first-visit Pal Pad scene; the info menu loops on A. Union-room floor.",
}


def affected_since(method):
    """Which outcomes could move for a row measured under `method`."""
    outcomes = set()
    for version in range(method + 1, METHOD + 1):
        _why, affects = METHODS[version]
        if affects is None:
            return None                       # anything could move
        outcomes |= set(affects)
    return outcomes


def is_stale(row):
    was = row.get("method", 0)
    if was >= METHOD:
        return False
    affects = affected_since(was)
    return affects is None or row.get("outcome") in affects


TRAP = re.compile(r"^(pc[_-][\w-]+|armrec\w*|pokeplatinum-pc):.*", re.M)
NOISE = ("rom:", "save:", "ARM7 sound driver", "sweeping", "line(s) on a")


def destinations():
    """{destination map name: sorted warp indices something arrives through}."""
    out = {}
    for path in sorted(glob.glob(os.path.join(EVENTS, "*.json"))):
        with open(path) as f:
            data = json.load(f)
        for warp in data.get("warp_events", []):
            dest = warp.get("dest_header_id")
            if not isinstance(dest, str) or not dest.startswith("MAP_HEADER_"):
                continue
            out.setdefault(dest, set()).add(int(warp.get("dest_warp_id", 0)))
    return {k: sorted(v) for k, v in sorted(out.items())}


def load_results():
    if os.path.exists(RESULTS):
        with open(RESULTS) as f:
            return json.load(f)
    return {}


def save_results(results):
    os.makedirs(SWEEP, exist_ok=True)
    tmp = RESULTS + ".tmp"
    with open(tmp, "w") as f:
        json.dump(results, f, indent=1, sort_keys=True)
    os.replace(tmp, RESULTS)


def run_chunk(todo, ids, tmp):
    """One boot over `todo`. Returns (verdicts, how it ended, what it said)."""
    listfile = os.path.join(tmp, "sweep.list")
    outfile = os.path.join(tmp, "sweep.out")
    with open(listfile, "w") as f:
        for name in todo:
            f.write("%d %d\n" % (ids[name][0], ids[name][1]))
    if os.path.exists(outfile):
        os.unlink(outfile)

    recipe = os.path.join(tmp, "sweep.recipe")
    with open(recipe, "w") as f:
        f.write("# the sweep needs a booted game with a party, not a save\n")
        f.write("name SWEEP\n")
        f.write("party SPECIES_STARAPTOR 50 ITEM_NONE\n")
        f.write("party SPECIES_LUXRAY 50 ITEM_NONE\n")
    lab = pc_lab.compile_recipe(recipe, os.path.join(tmp, "sweep.lab"))

    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    env.update({"PC_LAB": lab, "PC_LAB_AT": "1800", "PC_SAVE": "none",
                "PC_PACE": "0", "PC_SWEEP": listfile, "PC_SWEEP_OUT": outfile,
                "PC_INPUT": os.path.join(ROOT, "pc/replays/sweep-drive.txt")})
    r = subprocess.run([BINARY], env=env, capture_output=True, text=True,
                       timeout=7200)

    verdicts = []
    if os.path.exists(outfile):
        with open(outfile) as f:
            for line in f:
                cols = line.split()
                if len(cols) < 3:
                    continue
                v = {"warp": int(cols[1]), "outcome": cols[2],
                     "method": METHOD}
                if cols[2] != "refused":
                    v["digest"] = cols[3]
                    v["colours"] = int(cols[4])
                for extra in cols[5:]:
                    if "=" in extra:
                        k, _s, val = extra.partition("=")
                        v[k] = val
                verdicts.append((int(cols[0]), v))
    return verdicts, r.returncode, r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--limit", type=int, help="stop after this many maps")
    ap.add_argument("--redo", metavar="OUTCOMES",
                    help="re-measure rows with these outcomes, comma separated "
                         "(`--redo trap,refused` after a fix)")
    ap.add_argument("--all", action="store_true",
                    help="re-measure every row, which is only right when the "
                         "method changed in a way that could move a pass")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--tmp", default=os.path.join(SWEEP, "work"))
    args = ap.parse_args()

    dests = destinations()
    consts = pc_lab.constants()
    results = load_results()

    if args.report:
        by = {}
        for name, r in results.items():
            by.setdefault(r["outcome"], []).append(name)
        stale = sum(1 for r in results.values() if is_stale(r))
        print("warp sweep: %d of %d reachable map header(s) recorded, "
              "%d stale for method %d" % (len(results), len(dests), stale, METHOD))
        unexplained = []
        for outcome in sorted(by):
            print("  %-9s %4d" % (outcome, len(by[outcome])))
            if outcome != "clean":
                for name in sorted(by[outcome]):
                    why = REASONS.get(name) or results[name].get("why", "")
                    print("      %-52s %s" % (name, why[:72]))
                    if name not in REASONS and not results[name].get("why"):
                        unexplained.append(name)
        print("  %d left" % len([n for n in dests if n not in results]))
        if unexplained:
            print("  %d without a written reason:" % len(unexplained))
            for name in unexplained:
                print("      %s" % name)
        return 0

    ids = {n: (consts[n], dests[n][0]) for n in dests if n in consts}
    by_id = {v[0]: n for n, v in ids.items()}
    os.makedirs(args.tmp, exist_ok=True)

    # What actually needs measuring: never recorded, recorded under a method
    # that could have moved this row's outcome, or asked for by hand.
    if args.all:
        todo = list(ids)
        results = {}
    elif args.redo:
        want = {o.strip() for o in args.redo.split(",")}
        todo = [n for n in ids
                if n not in results or results[n].get("outcome") in want]
    else:
        todo = [n for n in ids if n not in results or is_stale(results[n])]

    stale = sum(1 for n in ids if n in results and is_stale(results[n]))
    if args.limit:
        todo = todo[:args.limit]
    print("sweep: %d map(s) to do (%d never measured, %d stale), %d recorded, "
          "%d reachable in all"
          % (len(todo), len([n for n in ids if n not in results]), stale,
             len(results), len(dests)))
    if not todo:
        print("  nothing to do; every row is current for method %d" % METHOD)
        return 0

    while todo:
        verdicts, code, stderr = run_chunk(todo, ids, args.tmp)
        for map_id, v in verdicts:
            name = by_id.get(map_id, str(map_id))
            if name in REASONS and "why" not in v:
                v["why"] = REASONS[name]
            results[name] = v
        done = {by_id.get(m, str(m)) for m, _v in verdicts}
        todo = [n for n in todo if n not in done]

        if not todo:
            break

        # The run ended with maps left, and there are two ways that happens.
        # A map that never settles is the port stopping ITSELF: it writes that
        # map's refusal and exits 0, so the verdict is already recorded and
        # nothing here should be blamed, restarting is the whole response.
        # A non-zero exit is the other way, and there the map that was next is
        # the one that did it and has no verdict of its own.
        if code == 0:
            if not verdicts:
                print("  the run ended clean with nothing recorded; stopping "
                      "rather than looping")
                break
            save_results(results)
            continue

        casualty = todo[0]
        traps = [m.group(0) for m in TRAP.finditer(stderr)
                 if not any(n in m.group(0) for n in NOISE)]
        results[casualty] = {
            "warp": ids[casualty][1],
            "method": METHOD,
            "outcome": "trap" if traps else "signal",
            "exit": code,
            "why": REASONS.get(casualty) or (traps[-1] if traps else
                    (stderr.strip().split("\n") or [""])[-1])[:200],
        }
        print("  %-52s %-8s %s" % (casualty, results[casualty]["outcome"],
                                   results[casualty]["why"][:50]))
        todo = todo[1:]
        save_results(results)

    save_results(results)
    left = len([n for n in ids if n not in results])
    counts = {}
    for r in results.values():
        counts[r["outcome"]] = counts.get(r["outcome"], 0) + 1
    print("%d recorded (%s), %d left"
          % (len(results),
             ", ".join("%s %d" % kv for kv in sorted(counts.items())), left))
    return 0


if __name__ == "__main__":
    sys.exit(main())
