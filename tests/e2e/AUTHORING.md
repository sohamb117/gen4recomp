# Authoring and proving e2e milestones

For phase-2 agents turning a `planned` skeleton into a passing milestone, or writing a new one. The plan and the
milestone list are in [PLAN.md](PLAN.md); the runner, bots and the authoritative schema are the harness's
(`tests/e2e/run.py`, `bots.py`, [README.md](README.md)). Schema changes go through the harness owner;
this guide follows the format every skeleton uses today.

## Layout

```
tests/e2e/<game>/chain.txt              story order, one dir per line (# comments); new game -> credits
tests/e2e/<game>/systems.txt            side systems (standalone)
tests/e2e/<game>/<nn>-<slug>/
    milestone.toml                      what to do and what must be true at the end
    lab.recipe                          story: FULL cumulative state + the start warp (standalone start)
    boost.recipe                        story: party strength on top of the start (Boosts), optional
    start.recipe                        side system: the state it needs + the start warp
    *.recipe / *.press                  extra recipes (e.g. a version twin) or exact menu schedules
```

`<game>` is `platinum`, `diamond` or `pearl`. Diamond dirs hold everything both versions share
(`version = "both"`); `pearl/chain.txt` and `pearl/systems.txt` list them as `../diamond/<dir>` and point at a
Pearl-local dir only where the scripts branch on the version (`version = "pearl"`, its Diamond twin
`version = "diamond"`). Numbers order the chain; a gap or a suffix (`31b-...`) is fine.

## milestone.toml

Start with a 2-6 line comment: what the milestone proves, and `Start: <map/tile> -> end: <map/tile>`
(`tools/plan.py` copies it into PLAN.md). Then, in this order:

```toml
title = "Oreburgh Gym: Roark and the Coal Badge"
status = "planned"          # the runner SKIPs planned milestones; remove the line once proven (Proving, step 6)
priority = "P0"             # P0 story to credits, P1 major system, P2 minor/post-game
version = "both"            # Diamond/Pearl only: both | diamond | pearl
estimate = 21000            # frames at 60 fps, measured once proven
refs = [                    # every decomp fact the milestone relies on (see Citations)
  "scripts_oreburgh_city_gym.s:26-28",
  "TRAINER_LEADER_ROARK (246)",
]
notes = "optional: why the estimate, risks"

[start]                     # exactly one source
from = "prev"               # story: the previous chain entry's end save ...
lab = "lab.recipe"          # ... or this recipe with --lab / when that save is missing
# boost = "boost.recipe"    # party strength applied on top of whichever start (Boosts)
# recipe = "start.recipe"   # side systems: mint fresh
# blank = true              # 01 only: blank chip, title screen -> new game by steps
# Post-game side systems chain from a real save: from = "59b-hall-of-fame-credits" (or another system's dir)
# + recipe = "chain.recipe" (only what no chained milestone plays, and the station's map line, minted on that
# end save) + lab = "start.recipe" (the standalone start, used with --lab or when the from save is missing).
# Run them with --out pointing at the chain's output (build/e2e/d22, p07) so the from save is found.

[run]
frames = 31500              # hard budget = estimate * 1.5, rounded up to 100
# clock = "2009-03-22 21:00:00"   # PC_RTC when time of day / day matters (honey, roamers, daily events)
# env = ["NAME=VALUE"]            # extra guest environment

[[step]]                    # ordered; any step may carry max = N (frame bound), shot = "name", note = "..."
do = "walk_to"
x = 5
z = 4
face = "up"
interact = true

[expect]                    # all checked at the end; any miss fails the milestone
map = "MAP_HEADER_OREBURGH_CITY"
badges = 1
badge = ["BADGE_ID_COAL"]
flags = ["FLAG_RECEIVED_ROARK_TM76"]
flags_clear = ["FLAG_HIDE_JUBILIFE_ROWAN"]
battles = 1
save = ['any(i["item"] == 403 for k, p in s["bag"].items() if k != "registered" for i in p)']

[expect.vars]
VAR_OREBURGH_CITY_STATE = 2
```

