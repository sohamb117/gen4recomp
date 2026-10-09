# tests/e2e: story and system milestones on the real core

The e2e harness plays Platinum, Diamond and Pearl, HeartGold and SoulSilver, Black and White, and Emerald, Ruby and Sapphire, headless, milestone by milestone, from a new game toward the
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
(`build/core-plat`, `build/core-dp`, `build/core-bw`, `build/rse/native`; docs/BUILDING.md, docs/HANDOFF-rse.md) and
builds `np_gp` and `np_save4`/`np_save5` itself.

## Status

Passing (`status` removed from `milestone.toml`), as a continuity chain from a blank chip:

| game | milestones |
|---|---|
| Platinum | 01-43: new game .. Relic Badge, Route 209 .. Cobble/Fen/Mine/Icicle badges, the lakes, Galactic HQ, Mt. Coronet and Spear Pillar into the Distortion World, B1F..B7F and Cyrus |
| Platinum systems | 60-72, 74: every HM field use (Cut, Rock Smash, Strength, Surf, Fly, Defog, Rock Climb, Waterfall), the three rods, a honey tree, the Day Care (deposit with the lady, egg from the man), level-up evolution after a wild battle |
| Diamond | 01-20: new game, Pokedex, Parcel + catching tutorial, Trainers' School, Poketch, Route 203 + Oreburgh Gate, Oreburgh Mine Roark, Coal Badge, Barry's farewell, Jubilife tag battle, Floaroma Meadow, Valley Windworks, Eterna Forest, Forest Badge, Galactic building, Bicycle + Explorer Kit, Cycling Road + VS Seeker, Mt. Coronet + Route 208 to Hearthome, Contest Hall + rival, Route 209 to Solaceon |
| Emerald | 01-48, the whole story: new game .. eight badges (Fortree's rotating gates, Mossdeep's statues, Sootopolis' thin ice from `tools/gba_puzzle.py`), Magma and Aqua hideouts, Seafloor Cavern (Dive, Strength boulders, currents), Sky Pillar, Victory Road, the Elite Four and Wallace, the Hall of Fame and credits to the game's own reset; boosts (`boost.recipe`, party levels, moves and items only) cited per milestone |
| Ruby, Sapphire | 01-44: new game .. Hall of Fame, credits and the game's reset (`sapphire/chain.txt` reuses `ruby/`, with Sapphire-local 21, 25, 32, 35, 36 for Team Aqua, Archie and Kyogre where pokeruby's scripts branch on the version) |
| HeartGold | 01-12: new game from a blank chip, Elm's Cyndaquil and the Pokegear, Mr. Pokemon's egg and the Pokedex, the rival, Route 30/31 to Violet, Sprout Tower and Elder Li, Falkner (Zephyr) and the Togepi egg, Union Cave to Azalea, the Slowpoke Well and Proton, Bugsy (Hive), the Azalea rival, Ilex Forest (both Farfetch'd, Cut), the Day Care, the Radio Tower quiz (Radio Card), Whitney (Plain); `grind` and `heal` where an honest lead needs levels (02, 05) |
| SoulSilver | 01-03 (the shared dirs, same core) |
| Black, White | 01: new game, the bedroom walked by the probe, Cheren, the first save through the X menu |

Everything else is still `status = "planned"` and skipped unless `--planned`.

## The GBA games (Emerald, Ruby, Sapphire)

The same runner, bots and milestone format, with these differences:

- **Probe**: np_e2e.h version 4, published by `games/gba-common/pc/src/gba_e2e.c` and the game halves
  `games/emerald/pc/src/emerald_e2e.c`, `games/ruby/pc/src/ruby_e2e.c` from the status hook at every frame
  boundary (the probe's calls into game functions do not count as game CPU time: `gba_tick`'s phase is put back).
  The grid is the game's own `MapGridGet*At` queries; grid cells also carry the tile's elevation and
  `NP_E2E_TILE_CONNECTED` (a connected map's tile seen across the border). Step layers are a flood over the player's
  on-foot step check, with heights = elevations. v4 adds the menu cursor (`ui_cursor`), the avatar flags, the guest
  addresses of the flags, vars and party (read with `Session.peek`), `connection_seq`, `soft_resets` and the map's
  warps.
- **Coordinates** are each map's own (map.json). A `walk_to` goal across a map border (a connected tile, e.g.
  Littleroot's (10,-1) for Route 101) ends the walk where the player crosses into that map. Any other crossing is
  an error. `walk_to` with `map` naming another map routes there over the world's maps (`gba_world.py`).
- **Saves** are edited and read on the host: `tools/gba/gen3_dump.py dump|gamedata` gives the JSON shapes
  `np_save4` gives. Lab and boost recipes run through `tools/gba/gen3_lab.py` (D/P's verbs; `map MAP X Z`; `badge 1..8`)
  on a new-game base save, the house 1F after `tests/rse/littleroot.sched`. The GBA guest leaves storing its flash
  chip to the host, so the bots `flush` (np_gp's serve command) after an in-game save.
- **Battles** take buttons: `auto_battle` makes the DS choices and moves the cursor the probe reports.
- **Names** come from the decomp headers (`labc.py --game emerald|ruby|sapphire`). Badges are `badges = N` plus
  the `FLAG_BADGE0N_GET` flags. A `talk_to` id is the object's 1-based index in map.json.
- **The end**: the credits end in the game's own `SoftReset`; `wait_reset` sees the probe's `soft_resets` go up.
- **Waiting**: `press` takes `until` (np_gp's condition syntax) for screens only the probe sees (the wall clock
  taking over the field: `until = "field=0"`).

## HeartGold and SoulSilver

The DS harness as Platinum and D/P use it, with these differences:

- **Probe**: `games/heartgold/pc/src/pc_hg_field.c` (game C, compiled into both guests by pc/Makefile.wasm) through
  Platinum's shared `pc_e2e.c`. The FieldSystem is field_system.c's `sFieldSysPtr` (getter added by
  `pc/patches/src/field_system.c.patch`). The grid and the step layers are D/P's: the terrain provider's tile word
  (`GetMetatileBehavior`, `sub_020548C0` collision) and a flood over the player's own step check `sub_020549F4`
  (plate heights, Gymmick_CheckCollision, the collision bit; heights from `sub_02054940`), only on the map matrix.
  The walking Pokemon is not listed among the objects. v4 fields: `avatar_flags` bit n = PLAYER_STATE_* n (walking,
  cycling, surfing), the flags/vars (SaveVarsFlags) and party (Party.core.mons, encrypted as on the cartridge)
  addresses, the map's warps (WarpEvent: tile, header, anchor) and, in the battle menu, `ui_cursor` = the raw
  BattleMenuCursor (menuY << 4 | menuX, 0 while hidden). The battle report comes from `BattleInput_CheckTouch`
  (`pc/patches/src/battle/battle_input.c.patch`), the party screen's two checks from overlay 8
  (`pc/patches/asm/overlay_08.s.patch`), `in_battle` from `pc/patches/src/encounter.c.patch`. HG/SS's battle menu
  ids and touch rects are Platinum's, so `auto_battle` taps the same points.
- **Coordinates** are world tiles outdoors, as the zone_event JSON (`files/fielddata/eventdata/zone_event/`) gives
  them; a `talk_to` id is the object's number in `files/fielddata/script/scr_seq/event_<MAP>.h`.
- **Names**: pokeheartgold's `include/constants/*.h` (labc.py `--game heartgold|soulsilver`): `MAP_NEW_BARK`,
  `FLAG_*`, `VAR_*`, `BADGE_ZEPHYR`..; `np_save4 dump` reads HG/SS saves (`trainer.badges` counts Johto's, Kanto's
  are `kanto_badges`) and `np_save4 gamedata` HG/SS ROMs.
- **Saves**: no HG/SS save lab yet, so every milestone starts from the previous one's end save (01 from a blank chip,
  the intro schedule of docs/HANDOFF-hgss.md); lab recipes are refused. A boost (`[start] boost`) takes only
  `party SPECIES LEVEL` and `party-move SLOT INDEX MOVE` for a Pokemon it adds: run.py's `addmon_boost` turns each into
  `np_save4 add-mon` after the save's party (HM carriers, since no bot teaches an HM through the Bag; strength for
  the boss fights, which `auto_battle` reaches with `send = "best"`). SoulSilver's chain lists HeartGold's dirs and
  its own `31-whirl-islands-lugia`.
- **Heal**: every Pokemon Center 1F has the nurse at (8,11) and the exit at (8,19).
- **Fly and field moves** (bots.py `_hgss_open_party_move`, `_hgss_fly`; the probe sees none of these screens, so every
  key count is the decomp's and the bots note the presses they make; seen at runtime on HeartGold after 03: X opens the
  menu with the cursor on POKEDEX, DOWN reaches POKEMON, A opens the party on slot 0 and A its context menu SUMMARY,
  SWITCH, ITEM, QUIT; the field-move entries and the fly map are not yet seen): X opens overlay 27's
  bottom-screen menu, slots POKEDEX, POKEMON, BAG, POKEGEAR (left column) and TRAINER CARD, SAVE, OPTIONS (right),
  each shown when its flag is set (FLAG_GOT_POKEDEX, _STARTER, _BAG, _POKEGEAR, _TRAINER_CARD, _SAVE_BUTTON,
  _OPTIONS_BUTTON, read from an in-game save's dump); the D-pad follows ov27's neighbour table and A takes the
  remembered cursor (FieldSystem.unkD3, zero at boot, tracked per run: a step that opens the menu by hand must leave
  it on POKEMON). The party menu opens on slot 0, RIGHT steps through the slots, A opens the context menu (SUMMARY,
  SWITCH, ITEM, CANCEL, then the field moves in move order: LEFT reaches the first, DOWN the next). FLY opens the
  Pokegear map in fly mode on the player's overworld block (off the overworld: the map header's worldMapX/Y, else
  give `start`); each 2-frame press moves the cursor one block; A on a fly point (`tools/hg_world.py fly`: rects,
  landing tiles) opens FLY/CANCEL on FLY, A flies. Fly only within the player's region (Johto/Kanto; the Indigo
  Plateau and Route 26 always) and only from maps with flyAllowed; `dig = true` first Digs out of a cave that allows
  it (SoulSilver's Lugia cave). The fly map is not the Pokegear's Map Card app.
- **The Pokegear**: no story step needs the player to operate the radio or the map. The scripted phone calls (the
  `PhoneCall` script macro -> std_phone_call -> RunPhoneCall: Elm on Route 30 / in Violet, Cianwood, Olivine,
  Mahogany, Blackthorn; Mom on Route 30; Baoba in Olivine) open the Pokegear phone and leave it on the contact list
  when the call ends (PhoneCall_Exit -> PokegearPhone_ReturnToContactList), where A would place another call: run
  them out with `advance_text` `key = "b"` (B advances the call text and its last wait, then the first B moves the
  cursor to the app row and the second closes the Pokegear: render_text.c:57, PhoneCall_WaitButtonBeforeHangup,
  PokegearPhone_HandleKeyInput_ContactList, PokegearApp_HandleKeyInput_SwitchApps).
- **World tools**: `tools/hg_world.py fly` (fly destinations), `grid X0 Z0 X1 Z1` (overworld tiles as ASCII),
  `route X0 Z0 X1 Z1 --surf` (a static path over the land data, for `via` waypoints of long surf routes);
  `tools/hg_map.py` per map.

## Black and White

ROM-only recompilations (docs/BW_PLAN.md): `--game black|white` takes the cartridge from the checkout's gitignored
`roms/` (or `NP_ROM`) and one core with both guests, `build/core-bw` (or `NP_CORE_BUILD`). With no game source the
probe is host code reading the recompiled game's own structures, `games/ndsrec/pc/src/pc_bw_e2e.c`; every address
and how it was proven is in docs/BW_RAM.md. Differences:

- **Probe**: the field (zone id as map_id, the player's tile and facing, the map objects), the grid from the game's
  own terrain query (collision = the attribute's blocked flag; the attribute values are not mapped to behaviors, so
  the planner gives none of them a meaning) and step layers from the game's own object movement check.
  `field_ready` is "a field, no event (script, menu, warp) running, the player on a tile centre". In a battle
  `in_battle` is 1 and the battle report carries each client's front Pokemon and the player's party (overlay 93's
  POKECON); `ui` is `UI_BATTLE_MENU` while the bottom screen's action menu (ui_arg 1) or move list (11) waits for
  input. `auto_battle` drives those with the D-pad; the screens the probe does not report are handled in bots.py:
  the YES/NO prompts (forget a move?, stop learning?) by peeking the battle's input screen (`_bw_input`), the forced
  replacement after the lead faints by touch (`TAP_PARTY`, then SHIFT), the trainer's "Will you switch?" by B.
- **Saves**: `save` (and the end save) goes through the game's X menu (SAVE, then A); the host's quick save is
  refused. `features/tools/np_save5 dump` reads the end save in np_save4's shapes (`location`, `trainer`, `party`,
  `flags`, `vars`).
- **Names**: none; zones, flags, vars and species are numbers. No lab recipes; a boost (`[start] boost`) takes
  `party SPECIES LEVEL` and `party-move SLOT INDEX MOVE` as HG/SS's do (run.py's `addmon_boost`, `np_save5
  add-mon`): HM carriers, since no bot teaches an HM through the Bag. Give an added Pokemon its moves, or it has
  none and battles with Struggle.
- **Boot**: CONTINUE waits for the opening movie (START at frame 5000).

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
   `OS_ResetSystem` after the credits: the port reboots the guest (the `resets` status goes up), and `end.sav` is the save the game wrote before.
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
| `walk_to` | `x`, `z`, opt. `via`, `map`, `face`, `interact`, `run`, `on_battle`, `on_text`, `surf`, `hm`, `dive` | A* over the probe's tile window (collision, ledges as one-way jumps, water, bike slopes and one-way blocks avoided, tall grass costs 6 so paths go round it), re-planned every step (a failed plan re-reads the probe once: right after a warp it can hold the last map); `surf = true`: surfable water is walkable, a bump from land into it (and up a waterfall) is A + YES; `hm = true`: Cut trees, Rock Smash rocks and Rock Climb walls are on the path (cost 4) and a bump into one is A + YES (Strength boulders are not: push them with steps); the party needs the move; holds the direction through the turn-in-place; bumps teach blocked edges; NPCs are obstacles. Border crossings outdoors are seamless; walking into a door/warp that is the goal takes it; a goal on an exit mat pushes off it. Battles on the way are fought (`auto_battle`) and do not count against `max`; text and cutscenes on the way are advanced with B (a YES/NO answered NO: the nickname an egg hatching on the way asks for). `on_text = "stop"`: a script that holds the player on the way (a coordinate trigger's scene, whose menus B would cancel) ends the walk there, for the next steps to play. `via = [[x, z], ...]`: waypoints walked first, each under its own `max`, for routes wider than the 64x64 window or past what A* cannot see (a bridge deck vs the path under it).; GBA `dive = true` (with `map`): the world route may also dive (A + YES on deep water) and surface (B + YES underwater) through the maps' dive/emerge connections and fixed dive warps (gba_world.py, Dive); the party needs Dive and the seventh badge |
| `slide` | `dirs`, opt. `max`, `on_battle`, `on_text` | ice: each direction (space separated) is a press, then a wait until the player has stopped (tile unchanged over six probes, free); battles and text on the way are handled as walk_to does (D/P Snowpoint Gym, 44) |
| `walk_to_door` | `pattern`, `doors`, opt. `wait` + `walk_to`'s | `walk_to` the door a guest log line names: the last match of `pattern` (one group) in run.log keys `doors` (`{"3" = [4, 2]}`). For doors the game rolls at random (Platinum's Hearthome Gym logs `pc-e2e: hearthome gym map M door D` under `PC_E2E`, pc_np_field.c). |
| `talk_to` | `id`, opt. `surf`, `hm`, `avoid` | talk to the map object with that local id wherever it is now (wandering people are chased): a free tile next to it, face it, A until a script starts; `surf`/`hm`/`avoid` go to its walks as walk_to's (an object on the water: HG/SS's red Gyarados) |
| `advance_text` | opt. `through_battle`, `map`, `key` | waits up to 40 frames for a script to start, then A (`key = "b"`: B) every 8 frames until the field has been free for 30 frames; stops at a battle (unless `through_battle`) or as soon as `map` is loaded. B answers YES/NO as NO; HG/SS scripted phone calls need it (HeartGold and SoulSilver) |
| `auto_battle` | opt. `move` (slot), `wait`, `flee` | FIGHT every turn with the usable move (PP left, not Disabled) of highest expected damage on the targeted foe: base power x STAB x type effectiveness x accuracy, halved for two-turn moves, cut to a quarter for a move that would hit a live ally (Earthquake beside a tag partner); Self-Destruct/Explosion, Dream Eater, Snore, Fake Out and Focus Punch never; a move every ability of the foe's species blocks (Levitate, Water/Volt Absorb, Dry Skin, Motor Drive, Flash Fire, Wonder Guard) never, and one that only one of its abilities may block is dropped for that species once a use leaves its HP unchanged; status moves only when no damaging move has PP; a move the game refuses is skipped for the turn. A lead with no damaging move left against the foe is switched (POKEMON) for the first healthy party member that has one; a fainted lead is replaced the same way (else the next slot). `move = N` overrides the choice: slot N every turn, the next slot once the game refuses it (also the behaviour when the probe has no battle report). YES/NO prompts: nickname NO, forget a move NO (gives up learning), use the next Pokemon YES, switch NO; otherwise A, so an evolution plays out |
| `fly` | `map`, opt. `slot`, `block`, `start`, `dig` | Platinum Fly from the field, as a player: X, POKEMON (the start menu's remembered cursor tracked per run), the party slot that knows Fly (`slot`, else found in an in-game save's dump), FLY in its context menu (SUMMARY, then its field moves in move order), the town map's cursor moved block by block from the player's overworld block to the destination's (`block = [x, z]`, else the nearest overworld-matrix block whose header is `map`), A; done when `map` loads and the player is free. Dumps the town map before A as `fly-map.ppm`. D/P: give `block` (D/P menus unverified). HG/SS (unverified at runtime): `map` a fly destination (`tools/hg_world.py fly`), done when its landing map loads; `dig = true` digs out of a cave that forbids Fly first (HeartGold and SoulSilver) |
| `steps` | `route`, opt. `face`, `interact`, `run`, `on_battle`, `on_text` | a fixed route, `route = [[x, z], ...]` corners joined by straight lines: each tile's direction held until the player's tile changes, then what the step started (a platform ride, a trainer, text) runs out; landing on the next corner off the line to the current one (carried by a platform) counts it reached; three retries per tile. For maps whose collision the probe cannot show (Platinum's Canalave Gym: `tools/pt_gym.py canalave` prints the route; the GBA gym puzzles, Fortree's rotating gates and Emerald Mossdeep's statues: `tools/gba_puzzle.py --game emerald MAP X Z X Z`) |
| `push` | `dir`, opt. `on_battle` | GBA Strength: push the boulder ahead in `dir` one tile; when FLAG_SYS_USE_STRENGTH is clear (every map load clears it) A on the boulder and YES first; a wild battle that cuts in is fled and the push tried again; done when the boulder has left the tile (`tools/gba_puzzle.py` prints these for boulder rooms) |
| `smash` | `dir`, opt. `on_battle` | GBA Rock Smash: A on the rock ahead in `dir` and YES, the wild battle it may start fought; done when the rock has gone |
| `field_move` | `move`, opt. `slot`, `text` | a field move from the party menu as a player does (Defog, Flash, Teleport, Dig, Sweet Scent, Softboiled; HG/SS also Headbutt, Whirlpool): X, POKEMON, the member that knows `move` (a MOVE_* name), the move in its context menu, then the text advanced; the start menu needs the Pokedex and a starter (VAR_PLAYER_STARTER). HG/SS menus: HeartGold and SoulSilver (unverified at runtime) |
| `fish` | opt. `casts` | Y casts the registered rod; the guest's fishing trace (`[run] env` PC_TRACE_FISH=1) says when the bite window opens and A hooks then; no-nibble casts are closed with B and recast; done when the hooked Pokemon's battle starts (the bite screen goes on the contact sheet) |
| `pace` | `x`, `z`, `until`, opt. `every` | run laps between (x, z) and (x+1, z) until `until` (a Python expression over the save dump `s`) holds, checked by an in-game save every `every` steps (the Day Care's egg roll: `s["daycare"]["egg_waiting"]`) |
| `dump` | `expr` | note `dump: EXPR = VALUE` in the run log, `expr` a Python expression over the dump `s` of an in-game save made now: a mid-run state for an [expect] `log` pattern (the roamers' maps before and after a map change, 110) |
| `hatch` | `x`, `z` | pace between (x, z) and (x+1, z) until no egg is left in the party (no-op without one; checked by an in-game save's dump after each long scene): a chain hatches a carried egg on safe ground instead of mid-puzzle; the nickname question is answered NO |
| `wait_map` | `map` | until that map id |
| `wait_field` | | until the player is free |
| `wait_battle` | | until a battle starts |
| `wait_reset` | opt. `max` | until the game resets itself (Platinum's ClearGame after the credits: `OS_ResetSystem`; the port reboots the guest and the `resets` status goes up); the game ends there: no end save or probe, the end shot is the rebooted game |
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
