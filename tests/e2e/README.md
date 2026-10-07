# tests/e2e: story and system milestones on the real core

The e2e harness plays Platinum, Diamond and Pearl headless, milestone by milestone, from a new game toward the
Hall of Fame. Each milestone starts from the previous one's end save (or a minted lab save), drives the game with
input bots that read a probe the guest publishes, saves in game, and is judged on the end save. The plan and the
milestone list are in [PLAN.md](PLAN.md); how to write and prove a milestone is in [AUTHORING.md](AUTHORING.md).

```
tests/e2e/run.py --game platinum                                # the story chain, planned milestones skipped
tests/e2e/run.py --game diamond --only 08-roark-coal-badge      # one milestone (from 07's end save, else --lab)
tests/e2e/run.py --game platinum --from 05-jubilife-poketch     # the chain from 05 on
tests/e2e/run.py --game platinum --only 09-... --planned --lab  # prove a planned one from its lab.recipe
tests/e2e/run.py --game pearl --systems                         # side systems (systems.txt)
tests/e2e/run.py --game diamond --check                         # parse and resolve every milestone, run nothing
tests/e2e/lab_check.py --game all                               # every save-lab verb, minted and read back
tests/e2e/tools/probe_map.py --game diamond X.sav --warp MAP_ETERNA_GYM 0   # the probe's tile window as ASCII
tests/e2e/tools/dp_prior.py                                     # D/P: rewrite every lab.recipe's cumulative state
```

Heavy runs go through `tools/heavy.sh --run` (its own pool of run slots). The runner needs the ROMs and a core build
(`build/core-plat`, `build/core-dp`; docs/BUILDING.md) and builds `np_gp` and `np_save4` itself.

## Status

Passing (`status` removed from `milestone.toml`), as a continuity chain from a blank chip:

| game | milestones |
|---|---|
| Platinum | 01-19: new game .. Coal Badge, Jubilife tag battle, Floaroma Meadow, Valley Windworks, Eterna Forest, Forest Badge, Eterna statue/HM01, Galactic building, Togepi/Bicycle/Explorer Kit, Cycling Road to Hearthome, Contest Hall, Relic Badge |
| Diamond | 01-18: new game, Pokedex, Parcel + catching tutorial, Trainers' School, Poketch, Route 203 + Oreburgh Gate, Oreburgh Mine Roark, Coal Badge, Barry's farewell, Jubilife tag battle, Floaroma Meadow, Valley Windworks, Eterna Forest, Forest Badge, Galactic building, Bicycle + Explorer Kit, Cycling Road + VS Seeker, Mt. Coronet + Route 208 to Hearthome |

Everything else is still `status = "planned"` and skipped unless `--planned`.

## How a milestone runs

1. **Start** (`[start]`): `from = "prev"` takes the end save of the chain entry before it, if that milestone passed
   (a failed run's save is renamed `failed.sav` so nothing continues it); else `lab` mints its `lab.recipe`
   (`--lab` forces this); `recipe` mints a side system's start; `blank = true` starts from an erased chip. Minting
   runs the save lab (Platinum: `games/platinum/pc/src/pc_lab.c` on a new game; D/P:
   `games/diamond/pc/game/pc_dp_lab.c` on the new-game base save) with the recipe compiled by
   `tests/gameplay/labc.py`; a recipe `clock` line becomes the run's `PC_RTC`, carried down the chain in `end.clock`.
   A `boost` recipe (party level/moves/held items/bag items only; AUTHORING.md, Boosts) is then applied on top of
   whichever start save was placed.
2. **Boot**: a save boots through the title and CONTINUE until the player is free; a blank chip starts at frame 0.
3. **Steps**: the `[[step]]` bots in order (below). Every frame counts against `[run] frames`; a step's `max` bounds
   that step.
4. **End**: an in-game save (the host's quick-save request), then `np_gp` exits and writes `end.sav`.
5. **Judge**: `[expect]` against the final map/tile and the end save as `features/tools/np_save4 dump` reads it.

The run is deterministic: the same start save and steps replay the same frames, battles and RNG.

Runs use `-o text_instant=1` (message boxes print at once; `DEFAULT_OPTIONS` in run.py). A milestone that plays a
recorded press schedule depends on the real text timing and sets `[run] options = ["text_instant=0"]` (both 01s).

## The probe

A guest started with `PC_E2E=1` publishes one block every frame (`core/include/np_e2e.h`), whose address is the
status slot `NP_STAT_E2E`: the map, the player's tile/height/facing/move state, up to 64 other map objects (tile,
local id, graphics id), a 64x64 tile window around the player (behavior byte, collision bit, loaded bit), and the
battle input the game is waiting on (`ui` 1: the bottom-screen menu, `ui_arg` its config index: 1-10 actions, 11
moves, 12 targets, 13-17 YES/NO prompts; `ui` 2: the battle party screen, 0 slots / 1 the SHIFT page). Without
`PC_E2E` nothing runs and hashes are unchanged (`tests/dp/regress.sh`). Platinum's half is
`games/platinum/pc/src/pc_np_field.c` + `pc_e2e.c` and the battle patches; D/P's is
`games/diamond/pc/game/pc_dp_field.c` and the overlay 9/11 patches.

`tests/gameplay/np_gp.c --serve 1` is the runner's link to the core: one command per line (`run N KEYS [X Y] [until
COND]...`, `e2e`, `dump`, `opt`, `sched`, `quit`); `tests/e2e/np_e2e.py` wraps it (`Session`, `Probe`).

## milestone.toml

```toml
title = "Oreburgh Gym: Roark and the Coal Badge"
status = "planned"          # skipped unless --planned; removed once the milestone passes
priority = "P0"
version = "both"            # D/P only: both | diamond | pearl
estimate = 13400            # measured frames of the passing chained run
refs = ["scr_seq 0046 @0x0010"]
notes = "..."