### Steps (bots)

| `do` | keys | what it does |
|---|---|---|
| `walk_to` | `x`, `z`, opt. `via`, `map`, `face`, `interact`, `surf`, `hm` | path to the tile on the current map, re-planning around NPCs; walking into a warp tile takes it; trainers who spot you on the way are fought; `surf`/`hm` let the path cross water / Cut trees, Rock Smash rocks, Rock Climb walls (the party must know the move: boost it); `via = [[x, z], ...]` waypoints first (long routes, bridges); `avoid = [[x, z], ...]` lists tiles never planned through |
| `slide` | `dirs` | ice: presses (space separated), each followed by a wait until the slide stops; battles handled |
| `talk_to` | `id` | talk to the map object with that local id wherever it stands (wandering people are chased) |
| `advance_text` | opt. `map` | press A with spacing until the field is free (cutscenes, dialogue, item jingles); with `map`, stop as soon as that map loads |
| `auto_battle` | opt. `move` (slot) | FIGHT every turn with the best usable move by type (README: base power x STAB x effectiveness from the ROM), switching out a lead with no damaging move left; `move = N` forces slot N for scripted fights; declines move learning and nicknames, allows evolution |
| `fly` | `map`, opt. `slot`, `block` | Platinum: Fly to a visited town (`map` its header) through the start menu, the party member that knows Fly and the town map; boost a Fly carrier into the party; use it wherever the story moves the player across the region |
| `steps` | `route` (`[[x, z], ...]` corners), opt. `face`, `interact` | a fixed tile route for maps whose collision the probe cannot show (moving-platform gyms: `tools/pt_gym.py canalave`) |
| `moves` | `dirs` (`[[dir, count], ...]`, dir "U", "D", "L" or "R"), opt. `face`, `interact` | a direction route, each move held until the probe's (map, x, y, z) changes: maps whose walkable surface the land grid does not show (Platinum's Distortion World); `tools/pt_explore.py` finds one by exploring with the game as the oracle |
| `hatch` | `x`, `z` | hatch a carried egg by pacing (x, z)/(x+1, z); no-op without one |
| `wait_map` | `map` | until the map id matches |
| `wait_field` | | until the player is free in the field |
| `wait_battle` | | until a battle starts (after walking into a trainer's sight) |
| `wait_reset` | opt. `max` | the last step of a run that ends the game: until the game's own `OS_ResetSystem` after the credits (the port reboots the guest: the `resets` status goes up); needs `[run] save = "none"`; `[expect]` reads the save the game wrote |
| `press` | `keys` (`A`, `UP+B`, ...), opt. `hold`, `gap`, `times` | raw buttons, for menus |
| `tap` | `x`, `y`, opt. `hold`, `gap`, `times` | bottom-screen touch (Pokétch, touch menus) |
| `wait_frames` | `n` | idle |
| `schedule` | `file` | play a recorded `.press` schedule (frames relative to the step) |
| `save` | | in-game save now |

Rules that keep runs deterministic:
- Prefer `walk_to` + `advance_text` over `press`; use `press`/`tap` only for menus whose layout you can state.
- Put `shot = "..."` on every proof point: badge screen, key cutscene, legendary/boss battle start, Hall of Fame.
- Never fake an input you cannot state. Write the steps you can, then `# PHASE2: <what is unknown, how to find it>`
  and stop; `[expect]` stays complete.
- Coordinates come from the map's events (Platinum `res/field/events/events_<map>.json`; D/P
  `dp_script.py events MAP_X`). Mark unproven tiles `note = "[INFERENCE] ..."`.

### Expect keys

