"""Rewrite a Platinum lab.recipe's party block (Garchomp/Salamence) to the chain's party, and add the fly first-arrival
flags before the start line. Usage: pt_labparty.py DIR [--fifth] TOWN... (towns visited beyond the base list; --fifth:
the Infernape 36's boost adds, for the milestones after it)."""
import re, sys
d = sys.argv[1]
fifth = "--fifth" in sys.argv
args = [t for t in sys.argv[2:] if t != "--fifth"]
towns = ["TWINLEAF_TOWN", "SANDGEM_TOWN", "JUBILIFE_CITY", "OREBURGH_CITY", "FLOAROMA_TOWN", "ETERNA_CITY",
         "HEARTHOME_CITY", "SOLACEON_TOWN", "VEILSTONE_CITY", "PASTORIA_CITY", "CELESTIC_TOWN", "CANALAVE_CITY"] + args
p = "tests/e2e/platinum/%s/lab.recipe" % d
s = open(p).read()
party = """# lab party: the chain's party (Torterra lead with four battle moves, the Togepi hatched from 16's egg, Bibarel
# carrying Cut/Rock Smash/Surf, Staraptor carrying Fly: 20's and 29's boosts), so the boosts touch the same slots on
# either start
party SPECIES_TORTERRA 63 ITEM_NONE
party-move 0 0 MOVE_EARTHQUAKE
party-move 0 1 MOVE_CRUNCH
party-move 0 2 MOVE_SEED_BOMB
party-move 0 3 MOVE_ROCK_SLIDE
party SPECIES_TOGEPI 1 ITEM_NONE
party SPECIES_BIBAREL 40 ITEM_NONE
party-move 2 0 MOVE_CUT
party-move 2 1 MOVE_ROCK_SMASH
party-move 2 2 MOVE_SURF
party-move 2 3 MOVE_TAKE_DOWN
party SPECIES_STARAPTOR 45 ITEM_NONE
party-move 3 0 MOVE_FLY
"""
if fifth:
    party += """party SPECIES_INFERNAPE 80 ITEM_NONE
party-move 4 0 MOVE_FLAMETHROWER
party-move 4 1 MOVE_CLOSE_COMBAT
party-move 4 2 MOVE_ICE_PUNCH
party-move 4 3 MOVE_THUNDER_PUNCH
"""
m = re.search(r"# lab party[^\n]*\n(?:#[^\n]*\n)*(?:party[^\n]*\n)+", s)
assert m, "no party block"
s = s[:m.start()] + party + s[m.end():]
if "FLAG_FIRST_ARRIVAL_JUBILIFE_CITY" not in s:
    i = s.rindex("\n# start")
    fl = ("\n# fly destinations: each town's first arrival, set on entering it (src/spawn_locations.c:135; the town map's"
          " fly\n# unlocks, src/applications/town_map/context.c:146)\n" + "".join("flag FLAG_FIRST_ARRIVAL_%s\n" % t for t in towns))
    s = s[:i] + fl + s[i:]
open(p, "w").write(s)
print("ok", d)