[start]
from = "prev"               # | recipe = "start.recipe" | blank = true ; lab = "lab.recipe" is the fallback
# boost = "boost.recipe"    # party strength on top of the start (AUTHORING.md, Boosts)
# boot = "continue"         # default with a save; "none" for a blank chip

[run]
frames = 20100              # hard budget, about 1.5x the estimate
# save = "quick"            # end save (default) | "none"
# options = ["text_instant=0"]
# clock = "2009-03-22 21:00:00"
# env = { PC_SOMETHING = "1" }

[[step]]                    # any step: max = N, shot = "name", note = "..."
do = "walk_to"
x = 5
z = 4
face = "up"
interact = true

[expect]
map = "MAP_OREBURGH_GYM"
position = [5, 4]
badges = 1
badge = ["BADGE_COAL"]
flags = ["0x7A"]
flags_clear = ["0x8E"]
party = ["SPECIES_TURTWIG"]
party_size = 1
battles = 3                 # at least this many battle starts
log = ["pc-np: quick save"] # regexes over run.log
save = ['s["trainer"]["money"] >= 500']   # Python over the np_save4 dump s

[expect.vars]
"0x4086" = 3

[[shots]]
frames = [1800, 2600]       # extra dumps at absolute run frames
```

Names resolve per game as the lab recipes do (Platinum `MAP_HEADER_*`, `FLAG_*`, `VAR_*`, `BADGE_ID_*`; D/P
`MAP_*`, `BADGE_*`, hex flags/vars).

## Bots

| `do` | keys | what it does |
|---|---|---|
| `walk_to` | `x`, `z`, opt. `via`, `map`, `face`, `interact`, `run`, `on_battle`, `on_text`, `surf`, `hm` | A* over the probe's tile window (collision, ledges as one-way jumps, water, bike slopes and one-way blocks avoided, tall grass costs 6 so paths go round it), re-planned every step (a failed plan re-reads the probe once: right after a warp it can hold the last map); `surf = true`: surfable water is walkable, a bump from land into it (and up a waterfall) is A + YES; `hm = true`: Cut trees, Rock Smash rocks and Rock Climb walls are on the path (cost 4) and a bump into one is A + YES (Strength boulders are not: push them with steps); the party needs the move; holds the direction through the turn-in-place; bumps teach blocked edges; NPCs are obstacles. Border crossings outdoors are seamless; walking into a door/warp that is the goal takes it; a goal on an exit mat pushes off it. Battles on the way are fought (`auto_battle`) and do not count against `max`; text and cutscenes on the way are advanced with A. `via = [[x, z], ...]`: waypoints walked first, each under its own `max`, for routes wider than the 64x64 window or past what A* cannot see (a bridge deck vs the path under it). |
| `walk_to_door` | `pattern`, `doors`, opt. `wait` + `walk_to`'s | `walk_to` the door a guest log line names: the last match of `pattern` (one group) in run.log keys `doors` (`{"3" = [4, 2]}`). For doors the game rolls at random (Platinum's Hearthome Gym logs `pc-e2e: hearthome gym map M door D` under `PC_E2E`, pc_np_field.c). |
| `talk_to` | `id` | talk to the map object with that local id wherever it is now (wandering people are chased): a free tile next to it, face it, A until a script starts |
| `advance_text` | opt. `through_battle`, `map` | waits up to 40 frames for a script to start, then A every 8 frames until the field has been free for 30 frames; stops at a battle (unless `through_battle`) or as soon as `map` is loaded |
| `auto_battle` | opt. `move` (slot, default 0), `wait` | taps FIGHT and the move every turn; YES/NO prompts: nickname NO, forget a move NO (gives up learning), use the next Pokemon YES, switch NO; on the party screen tries the next slot after a faint; otherwise A, so an evolution plays out |
| `wait_map` | `map` | until that map id |
| `wait_field` | | until the player is free |
| `wait_battle` | | until a battle starts |
| `press` | `keys` (`A`, `UP+B`), opt. `hold`, `gap`, `times` | raw buttons (B answers a field YES/NO as NO) |
| `tap` | `x`, `y`, opt. `hold`, `gap`, `times` | bottom-screen touch |
| `wait_frames` | `n` | idle |
| `schedule` | `file`, opt. `frames` | a recorded `.press` schedule, frames relative to the step |
| `save` | | in-game save now |

## Output

`build/e2e/<game>/` (or `--out DIR`): `report.md` (one row per milestone, then each milestone's steps and contact
sheet), and per milestone `start.sav`, `end.sav` (or `failed.sav`), `end.json` (the np_save4 dump), `run.log` (the
guest's log, status changes, bot notes), `steps.txt`, the PNG shots and the contact sheet `<milestone>.png`. Exit
status 1 if any milestone failed, 2 on a usage or setup error. Look at the sheets: a pass with a wrong picture is a
fail.