`map`, `position = [x, z]`, `badges` (count), `badge` (names set), `flags` / `flags_clear`, `[expect.vars]`, `party`
(species present), `party_size`, `battles` (at least this many battle starts), `log` (regexes over the run log),
`save` (Python expressions over the end save's `np_save4` dump `s`: `s["trainer"]`, `s["party"]`, `s["bag"]`,
`s["pokedex"]`, `s["location"]`, Platinum's `s["hall_of_fame"]`: `total` and the `latest` entry's `date`, `party`).
Cover everything the milestone's scripts set that a later milestone depends on.

## Recipes

One lab verb per line, `#` comments. Platinum verbs: `games/platinum/pc/src/pc_lab.c` (header comment and verb
table); Diamond/Pearl: `games/diamond/pc/game/pc_dp_lab.c` (header comment, `sVerbs`). `tests/gameplay/labc.py
[--game diamond|pearl] RECIPE` compiles names to the inline form; run it on every recipe you touch. Names resolve
from each game's own headers: Platinum `MAP_HEADER_*`, `FLAG_*`, `VAR_*`, `BADGE_ID_*`; Diamond/Pearl only
`games/diamond/include/constants/*.h` (+ `include/poketch.h`, `FACE_*`), so D/P flags and vars are hex
(`flag 0x964`, `var 0x4086 3`; in TOML `flags = ["0x964"]`, `"0x4086" = 3`), badges `BADGE_COAL`..,
maps in D/P spelling (`MAP_JUBLIFE`).

Every state line carries a comment citing the script that sets it in play:

```
flag FLAG_RECEIVED_HM06  # scripts_oreburgh_gate_1f.s:44
var 0x4086 3             # scr_seq 0392 @0x01A2
```

### Story recipes (`lab.recipe`)

Standalone: a standard header (`name GQ`, gender, `trainer-id 31415`, money), a lab party, the cumulative state, the
start. The state is grouped by the milestone that set it, in chain order:

```
# -- 07 Oreburgh Mine, Roark --
flag FLAG_ROARK_RETURNED_TO_OREBURGH_GYM  # scripts_oreburgh_mine_b2f.s:56
# -- start --
warp MAP_HEADER_OREBURGH_CITY_GYM 0       # events_oreburgh_city_gym warp 0
```

