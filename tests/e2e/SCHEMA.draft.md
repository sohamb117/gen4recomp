# milestone.toml (draft v1, E2EHarness)

Layout: `tests/e2e/<game>/chain.txt` lists milestone dirs in order (one per
line, `#` comments). Each `tests/e2e/<game>/<nn>-<name>/` holds
`milestone.toml` plus any files it references (recipes, .press schedules).
`<game>` is platinum | diamond | pearl. Pearl may reuse diamond milestones
with `chain.txt` lines like `../diamond/04-roark` (paths are relative to the
game dir).

Names (MAP_*, FLAG_*, VAR_*, SPECIES_*, ITEM_*, MOVE_*, BADGE_*) resolve per
game exactly as lab recipes do (Platinum: build/pc-wasm generated headers;
D/P: pokediamond include/constants). Numbers are always accepted.

```toml
title = "Oreburgh Gym: Roark and the Coal Badge"

[start]                      # exactly one source; default from = "prev"
from = "prev"                # end save of the previous chain.txt entry
# from = "03-gate"           # end save of a named milestone (same game)
# recipe = "start.recipe"    # lab recipe in this dir, minted fresh (lab verbs)
# blank = true               # blank chip: title screen -> new game by steps
boot = "continue"            # "continue" (default when a save exists: boot +
                             # title + CONTINUE until field_ready=1) | "none"

[run]
frames = 30000               # hard budget for the whole run (boot included)
save = "quick"               # at the end: "quick" (in-game save via the host
                             # quicksave request once field_ready=1; default)
                             # | "none" (keep only what the game itself stored)
options = ["text_instant=1"] # np_core options (-o NAME=VALUE), optional

# Ordered steps; each is one table with `do` = bot name. Common keys on any step:
#   max = N        frame bound for the step (default per bot); exceeding = FAIL
#   shot = "name"  dump a frame when the step ends (into the contact sheet)
#   note = "..."   shown in the report
[[step]]
do = "walk_to"               # A* to tile (x, z) on the current map; walks into
x = 5                        # doors/warps when the target is one; fails if no
z = 3                        # path; re-plans when blocked (NPCs)
# map = "MAP_HEADER_..."     # optional: assert the map first
# face = "up"                # optional: turn to face after arriving
# interact = true            # optional: press A after arriving (talk / sign)

[[step]]
do = "advance_text"          # A with spacing until field_ready=1 (bounded)

[[step]]
do = "auto_battle"           # while in_battle: FIGHT + move slot `move`
# move = 0                   # default 0 (first move); learn-move -> give up,
                             # nickname -> NO, evolution allowed, faint -> next
                             # healthy mon; ends when in_battle=0

[[step]]
do = "wait_map"              # until map_id == map (field_ready not required)
map = "MAP_HEADER_OREBURGH_CITY"

[[step]]
do = "wait_field"            # until field_ready=1

[[step]]
do = "wait_battle"           # until in_battle=1 (e.g. after walking into a trainer's view)

[[step]]
do = "press"                 # keys: A B X Y L R START SELECT UP DOWN LEFT RIGHT,
keys = "A"                   # '+'-joined for chords ("UP+B")
# hold = 4                   # frames held (default 4)
# gap = 12                   # frames released after each press (default 12)
# times = 1

[[step]]
do = "tap"                   # touch screen tap, bottom screen coords
x = 128
y = 96
# hold = 4, gap = 12, times = 1

[[step]]
do = "wait_frames"
n = 120

[[step]]
do = "schedule"              # a shell press file (shell/README.md format),
file = "menu.press"          # frames relative to the step's start

[[step]]
do = "save"                  # in-game save now (quicksave request; needs field)

[expect]                     # all checked at the end; any miss = FAIL
map = "MAP_HEADER_OREBURGH_CITY_GYM"   # final map_id (name or number)
position = [5, 3]            # final player tile, optional
badges = 1                   # badge count in the end save
badge = ["BADGE_ID_COAL"]    # these badges set (platinum BADGE_ID_*, D/P BADGE_*)
flags = ["FLAG_..."]         # set in the end save
flags_clear = ["FLAG_..."]   # clear in the end save
vars = { VAR_SANDGEM_TOWN_STATE = 3 }
party = ["SPECIES_EMPOLEON"] # each species present in the party
party_size = 2
battles = 3                  # at least this many in_battle 0->1 transitions
log = ["stored 524288-byte save"]      # regexes over the run log
save = ['s["trainer"]["money"] >= 5000']  # python exprs over the np_save4 dump `s`

[[shots]]                    # extra fixed-frame shots (absolute run frames)
frames = [1800, 2600]
```

Outputs (per run, `--out DIR`, default build/e2e/<game>): `DIR/<milestone>/`
start.sav, end.sav, run.log (status/guest log), steps.txt (step, frames,
result), shots as PNG, `<milestone>.png` contact sheet; `DIR/report.md`.
