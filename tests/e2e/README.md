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
| Platinum | 01-43: new game .. Relic Badge, Route 209 .. Cobble/Fen/Mine/Icicle badges, the lakes, Galactic HQ, Mt. Coronet and Spear Pillar into the Distortion World, B1F..B7F and Cyrus |
| Platinum systems | 60-72, 74: every HM field use (Cut, Rock Smash, Strength, Surf, Fly, Defog, Rock Climb, Waterfall), the three rods, a honey tree, the Day Care (deposit with the lady, egg from the man), level-up evolution after a wild battle |
| Diamond | 01-20: new game, Pokedex, Parcel + catching tutorial, Trainers' School, Poketch, Route 203 + Oreburgh Gate, Oreburgh Mine Roark, Coal Badge, Barry's farewell, Jubilife tag battle, Floaroma Meadow, Valley Windworks, Eterna Forest, Forest Badge, Galactic building, Bicycle + Explorer Kit, Cycling Road + VS Seeker, Mt. Coronet + Route 208 to Hearthome, Contest Hall + rival, Route 209 to Solaceon |

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
4. **End**: an in-game save (the host's quick-save request), then `np_gp` exits and writes `end.sav`. A milestone
   that plays the game to its end (`[run] save = "none"`, last step `wait_reset`) ends in the game's own
   `OS_ResetSystem` after the credits: the port traps it, and `end.sav` is the save the game wrote before.
5. **Judge**: `[expect]` against the final map/tile and the end save as `features/tools/np_save4 dump` reads it.

The run is deterministic: the same start save and steps replay the same frames, battles and RNG.

Runs use `-o text_instant=1` (message boxes print at once; `DEFAULT_OPTIONS` in run.py). A milestone that plays a
recorded press schedule depends on the real text timing and sets `[run] options = ["text_instant=0"]` (both 01s).

## The probe

A guest started with `PC_E2E=1` publishes one block every frame (`core/include/np_e2e.h`), whose address is the
status slot `NP_STAT_E2E`: the map, the player's tile/height/facing/move state, up to 64 other map objects (tile,
local id, graphics id), a 64x64 tile window around the player (behavior byte, collision bit, loaded bit), and the
battle input the game is waiting on (`ui` 1: the bottom-screen menu, `ui_arg` its config index: 1-10 actions, 11
moves, 12 targets, 13-17 YES/NO prompts; `ui` 2: the battle party screen, 0 slots / 1 the SHIFT page). While the
battle menu waits, the block also carries a battle report: every battler (species, level, HP, the battle's types,
moves, PP, the Disabled move) and the menu battler's party in its party screen's order. Without `PC_E2E` nothing
runs and hashes are unchanged (`tests/dp/regress.sh`). Platinum's half is `games/platinum/pc/src/pc_np_field.c` +
`pc_e2e.c` and the battle patches; D/P's is `games/diamond/pc/game/pc_dp_field.c` and the overlay 9/11 patches.
Version 3 adds step layers (D/P only; Platinum leaves `steps_seq` 0): whenever the window is refilled, the guest
floods out from the player over the game's own movement check (`sub_0204A7C8`, Platinum's
`TerrainCollisionManager_WillPlayerCollide`: plate height, dynamic map features, collision bit), carrying the height
a step lands at, so each tile lists up to two places to stand (a bridge deck and the path under it) with the steps
allowed from each. `walk_to` plans over (tile, height) states where layers exist and over the grid elsewhere;
`tools/probe_map.py` prints the layers, `tools/dp_warps.py` routes through doors and warp panels with them.