Milestone N's recipe = header + party + the groups of 01..N-1 + start. When you change what milestone K sets, edit
K's group in every later `lab.recipe` (they are identical copies; `grep -l '^# -- K '`). Include the hide flags of
NPCs a script removes with `RemoveObject` (the object's `hidden_flag`): a missing one puts a story NPC back on the
map. Platinum recipes start from nothing (the lab runs the new-game init); D/P recipes are applied on top of the
new-game base save in the bedroom (`tests/gameplay/mint.sh`). Caps: `LAB_MAX_OPS 512` in both labs.

D/P recipes are not edited by hand between the `# -- prior: tests/e2e/tools/dp_prior.py --` and `# -- start --`
lines: `tests/e2e/tools/dp_prior.py` writes every milestone's groups from the earlier milestones' `[expect]` (flags,
cleared flags, vars, badges, the Pokedex, Poketch apps, bag items; an item a later milestone checks gone leaves the
recipes after it), each line citing the SetFlag/ClearFlag/SetVar site (`dp_script.py grep-flag/grep-var`, preferring
one in the milestone's refs). So what a D/P milestone sets is its `[expect]`: change it, rerun the tool (`--check`
fails on a stale recipe). The Journal flag 0x963 is never minted (no lab verb writes the journal data; with the flag
alone CONTINUE stops on a blank Journal page). The fly map's towns are the arrival flags 0x9B0 + id the game sets in
code on entering a town (sub_02034F88, the table UNK_020F2224 in arm9/asm/unk_02034E84.s: Twinleaf 0x9B0 .. Veilstone
0x9BD, Sunyshore 0x9BE, Snowpoint 0x9BF): list each in the `[expect]` flags of the milestone that first enters the
town, so lab starts can Fly there.

### The lab party

`auto_battle` picks each turn's move by type and PP (README, `auto_battle`), so give the lead a few damaging moves
of different types chosen from the opponents' real teams (Platinum `res/trainers/`, D/P `dp_script.py trainer
TRAINER_X`) and cite them in a step `note`; no `move = N` is needed unless a fight is scripted. Field-move carriers
sit behind the lead, so field moves do not take battle slots.

### Boosts (`boost.recipe`)

`auto_battle` knows types and PP but not levels or stats, so a chained party that played the story honestly is
often too weak for the next boss, and grinding it costs tens of thousands of frames. A milestone fixes that with
`[start] boost =
"boost.recipe"`: after the start save is placed (the previous end save, or the minted lab start), the runner applies
the boost recipe on top of it with the save lab (the derived-save mint `recipe` uses) and the run continues from
there; the report says `... + boost boost.recipe`. The boosted party carries down the chain.

A boost is party strength and items only: `--check` rejects any verb but `party`, `party-move`, `party-level`
(slot, level: experience set to that level's base, stats recomputed, HP full; no evolution, no moves learned),
`party-item` (slot, held item), `party-iv`, `party-ev`, `item` and `register-item` (the Y button item, e.g. the
Bicycle), and any `clock` line. Story state (flags, vars,
badges, the map) is never boosted, so the chain still proves the story was played. Use a boost in place of a
`grind`; keep `grind` only where a script itself needs a battle won or a level reached. Cite the opponents' teams in
the recipe's comments, as for the lab party.

## Citations

| game | form |
|---|---|
| Platinum | `scripts_<map>.s:LINE`, `events_<map> (...)`, `src/...c:LINE`, `TRAINER_X (id)`, other paths from `games/platinum/` |
| Diamond/Pearl | `scr_seq NNNN @0xOFF`, `zone_event NNNN object|warp|coord|bg k`, `maps.h:LINE`, `map_header.c:LINE`, `trdata.json #N`, `msg NNNN #i`, other paths from `games/diamond/` |

D/P field data is binary; `tests/e2e/tools/dp_script.py` decodes it (run from the repo root):

```
python3 tests/e2e/tools/dp_script.py map MAP_OREBURGH_GYM       # script/level-script/msg/event ids
python3 tests/e2e/tools/dp_script.py script MAP_OREBURGH_GYM    # disassembly with offsets, flags/vars, text inlined
python3 tests/e2e/tools/dp_script.py events MAP_OREBURGH_GYM    # objects (x/z, sight, hidden flag), warps, coords
python3 tests/e2e/tools/dp_script.py trainer TRAINER_LEADER_ROARK_ROARK --where
python3 tests/e2e/tools/dp_script.py grep-flag 0x198            # who sets/clears/checks it
python3 tests/e2e/tools/dp_script.py grep-var 0x4086
python3 tests/e2e/tools/dp_script.py text-grep "Coal Badge"
python3 tests/e2e/tools/dp_script.py version-diff               # Diamond vs Pearl differences
```

Facts the scripts do not prove are `[INFERENCE]`.

## Proving a milestone (phase 2)

1. Build the core (docs/BUILDING.md; heavy builds through `tools/heavy.sh`, `-j` 6 at most).
2. Static check: `python3 tests/e2e/run.py --game <game> --check`.
3. Run it standalone, then chained (`--planned` runs milestones still marked planned):
   `python3 tests/e2e/run.py --game <game> --only <dir> --planned --lab` and
   `python3 tests/e2e/run.py --game <game> --from <dir> --planned` (side systems: `--systems --only <dir>`).
4. Look at every shot and the contact sheet in `build/e2e/<game>/<dir>/` (dumps are PPM:
   `sips -s format png a.ppm --out a.png`). A pass with a wrong picture is a fail.
5. Fix steps/recipe until it passes from both starts; keep fixes cited.
6. Remove `status = "planned"`, set `estimate` to the measured frames and `[run] frames` to 1.5x, then
   `python3 tests/e2e/tools/plan.py` to refresh PLAN.md (`--check` must pass before you commit).
7. Delete scratch dumps once inspected. Never commit ROMs, saves or ROM-derived data (dumps, minted saves).
