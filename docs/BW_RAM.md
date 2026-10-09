# Pokémon Black / White: the RAM map the e2e probe reads

Black and White have no game source, so the end-to-end probe (core/include/np_e2e.h, tests/e2e) reads the
recompiled cartridge's own structures in guest memory at the frame boundary: `games/ndsrec/pc/src/pc_bw_e2e.c`
(field status every frame, the probe block under `PC_E2E=1`); the e2e judge reads saves with main's `np_save5` (the same
layout, cross-checked below). This file lists every address and offset the probe uses and how each was established. Guest address ==
DS address. Addresses are the USA/Europe cartridges of docs/BW_PLAN.md.

Methods: memory diffs of whole main RAM across controlled inputs (walk one tile, turn, open the menu, talk,
receive the starter) through np_gp's `peek`; the game's heap allocator, which stamps every block with the source
file name and line that allocated it (`fieldmap.c`, `fldmmdl.c`, `game_data.c`, `eventdata_system.c`, ...); a
pointer-graph search from the static BSS to a found heap object; an LLDB hardware read watchpoint on a tile's
terrain entry while the player steps onto it, for the functions that read it; and the recompiled assembly
(`games/ndsrec/build/pc-wasm/<game>/ndsrec/asm`) of those functions. White's equivalents of Black's functions
were found by matching normalised function bodies (addresses and labels stripped); White's heap and BSS objects
sit 0x20 bytes above Black's.

## The chain from the static pointer

| field | Black | White | how it was proven |
| --- | --- | --- | --- |
| GAMESYS pointer | `*0x02146248` | `*0x02146268` | `sub_02011D9C` (the game-system proc init, the function `pc/patch_bw_startup.py` edits) allocates `sub_02011F24()` = 0x34 bytes and stores the block there (`str r6, [r0]` with the literal 0x02146248 / 0x02146268); the same pointer heads the only static chain the pointer-graph search finds to the object system |
| FIELDMAP | GAMESYS+0x14 | same | the `fieldmap.c` block (0x170); 0 while the BAG application runs, the same address again when the field returns (+0x20 holds it too) |
| running event (GMEVENT) | GAMESYS+0x18 | same | 0 while the player is free; non-zero while the X menu is open, while Cheren's dialogue runs, through the save prompt and the save, and through the starter scene; back to 0 after the last A |
| GAMEDATA | GAMESYS+0x1C | same | the `game_data.c` block (0x6CC) |
| zone id | GAMEDATA+0x114 (u16) | same | 0x187 (391) in the player's bedroom; the same id in FIELDMAP+0xD0, in every bedroom object's MMDL (+0x0A) and in the save at 0x19580 |
| player position at rest | GAMEDATA+0x118, VecFx32 | same | 0x58000/0/0x68000 -> 0x78000 after one step down (z tile 6 -> 7); written when a step ends |
| event data (warps, triggers) | GAMEDATA+0x158 | same | the `eventdata_system` block, below |
| party | GAMEDATA+0x194 -> 0x022349AC | -> 0x022349CC | below |
| MMDLSYS | GAMEDATA+0x1A8 | same | the `fldmmdl.c` 0x64 block; every MMDL's +0x88 points back to it (`ov10_0216D67C`: `add r0, #0x88; ldr r0, [r0]`) |
| event work (flags, vars) | GAMEDATA+0x1AC -> 0x0223BCAC | -> 0x0223BCCC | below |
| save image | 0x0221BBAC + save offset | 0x0221BBCC + offset | 32-byte chunks of the save file's blocks (party 0x18E00, trainer 0x19400, 0x1C800, 0x1E200, 0x21500, 0x23500) found in RAM at one constant distance |

## Objects (MMDL)

| field | offset | how it was proven |
| --- | --- | --- |
| slot count | MMDLSYS+0x04 (u16, 0x40) | the array block is 0x401C = 0x40 x 0x100 + header |
| MMDL array | MMDLSYS+0x18 | the `fldmmdl.c` 0x401C block; the bedroom's four live objects (Cheren, the gift box, the player, Bianca) in its first four 0x100-byte slots |
| G3D mapper | MMDLSYS+0x34 | `ov10_0216D52C`: `ldr r0, [r0, #0x34]`, the mapper the movement check passes to the grid query |
| status | MMDL+0x00 (bit 0: in use) | set on the four live objects, clear on the free slots |
| object id | MMDL+0x08 (u16; 0xFF the player) | player 0xFF, Cheren 0, Bianca 1, the gift box 2 (the map's object events at EVENTDATA's object list carry the same ids) |
| graphics id | MMDL+0x0C (u16) | player 1, Cheren 7, Bianca 0x86, the gift box 0xD0 |
| facing | MMDL+0x18 (s16; 0 up, 1 down, 2 left, 3 right) | one-frame taps left, up, right, down turn in place and write 2, 0, 3, 1 (+0x1C and +0x26 follow) |
| tile x, y, z | MMDL+0x3C (s16 each; +0x30 initial, +0x36 previous) | (5,0,6) -> (5,0,7) on a step down, x unchanged; the stored save position agrees |
| position | MMDL+0x44, VecFx32 (16 units a tile, centre +8) | 0x58000/0/0x68000 -> 0x78000; between tiles while a step is under way |