`auto_battle` with `snap = true` puts its first action menu on the contact sheet (bots.snap: any bot may dump `frame_NNNNNN.ppm`). `np_save4 dump` has a Platinum `daycare` field (both parents with their steps, `egg_waiting`, `step_counter`) a Platinum `roamers` field, `poffins` (the Poffin Case), `trophy_garden` (Backlot's daily slots), `underground` (the sphere, trap, goods, treasure and goods-PC bags, `mined_plates`, `has_mined`), `game_records` (the decoded GameRecords, a list indexed by record id in `generated/game_records.txt` order: `s["game_records"][48]` is RECORD_WATCHED_TV) and, per party mon, `super_contest_ribbons`; and a Platinum `roamers` field (the player's current and previous map, each used roamer slot's species, level, HP, map and `active`). `tools/pt_poke.py` plays a key script on a save with shots (menu layouts); `tools/pt_ugspots.py` lists the Underground's mining spots (wall sparkles) for a save, nearest first; `tools/ms_pass.py` marks proven milestones.

`auto_battle` scores moves with the ROM's own tables, `np_save4 gamedata ROM` (`features/ndsdata` nd_gamedata:
species types and abilities, each move's class/power/type/accuracy/range, the type chart read out of the battle
overlay).

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
| `walk_to` | `x`, `z`, opt. `via`, `map`, `face`, `interact`, `run`, `on_battle`, `on_text`, `surf`, `hm` | A* over the probe's tile window (collision, ledges as one-way jumps, water, bike slopes and one-way blocks avoided, tall grass costs 6 so paths go round it), re-planned every step (a failed plan re-reads the probe once: right after a warp it can hold the last map); `surf = true`: surfable water is walkable, a bump from land into it (and up a waterfall) is A + YES; `hm = true`: Cut trees, Rock Smash rocks and Rock Climb walls are on the path (cost 4) and a bump into one is A + YES (Strength boulders are not: push them with steps); the party needs the move; holds the direction through the turn-in-place; bumps teach blocked edges; NPCs are obstacles. Border crossings outdoors are seamless; walking into a door/warp that is the goal takes it; a goal on an exit mat pushes off it. Battles on the way are fought (`auto_battle`) and do not count against `max`; text and cutscenes on the way are advanced with B (a YES/NO answered NO: the nickname an egg hatching on the way asks for). `on_text = "stop"`: a script that holds the player on the way (a coordinate trigger's scene, whose menus B would cancel) ends the walk there, for the next steps to play. `via = [[x, z], ...]`: waypoints walked first, each under its own `max`, for routes wider than the 64x64 window or past what A* cannot see (a bridge deck vs the path under it). |
| `slide` | `dirs`, opt. `max`, `on_battle`, `on_text` | ice: each direction (space separated) is a press, then a wait until the player has stopped (tile unchanged over six probes, free); battles and text on the way are handled as walk_to does (D/P Snowpoint Gym, 44) |
| `walk_to_door` | `pattern`, `doors`, opt. `wait` + `walk_to`'s | `walk_to` the door a guest log line names: the last match of `pattern` (one group) in run.log keys `doors` (`{"3" = [4, 2]}`). For doors the game rolls at random (Platinum's Hearthome Gym logs `pc-e2e: hearthome gym map M door D` under `PC_E2E`, pc_np_field.c). |
| `talk_to` | `id` | talk to the map object with that local id wherever it is now (wandering people are chased): a free tile next to it, face it, A until a script starts |
| `advance_text` | opt. `through_battle`, `map` | waits up to 40 frames for a script to start, then A every 8 frames until the field has been free for 30 frames; stops at a battle (unless `through_battle`) or as soon as `map` is loaded |
| `auto_battle` | opt. `move` (slot), `wait`, `flee` | FIGHT every turn with the usable move (PP left, not Disabled) of highest expected damage on the targeted foe: base power x STAB x type effectiveness x accuracy, halved for two-turn moves, cut to a quarter for a move that would hit a live ally (Earthquake beside a tag partner); Self-Destruct/Explosion, Dream Eater, Snore, Fake Out and Focus Punch never; a move every ability of the foe's species blocks (Levitate, Water/Volt Absorb, Dry Skin, Motor Drive, Flash Fire, Wonder Guard) never, and one that only one of its abilities may block is dropped for that species once a use leaves its HP unchanged; status moves only when no damaging move has PP; a move the game refuses is skipped for the turn. A lead with no damaging move left against the foe is switched (POKEMON) for the first healthy party member that has one; a fainted lead is replaced the same way (else the next slot). `move = N` overrides the choice: slot N every turn, the next slot once the game refuses it (also the behaviour when the probe has no battle report). YES/NO prompts: nickname NO, forget a move NO (gives up learning), use the next Pokemon YES, switch NO; otherwise A, so an evolution plays out |
| `fly` | `map`, opt. `slot`, `block` | Platinum Fly from the field, as a player: X, POKEMON (the start menu's remembered cursor tracked per run), the party slot that knows Fly (`slot`, else found in an in-game save's dump), FLY in its context menu (SUMMARY, then its field moves in move order), the town map's cursor moved block by block from the player's overworld block to the destination's (`block = [x, z]`, else the nearest overworld-matrix block whose header is `map`), A; done when `map` loads and the player is free. Dumps the town map before A as `fly-map.ppm`. D/P: give `block` (D/P menus unverified) |
| `steps` | `route`, opt. `face`, `interact`, `run`, `on_battle`, `on_text` | a fixed route, `route = [[x, z], ...]` corners joined by straight lines: each tile's direction held until the player's tile changes, then what the step started (a platform ride, a trainer, text) runs out; landing on the next corner (carried by a platform) counts it reached; three retries per tile. For maps whose collision the probe cannot show (Platinum's Canalave Gym: `tools/pt_gym.py canalave` prints the route) |
| `field_move` | `move`, opt. `slot`, `text` | a field move from the party menu as a player does (Defog, Flash, Teleport, Dig, Sweet Scent, Softboiled): X, POKEMON, the member that knows `move` (a MOVE_* name), the move in its context menu, then the text advanced; the start menu needs the Pokedex and a starter (VAR_PLAYER_STARTER) |
| `fish` | opt. `casts` | Y casts the registered rod; the guest's fishing trace (`[run] env` PC_TRACE_FISH=1) says when the bite window opens and A hooks then; no-nibble casts are closed with B and recast; done when the hooked Pokemon's battle starts (the bite screen goes on the contact sheet) |
| `pace` | `x`, `z`, `until`, opt. `every` | run laps between (x, z) and (x+1, z) until `until` (a Python expression over the save dump `s`) holds, checked by an in-game save every `every` steps (the Day Care's egg roll: `s["daycare"]["egg_waiting"]`) |
| `dump` | `expr` | note `dump: EXPR = VALUE` in the run log, `expr` a Python expression over the dump `s` of an in-game save made now: a mid-run state for an [expect] `log` pattern (the roamers' maps before and after a map change, 110) |
| `hatch` | `x`, `z` | pace between (x, z) and (x+1, z) until no egg is left in the party (no-op without one; checked by an in-game save's dump after each long scene): a chain hatches a carried egg on safe ground instead of mid-puzzle; the nickname question is answered NO |
| `wait_map` | `map` | until that map id |
| `wait_field` | | until the player is free |
| `wait_battle` | | until a battle starts |
| `wait_reset` | opt. `max` | until the game resets itself (Platinum's ClearGame after the credits: `OS_ResetSystem`, trapped by the port); the run ends there, with no end save, shot or probe |
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