## Terrain: walkability and the step check

| field | Black | White | how it was proven |
| --- | --- | --- | --- |
| grid query (mapper, VecFx32 *pos, out[16]) -> found | `ov21_0218DAEC` | `ov21_0218DB0C` | LLDB read watchpoint on the bedroom tile (5,7)'s 8-byte terrain entry while stepping onto it: every read comes from `ov21_021D14C4` <- `ov21_02193398` <- `ov10_02169800` <- `ov21_0218D9E8` <- `ov21_0218DAEC`, called by the movement check below. It collects the places to stand on the tile under pos (up to 16 layers) and returns the one nearest pos.y: out+8 the MAPATTR, out+12 the height (`ov10_02163E34` / `ov10_02163E58` read those two) |
| off the map (mapper, pos) -> 1 | `ov21_0218DC04` | `ov21_0218DC24` | read: no map loaded, or pos outside blocks-wide x blocks-high (mapper +0x14/+0x16) times the block size (+0x04) |
| MAPATTR | value = low 16 bits, flags = high 16 | same | `ov21_021AB0F0` (`lsl 16; lsr 16`), `ov21_021AB100` (enabled unless the word is 0xFFFFFFFF or the value 0xFF), `ov21_021AB11C` (blocked: not enabled, or flag bit 0, the "hitch"). In the bedroom: floor 0x00800000, walls, furniture, the table, TV and stairs 0x00810001 |
| terrain entries | the loaded block's `WB` container, section 1: u16 32, u16 32, then 32 x 32 entries of 8 bytes (+0: height/normal index, +4: the MAPATTR) | same | the four `field_g3d_map.c` 0x1B024 block buffers; the bedroom's 32 x 32 grid drawn from them matches the screen (desk, bed, shelf, the table under the gift box, the stairs) |
| movement check (mmdl, VecFx32 *from, x, y, [sp] z, [sp+4] dir) -> bits | `ov10_021638CC` | `ov10_021638EC` | the caller in the watchpoint's backtraces (`ov10_02163994` passes the object's own position); bits 1 move limit (`ov10_02163C9C`), 2 attribute (`ov10_02163D04`: hitch over the object's footprint and the per-direction behaviour tables), 4 another object (`ov10_02163BAC`), 8 the height step of 20 units or more / no ground (`ov10_02163E58`), 0x10 off the map (`ov21_0218DC04`) |

The probe's grid cell is `KNOWN | (value & 0xFF) | (hitch ? COLLISION : 0)` from the grid query at the player's
height; its step layers (np_e2e.h v3) are a flood over the movement check for the player from each reached tile,
bits 2, 8 and 0x10 refusing a step (objects are the planner's, from objects[], as on D/P). Ledge jumps are not
reported yet (no ledge in reach to verify them on).

## Warps and triggers (EVENTDATA)

GAMEDATA+0x158 -> the `eventdata_system` block: +0x10 u16 zone, then u16 counts of background events (8), object
events (8), connections (1) and position triggers (1) in the bedroom; +0x1C/+0x20/+0x24/+0x28 their arrays. The
bedroom's one connection reads destination zone 0x186 (the 1F), destination exit 1, centre x 0x98 / z 0x28 units
(tiles 9.5, 2.5: the two stairs tiles (9,2)-(10,2), MAPATTR value 0x18 in the first terrain word); the position
trigger at tile (8,2) fires while var 0x4081 = 1: Cheren's "where are you going?" gate in front of the stairs
(the intro scene sets 1, the two battles set 2: scr 0782 @0x017D / @0x06D2). The ROM's event files (a/1/2/5, as
`tests/e2e/tools/bw_script.py events` decodes them) give the entry sizes: bg 20, object 36, warp 20, trigger 22
bytes, the trigger as {script, value, var, ..., x, z, w, h}; the probe does not publish warps.

## Party, event work, save

| field | Black | White | how it was proven |
| --- | --- | --- | --- |
| POKEPARTY | 0x022349AC | 0x022349CC | u32 slots (6), u32 count: 0 -> 1 when the gift box's starter is received |
| party Pokemon | POKEPARTY+8, 220 bytes each | same | Gen 5 PKM: the 128-byte core shuffled by (PID >> 13) % 24 and XOR-ed with the checksum-seeded LCG, the battle stats with the PID-seeded one; decrypted: species 498 (Tepig), checksum valid, Tackle/Tail Whip with 35/30 PP, level 5, HP 22/22 |
| vars | event work + 2 x (id - 0x4000), 318 of them | same | `sub_020143CC` / `sub_020143E8`: `(id - 0x4000) << 1` added to the work; var 0x4030 0 -> 1 with the starter |
| flags | event work + 0x27C + id / 8, bit id % 8, ids below 0x4000, 0x16C bytes | same | `sub_02014388` / `sub_020143A4` (ids from 0x4000 index a static area instead); flag 0x961 set with the starter |
| trainer | save 0x19400: +0x04 name, +0x14 TID, +0x16 SID | same | "AAAAAAA" (the A-spam name), TID 45994 also in the starter's PKM |
| location | save 0x19580: u32 zone, VecFx32 | same | 0x187 and the tile the save was made on |
| save footer | slot + 0x23F8C u32 save count, + 0x23F94 magic 0x31053527 | same | both 0x24000-byte slots written on every save; the count 2 -> 3 on a second save |

## Battle

Observed in Bianca's first battle (Black, from the bedroom save: the gift box, Tepig, the battle intro, where the
battle waits today, docs/BW_PLAN.md "First battle"). Cross-checked against the unlicensed squiddonaut/pokeblack
disassembly (same Black ROM), which names some of these functions; everything below was proven on our core.

| field | Black | White | how it was proven |
| --- | --- | --- | --- |
| battle view static | `0x021F6398` (overlay 93 .bss) | `0x021F63B8` | overlay 93's view code loads it (`_ov093_021EECCC` and 9 more literals; White's identical functions at +0x20); +0x00 the battle main module, +0x04 its POKECON. Only meaningful while overlay 93 is resident |
| battle main module | `*view` | same | a `procsys.c` heap block (0x490) holding the `btl_setup.c`, `btl_server.c` and `btl_client.c` blocks; the field is gone meanwhile (GAMESYS+0x14 = 0) |
| POKECON | main+0xC8 = `*(view+4)` | same | its +0x00 points back to main; `ov93_021B9940` reads BattleMon pointers by Pokemon id at +0x84 (`ov93_021EED24` calls it with `*(view+4)`), `ov93_021B985C` the clients' POKEPARTYs at +0x74 (= the two `pokeparty.c` blocks), and a second POKECON at main+0x1B0 holds the clients' copies |
| client party | POKECON + 4 + 0x1C x client: BattleMon pointers by slot, u8 count at +0x18 | same | `ov93_021B9864` / `ov93_021B98AC` index parties by `client * 0x1C` from POKECON+4; `ov93_021B9B94` reads the count at +0x18, `ov93_021B9C00` slot i (bounded by it), `ov93_021B9C10` swaps two slots. In the battle: client 0 = Tepig, client 1 = Bianca's Snivy, counts 1 and 1 |
| BattleMon | 0x214 bytes (`btl_pokeparam.c`) | same | +0x00 the source POKEMON (in the client's POKEPARTY), +0x0C species, +0x0E max HP, +0x10 HP, +0x16 ability, +0x18 level, +0xEE..+0xF6 the five battle stats, +0xF8/+0xF9 the current types, +0xFC seven stat stages (6 = neutral), +0x104 four 0x0E-byte move slots {u16 move, u8 PP, u8 max PP, ...}. `ov93_021D4D84` (the constructor) fills +0x0C/+0x10/+0x0E/+0x18/+0x16 from the POKEMON params species / HP / max HP / level / ability, and the level-up code `ov93_021D69B8` reloads only +0x0E from max HP. Values: Tepig 498, 22/22, level 5, Blaze 66, Fire/Fire (9), Tackle 35/35, Tail Whip 30/30; Snivy 495, 19/19, level 5, Overgrow 65, Grass/Grass (11), Tackle, Leer 30/30 |

## The probe on the core

`tests/e2e/tools/probe_map.py --game black` on the bedroom save (CONTINUE free at frame 5295) prints zone 391, the
player at (5,6) facing down, objects 1 (gfx 0x86, Bianca) at (4,6), 2 (0xD0, the gift box) at (5,8) and 0 (7,
Cheren) at (6,6), and the room's walls, desk, shelf, bed, table and stairs as collision around the floor x 1..10,
z 2..8 exactly as the terrain entries above; the step layers (the game's movement check flooded from the player)
cover the same floor. `walk_to (2,4)` then `walk_to (10,5)` take 5 and 9 steps, the shortest paths round Bianca,
Cheren and the table. Milestone 01 (tests/e2e/black) passes on both games with the end save at the tile the probe
reported. From the bedroom saves, the gift box and A presses, `in_battle` goes 1 at frame 8127 (Black) / 8227
(White) as Bianca's battle opens, the field gone (`field` 0); the probe's battle report then reads battler 0 Tepig
(498) L5 22/22 Fire/Fire, Tackle 35 / Tail Whip 30, battler 1 Snivy (495) L5 19/19 Grass/Grass, Tackle 35 / Leer
30, and a party of one Tepig, on both games; the screen at that point is the battle intro before the send-out.

## Not established

- **Battle input and type**: the menu state (`ui`, the battle's own menus) and the battle type bits. Bianca's battle
  waits in its intro on the core today, so no battle menu has been seen; the probe reports the battlers and the
  party but `ui` stays 0, and `menu_battler` and `battle_type` are 0. Only a single battle has been seen: battler n
  is client n's front Pokemon.
- **Menus and text**: no separate text-wait or menu state was needed: the running-event pointer (GAMESYS+0x18) is
  set for the whole of every held scene seen (menu, dialogue, save, the starter scene), and `field_ready` is "a
  field, no event, the player on a tile centre".
