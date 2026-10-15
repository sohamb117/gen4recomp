# Black / White and HeartGold / SoulSilver: the game-side options

The Gen1Recomp rows that need the game's cooperation (docs/FEATURE_PARITY.md, "Black / White and HeartGold /
SoulSilver"): camera zoom and tilt, 3D render scale, widescreen 3D, the music / effects volume split, F1 quick save,
instant text, the fix-cartridge-bugs rules, mods and custom carts, and the battle layout's `NP_STAT_IN_BATTLE`. For
each: how Diamond/Pearl and Platinum do it today, then the plan for HG/SS (the pokeheartgold decomp,
`games/heartgold`) and for B/W (ROM-only, `games/ndsrec`), then a ranking.

This is a design. Nothing in it is built. The measurements in it are np_headless runs of the 2026-10-08
proposal cores with `-o` options (frames in `build/evidence/bwhgss/hooks/`, outside git), the SDAT tables
read from the four ROMs, and the code as of main.

## The contract and where it lands

The shell sends options in the frame descriptor (`core/include/np_guest_abi.h`: `NP_OPT_BGM_VOLUME` 0,
`SE_VOLUME` 1 (0..256), `RENDER_SCALE` 2 (1..4), `WIDESCREEN` 3, `CAMERA_ZOOM` 4 (64..1024, 256 the cartridge),
`CAMERA_TILT` 5 (1/16 degree, ±45°), `QUICKSAVE_SEQ` 6, `RULES` 7 (`NP_RULE_FIX_BUGS` bit 0), `TEXT_INSTANT` 8)
and reads status back (`FIELD_READY` 1, `QUICKSAVE_SEQ` 2, `QUICKSAVE_RESULT` 3, `MAP_ID` 4, `IN_BATTLE` 5). In
every DS core the guest end is Platinum's `pc/src/pc_view.c`: `view_wasm_options` copies the options into
`pc_np_opt` through `pc_np_options_set` (`pc/src/pc_np_options.c`), `view_wasm_geometry` re-aims the renderer,
and `view_wasm_status` calls the game's `pc_np_frame` and publishes `pc_np_stat`
(`IN_BATTLE = in_encounter || in_battle_app`).

HG/SS and B/W already link all of that: `games/heartgold/pc/Makefile.wasm` and `games/ndsrec/pc/Makefile.wasm`
include Diamond's `pc/mk/host.mk`, which compiles Platinum's `pc/src` (minus `pc_np_field.c`), `pc/hw`, the
ARM7 sound driver (`pc/arm7snd`) and Diamond's `pc/src` (including `pc_dp_snd.c`). Each game then has its own
`pc_np_frame`:

- HG/SS: `games/heartgold/pc/src/pc_hg_field.c` (game C). It already publishes `field_ready` (the moment X
  would open the start menu) and `map_id`, answers F1 through `Field_SaveGameNormal` behind the start menu's
  own gates (`save_allowed`: link-room load types, Mystery Zone, Safari / Bug Contest / Pal Park flags, the
  Battle Tower partner room, another player's file), and `pc/patches/src/encounter.c.patch` sets
  `in_encounter` from `Encounter_New` / `WildEncounter_New` to their `_Delete`. None of it can run yet: the
  first field load stops at overlay 123 (`ds_protect`, docs/HANDOFF-hgss.md).
- B/W: `games/ndsrec/pc/src/pc_bw_e2e.c` (`bw_frame`). It publishes `field_ready` (GAMESYS+0x18 event pointer
  0, the player on a tile centre) and the zone, and refuses every quick save at once. It already calls
  recompiled functions on its own 32 KiB guest stack (`pc_np_frame` swaps `armrec_sp`, `ARMREC_CALL`), which is
  the pattern every B/W hook below uses.

How a hook reaches game code differs per game:

| | D/P (recompiled asm + decomp C) | Platinum (decomp C) | HG/SS (decomp C + asm) | B/W (ROM-only asm) |
|---|---|---|---|---|
| patch C | `pc/patches/arm9/src/*.c.patch` | `pc/patches/src/*.c.patch` | `pc/patches/src/*.c.patch` (`pc/mk/game.mk`) | none (no C) |
| patch asm | `*.s.patch`: `bl X` -> `bl PcDp_Hook`, or an instruction pair -> `bl Hook; nop`, size for size (`overlays/05/asm/ov05_021D74E0.s.patch`, `overlays/11/asm/ov11_0223D1DC.s.patch`) | n/a | `pc/patches/asm`, `lib/asm` `.s.patch` (`pc/mk/armrec.mk`) | `pc/patches/<VER>/*.s.patch` on the emitted assembly (`pc/mk/ndsrec.mk`), no fuzz, optional `SHA256SUMS` |
| replace a function | strong C symbol over a weak SDK body (`FS_OpenFile`, `pc_dp_modfs.c`) | C | `pc/host_overrides.txt` | `tools/ndsrec/primitives.txt` `override` (signature-matched from Diamond: 71 of 136 placed, docs/BW_PLAN.md) |
| call game code | direct C call | direct C call | direct C call | `ARMREC_CALL` / `armrec_call_code(addr, r0..r3)` on a host-owned guest stack |
| read game state | C structs | C structs | C structs | RAM map, docs/BW_RAM.md (GAMESYS `*0x02146248` Black / `*0x02146268` White) |

For B/W the D/P `.s.patch` is the model: the emitted assembly is deterministic per ROM, so a per-version patch
under `games/ndsrec/pc/patches/<version>/`, applied by `pc/mk/ndsrec.mk` after `ndsrec.py emit` with no fuzz
(and pinned by `SHA256SUMS` where an exact result is reviewed), can retarget a `bl` to host C. The startup
proposal (formerly `pc/patch_bw_startup.py`) is the first such patch.

## Per feature

### 3D render scale and widescreen 3D: no hook needed

- **D/P/Pt.** Renderer-side, not game code: `pc_view.c` `view_wasm_geometry` calls `pc_gpu3d_set_hd`,
  `pc_gpu3d_soft_set_scale`, `pc_gpu2d_set_hd` and, for widescreen, `pc_gpu3d_set_wide` /
  `pc_gpu3d_soft_set_width` (342 columns, `PC_VIEW_WIDE_MAX`); the geometry engine widens the clip X so the
  extra columns are more world, not stretched pixels (`pc/hw/pc_gpu3d.c`).
- **B/W: works today.** Black's title with `-o render_scale=2` draws Reshiram at 512x768; the bedroom (from
  the parity save, CONTINUE) at 512x768 is sharp; `-o widescreen=1` gives 342x384 frames with more of the room
  (both side walls in full where the 256 frame cuts the right one). Frames: `black-title-scale2.png`,
  `black-bedroom-scale2.png`, `black-title-widescreen.png`, `black-bedroom-widescreen.png`.
- **HG/SS: works in the field (2026-10-08).** New Bark Town renders at 512x768 with `render_scale=2` and at
  342x384 with `widescreen=1`, the wide frame showing more of the town (`heartgold-field-*.png`, parity.sh).
  `ov01_021E6220`'s depth tweak of `NNS_G3dGlb.projMtx` (`_32` only) leaves the wide clip X alone, as expected.
- **Remaining work:** app-level cases (`n2_render`: `render_scale = 2`, `widescreen = 1` in the slot's
  options, frame size and a screenshot) and the parity rows. No D/P/Pt inputs.

### Music / effects volume split: a per-game player table

- **D/P/Pt.** Model-side, in the shared ARM7 driver. `pc_np_options_set` turns the two volumes into tenths of
  a dB in `arm7_SND_HostPlayerDecay[16]`, which `arm7snd/src/SND_seq.c` adds per sequence player in
  `TrackUpdateChannel`. The driver player is mapped back to the sound-archive player by
  `pc_np_seq_player_group` (Platinum: a `player.c` patch; D/P: `games/diamond/pc/src/pc_dp_snd.c`, reading
  NitroSystem's `sSeqPlayer[16]` / `sPlayer[32]` at their guest addresses). The music players are
  hard-coded as Platinum's SDAT: `player_is_music` = 1 FIELD, 2 ME, 7 BGM.
- **Measured on Black:** `-o bgm_volume=0` leaves the title's rms at 7470 (L) and `-o se_volume=0` takes it
  to 0.0. The mapping itself works: B/W links `pc_dp_snd.c`, and Black's `sSeqPlayer` (0x0214E024) and
  `sPlayer` (0x0214E464), both signature-matched, are 0x440 = 16 x 68 bytes apart, D's stride. The table is
  what's wrong. Black's and White's SDAT players are 0 PLAYER_BGM, 1 SE_SYS (cries too: SEQ_PV is on 1),
  2 SE_1, 3 SE_2, 4 SE_PSG, 5 SE_3, 6 PLAYER_BGM2, and the title music (SEQ_BGM_TITLE) plays on 6. So both B/W
  music players are classed as effects.
- **HG/SS:** players 0 PV, 1 FIELD, 2 ME, 3..6 SE_1..4, 7 BGM (Platinum's, also
  `include/constants/sndseq.h`) plus 8 PLAYER_OPED, the opening / ending music, which Platinum's table would
  class as effects.
- **B/W: done (2026-10-08).** `games/ndsrec/pc/src/pc_bw_snd.c` is the B/W `pc_np_seq_player_group`: the
  same two arrays, B/W's players folded onto Platinum's classes (0 and 6 answer 7, the rest 3), and
  `pc/Makefile.wasm` links it in place of `pc_dp_snd.o` for Black and White. `pc_np_options.c` only ever
  sees Platinum's numbering, so D/P/Pt are untouched. Black's title (rms from frame 4000, L): 7492 by
  default, 47 with `bgm_volume=0`, 7491 with `se_volume=0` (parity.sh `title, bgm_volume 0` / `se_volume 0`,
  on both games).
- **HG/SS: no code needed (2026-10-08).** HG/SS's players are Platinum's (1 FIELD, 2 ME, 7 BGM music), and
  Diamond's `pc_dp_snd.c`, which HG/SS link, maps driver players back to them. PLAYER_OPED (8) has no SDAT
  sequences and nothing in `src/` plays on it, so there is nothing to fold. Measured once HG/SS made sound
  (main 9bfca2d74): the title rms 6591 by default, 0 with `bgm_volume=0`, 6295 with `se_volume=0` (both
  games); New Bark Town 2407 -> 0 / 2407 (parity.sh `title, bgm_volume 0` / `se_volume 0`).

### F1 quick save

- **D/P/Pt.** `pc_np_frame` (Pt `pc/src/pc_np_field.c`; D/P `pc/game/pc_dp_field.c` `np_frame`) waits up to
  60 frames (`QUICKSAVE_PATIENCE`) for `field_ready`, applies the start menu's refusals, and calls the game's
  own field save (`FieldSystem_Save` / `Field_SaveGame`) on a host guest stack (`pc_dp_on_guest_stack`). The
  result is saved / refused / failed.
- **HG/SS: proven in the field (2026-10-08).** `pc_hg_field.c` `np_frame`, `field_ready`, `save_allowed` and
  `quicksave_done` mirror D/P with `Field_SaveGameNormal` (overlay 1, the same call `TouchSaveApp_SaveGame` and
  `ScrCmd_SaveGameNormal` make). In New Bark Town `-o 2300:quicksave_seq=1` saves in that frame, np_save4
  verifies (the general and storage blocks' count +1), and CONTINUE comes back to map 60.
- **B/W: done (2026-10-08)**, in `games/ndsrec/pc/src/pc_bw_e2e.c` (`quicksave_frame`). There is no
  synchronous save in B/W: the X menu's SAVE (`ov10_02169AB8`, its state the GMEVENT's seq) and the script
  SAVE command (`ov10_02159A64`) both start with `sub_02012DAC(GAMEDATA)` (which writes the live GAMEDATA
  back into the save blocks and starts the asynchronous write) and poll `sub_02012DD0(GAMEDATA)` every
  frame (0 / 1 writing, 2 saved, 3 failed). Before the start they add 1 to the "times saved" record
  (`sub_02008DF0(sub_02012F2C(gd), 1)`). The menu refuses when `sub_020071F0(savecontrol)` is 1, and a save
  is running while `GAMEDATA+0x1CE` is set. The quick save does the same, from `field_ready`. It also holds
  the player for the write the way the game does: an event of the game's own (`sub_020122C0` create,
  `sub_02012108` make it the running one) whose function is `ov10_02161340` ("done once seq is not 0",
  White `ov10_02161360`); setting its seq to 1 when the write ends lets the game's runner free it. A host C
  function cannot be the event function: on wasm a C function pointer is a table index the dispatcher does
  not know. The static functions are at the same addresses in White. Found by watching the menu's event
  chain and its seq (`--watch`, no debugger: lldb cannot attach here). On Black's bedroom save, `-o
  5600:quicksave_seq=1` saves by frame 5685 (the footer's count 2 -> 3 at 5672), the player is free again
  at 5686, and np_save5 verifies the file (parity.sh `F1 quick save in the bedroom`).

  Refusals still missing: the X menu's own list conditions (where it greys SAVE out) were not found. Only
  `field_ready` (the field, no event, the player on a tile centre) and the two checks above gate it.

### Instant text

- **D/P/Pt.** Pt `pc/patches/src/text.c.patch` wraps `RenderFont`'s result in `PcNp_RushPrinter`: while the
  option is on and the printer is only handing out characters (`RENDER_PRINT` / delay), it keeps rendering
  within the frame. It stops at waits, scrolls, pauses and the finish, so input and callbacks are unchanged.
  D/P: the same rule on D's printer (`pc/patches/arm9/src/text.c.patch`).
- **HG/SS: done (2026-10-08).** HG's `src/text.c` has the same `RunTextPrinter` / `RenderFont` pair as
  Platinum's (HG's `unk2E` is Platinum's `callbackParam`), so `games/heartgold/pc/patches/src/text.c.patch`
  is Platinum's patch with HG's names. Proven on Prof. Oak's introduction (`tests/bwhgss/hgss-intro.sched`):
  with `-o 7284:text_instant=1` the first page ("Huh? It's already become so bright outside!") is whole at
  frame 7285, where the cartridge has drawn "H", and stays until the tap; with the option on from frame 0
  the intro still reaches the naming screen (parity.sh `instant text (Oak)`,
  `build/evidence/bwhgss/hooks/heartgold-text-*.png`).
- **B/W: done (2026-10-08).** B/W print through a print stream: a task (Black `sub_0201CEE0`, White
  `sub_0201CEFC`) with its work block (+0x00 state: 0 running, 1 waiting for a button, 2 done; +0x14 the
  current code; +0x1E the per-character delay; +0x24 / +0x2C / +0x30 the callback and its arguments; +0x34
  the glyph job). Each frame it renders one glyph through `sub_0201C974` (White `sub_0201C990`) and handles
  the 0xBEnn control tags through `sub_0201D110` (White `sub_0201D12C`): BE00 / BE01 wait for a button,
  BE02 waits N frames. `pc/patches/<VER>/arm9/asm/ndsrec_arm9_006.s.patch` retargets the task's glyph call
  to `PcBw_RushPrinter` (`pc/src/pc_bw_text.c`). Off, it is that call. On, it carries on within the frame
  through text, newlines and BE03..BE09 tags, running the callback after each item as the task does. It
  stops at the end, at BE00..BE02, when the callback is busy, or when the stream leaves the running state.
  Proven on the X menu's "Would you like to save the game?" (`-o 22090:text_instant=1`): whole, with YES/NO
  open, by frame 22101, where the cartridge has drawn "W"; the save then completes and verifies (parity.sh
  `instant text (the save prompt)`).

### Camera zoom and tilt

- **D/P/Pt.** Around the field's 3D draw, never touching the game's camera. Pt: `fieldmap.c.patch` calls
  `pc_np_camera_begin(fieldSystem->camera)` after `Camera_ComputeViewMatrix` and `pc_np_camera_end` after
  `G3_RequestSwapBuffers`. `pc_np_field.c` `pc_np_camera_begin` copies the camera, scales the distance by
  `zoom/256`, adds the tilt to the pitch, adjusts near/far, and loads `NNS_G3dGlbLookAt` / the projection;
  `end` restores. D/P: `ov05_021D74E0.s.patch` retargets `bl Camera_PushLookAtToNNSGlb` -> `bl
  PcDp_FieldPushLookAt` and `bl sub_020222B4` (the swap) -> `bl PcDp_FieldSwapBuffers` (`pc_dp_field.c`).
- **HG/SS: done (2026-10-08).** `src/field/fieldmap.c` `ov01_021E6220` is the field draw:
  `Camera_PushLookAtToNNSGlb()` (which advances the game's camera and loads its view), the map, props and the
  projection's depth tweak, then the swap request. `pc/patches/src/field/fieldmap.c.patch` calls
  `pc_np_camera_begin(fieldSystem->camera)` right after the push and `pc_np_camera_end` after the swap request.
  The two are in `pc_hg_field.c`: Platinum's maths on a copy of HG's `Camera` (distance x zoom, pitch + tilt
  clamped to 5..85 degrees below the horizon, the position as `Camera_CalcLookAtPosFromTargetAndAngle` forms
  it, near / far widened as Pt does), loaded with `NNS_G3dGlbLookAt` and `NNS_G3dGlbPerspective` / `Ortho`
  directly (`Camera_ApplyPerspectiveType` would also set the depth buffering mode). The draw's depth tweak then
  applies to this projection, and `_end` copies the game's saved projection back. Copied, not shared, so
  D/P/Pt's inputs are untouched; the cutscene / photo cameras (`field_take_photo.c`, title, starter app) are
  untouched. In New Bark: zoom 512 shows the whole town, 128 the player close up, tilt 20 degrees a flatter
  view with the houses' fronts, and with both options set back to the defaults ten frames before the dump the
  frame is byte-identical to the plain one (`heartgold-camera-*.png`, parity.sh `camera zoom`).
- **B/W: done** (`pc/src/pc_bw_camera.c`). B/W's 3D camera is the GFL library's `GFL_G3D_CAMERA` (0x44 bytes:
  the projection type and parameters, near +0x14, far +0x18, position +0x20, up +0x2C, target +0x38).
  `GFL_G3D_CAMERA_Switching` (Black `sub_02048AD0`, White `sub_02048AE8`) loads one into `NNS_G3dGlb`. The field
  map's camera is FIELDMAP+0xA4 (also field_camera.c's FIELD_CAMERA+0x0C, FIELDMAP+0x10; FIELD_CAMERA_Create is
  `ov21_0218E018`, found by the `field_camera.c` heap-stamp string the decomp's ndsdisasm config names). The
  field overlay's seven Switching calls are retargeted to `PcBw_CameraSwitch`
  (`pc/patches/<VER>/arm9/overlays/21/asm/ndsrec_ov021_{000,002,008,011,014}.s.patch`). For the field camera with
  an option off its default, it loads a copy: the distance to the target x zoom / 256, the elevation lowered by
  the tilt (kept 5..85 degrees), the same heading and target, and the clip planes moved as on Platinum. Any
  other camera, and the defaults, go straight to Switching.
- **Proof:** bedroom frames at `camera_zoom=512` / `camera_tilt=160` against default (the room smaller / seen
  more from above), default byte-identical.

### Fix cartridge bugs (`NP_OPT_RULES`)

- **D/P/Pt.** Each documented bug is gated at its decision point, faithful with the bit off. Pt:
  `battle/battle_lib.c.patch` (Fire Fang's effect in the multi-turn list where Shadow Force's belongs, for
  Wonder Guard), `battle/battle_controller_player.c.patch` (Rage clears every other volatile),
  `trainer_data.c.patch` (form stats). D/P: `pc/game/pc_dp_rules.c` with `bl` retargets in
  `overlays/11/asm/ov11_02242B78.s.patch` and a `bl PcDp_RageAfterOtherMove; nop` pair replacement in
  `ov11_0223D1DC.s.patch`. A `PC_NP_RULES_CHECK` diagnostic runs the patched code with the bit off and on.
- **HG/SS: done (2026-10-09).** Of Platinum's three, HG/SS still have the Rage bug: the before-turn pass
  (`src/battle/battle_controller_player.c`, `BT_STATE_RAGE`) does `status2 &= STATUS2_RAGE` for a raging battler
  that picked another move, keeping only rage. `pc/patches/src/battle/battle_controller_player.c.patch` gates
  the fix (`&= ~STATUS2_RAGE`) on the bit, and `pc_hg_field.c`'s PC_NP_RULES_CHECK runs the pass on a built
  battle with the bit off and on: status2 0x800000 (the bug) and 0x1 (only the confusion left). Fire Fang has
  nothing to fix (HG/SS's multi-turn list, `BattleCtx_IsIdenticalToCurrentMove`, names Shadow Force), and the
  form-stats bug cannot show: no HG/SS trainer's Pokemon has a form (`files/poketool/trainer/trainers.json`).
- **B/W: done for the one documented bug at a single decision point** (`pc/src/pc_bw_rules.c`). Bulbapedia's Gen 5
  battle glitches: the 0 damage glitch is a clamp in the wrong place. Overlay 93's damage routine (Black
  `ov93_021C1E74`, White `ov93_021C1E94`) raises 0 to 1 at 0x021C1FEC, then multiplies by the "other" modifier
  (handler event 0x47, work 0x35) with the fx multiply that rounds half down (Black `ov93_021D7B10`, White
  `ov93_021D7B30`), so 1 x 1/2 is 0. That call goes to `PcBw_DamageOther`
  (`pc/patches/<VER>/arm9/overlays/93/asm/ndsrec_ov093_002.s.patch`), which with the bit on keeps it at 1, as the
  game's own `ov93_021D7B2C` does. The others (Sky Drop, the Choice lock under Klutz / Embargo / Magic Room,
  confusion and pinch Berries, Trick Room's speed wrap, Shed Shell) are state across turns or handler order, not
  one decision point, and stay as the cartridge has them. `PC_NP_RULES_CHECK` (parity.sh `battle`) runs the hook
  on the real multiply with the bit off and on.

### Mods and custom carts

- **D/P/Pt.** The packages, load order and claim tables are shared (`games/platinum/pc/src/pc_modfs.c`,
  `pc/include/pc_modfs.h`). The game half intercepts whole files and NARC members: D/P `pc/game/pc_dp_modfs.c`
  (a strong `FS_OpenFile` over the weak SDK one serves claimed files from a memory archive) and
  `pc/patches/arm9/src/filesystem.c.patch` (NARC member stat / read / object paths). Pt adds cooked formats
  (maps, billboards). Carts pin a package set to a slot.
- **HG/SS: B/W's ROM view, linked (2026-10-09).** HG/SS's NitroSDK 4.2 FS finds its files through the same
  FAT, read off the cartridge through `pc_card_rom.c`, and the 128 MB cartridge has ~7 MB of padding after the
  used area, so `games/ndsrec/pc/src/pc_bw_romview.c` serves HG/SS's packages unchanged (its TWL-only limit write
  now only on a DSi-enhanced header). `games/heartgold/pc/Makefile.wasm` compiles it from its path with
  `-DPC_BW_ROMVIEW`. `tests/bwhgss/hgss_mod_example.py` makes the example package from the player's ROM (the
  main menu's text bank, a/0/2/7 member 442: CONTINUE (MOD)); parity.sh checks it. Still to prove on a core
  with the link.
- **B/W: done as a ROM view** (`games/ndsrec/pc/src/pc_bw_romview.c`, guest side, B/W builds only:
  `pc_card_rom.c`'s `rom_read` calls it under `PC_BW_ROMVIEW`, so D/P/Pt compile the same code as before). On the
  first cartridge read it walks the ROM's FNT, asks pc_modfs (packages, load order, claims: shared) for each
  file and NARC member, and moves every claimed whole file and every NARC with claimed members (rebuilt, its BTAF
  rewritten, appends included) past the image's used area (header 0x80 / the TWL total at 0x210), patching their
  FAT entries in the reads. TWL-SDK's FS refuses ROM-archive reads at or past the TWL-only area in DS mode
  (`sub_0207A980`: offset >= header 0x92 << 19), so the in-memory header's 0x92 moves past the view. Nothing
  claimed: no view, every read the cartridge's. The example (`tests/bwhgss/bw_mod_example.py`) is made from the
  player's ROM: `narc/a/0/0/2/179`, the main menu's bank with CONTINUE -> CONTINUE (MOD), and its empty-input
  `.cooked/digest`. Proven headless (parity.sh `mods`) and in the app's Mods page and carts (`n2_mods`,
  `n2_carts`).
- **Any DS game: a ROM-level view.** The sketch B/W's view follows: B/W has no FS symbols placed, and its SDK is TWL-SDK 5. Instead
  of hooking the FS, serve the mod at the ROM read. Every cartridge read goes through `CARDi_ReadRom` ->
  `np_host_rom_read` (`pc/src/pc_card_rom.c`). The SDK finds files through the FAT it reads from the ROM. A host
  ROM view that (1) appends each claimed whole file past the ROM's end and rewrites its FAT entry, and (2)
  rebuilds a claimed NARC with the replaced members appended, works with no game hook. This is the DS
  counterpart of the GBA's `.ips` at boot (`gba_mods.c`). It lives where `rom_read` is answered (the shell's /
  np_headless's host callback), so guests are untouched and D/P/Pt never see it unless a package is selected.
  It still **touches shared host code**, so run the D/P/Pt shell tests. Risks: integrity checks (HG's
  `ds_protect` checks overlay code, not data, but must be confirmed; B/W's own checks are in overlay 230's
  detectors, docs/BW_PLAN.md), and the per-file FNT/FAT parse. Carts reuse the shell's existing pinning.
- **Proof:** an example package replacing one title-screen graphic or one message, checked in a frame
  (`n2_mods` case), with vanilla byte-identical when no package is selected.

### `NP_STAT_IN_BATTLE` (battle layout)

- **D/P/Pt.** `in_encounter` from encounter creation to its free (Pt `encounter.c.patch`; D/P the same in
  `pc_dp_field.c` plus its patch) and `in_battle_app` around the battle application for facility battles
  (Pt `unk_0203D1B8.c.patch`).
- **HG/SS: proven for field and Battle Frontier battles.** `in_encounter` (`pc/patches/src/encounter.c.patch`):
  on Route 29 (`tests/bwhgss/hgss-wild.sched` from New Bark's west exit) a wild Sentret sets in_battle at frame
  3329; RUN ("Got away safely!") clears it at 4650 as the field comes back. `in_battle_app`
  (`pc/patches/src/launch_application.c.patch`: `Battle_Init` / `Battle_Exit`, `gOverlayTemplate_Battle`'s init
  and exit) covers the battles that do not come from an `Encounter`: the Battle Frontier's engine launches the
  battle application itself. tests/e2e/heartgold/87-battle-frontier-tower's seven Battle Tower battles each
  raise and drop in_battle on both games. Field battles are unchanged: HG 60 (Cut), SS 60, HG 03 (the rival,
  the catching lesson) and HG 101 (24 wild battles) give the same np_gp hash over every frame's screens and
  audio before and after the patch.
- **B/W: done, no game patch.** `pc/src/pc_bw_e2e.c` `bw_frame` sets `in_battle_app` while overlay 93's battle
  POKECON exists (`bw_pokecon`, docs/BW_RAM.md "Battle"), from the battle's intro to the fade back to the field.
  Bianca's battle in the bedroom (`tests/bwhgss/bw-battle.sched`) sets it at ~9920 and clears it at ~14030
  (parity.sh `battle`); in the app with `battle_layout = hybrid` the field is drawn in the vertical layout,
  the battle in the hybrid one, and the field after it vertical again (`n2_battle_layout`).

## Ranking (value / effort)

| # | Feature | Game | Effort | Value | Blocked by | Touches D/P/Pt inputs | Status |
|---|---|---|---|---|---|---|---|
| 1 | Render scale, widescreen | B/W | none in game code: app cases + rows | high (works today) | nothing | no | **done**: n2_render |
| 2 | Volume split (player table) | B/W | small: `pc_np_seq_player_group` in ndsrec, drop `pc_dp_snd.o` | high (B/W makes music) | nothing | no (weak hook alternative: yes) | **done** |
| 3 | Instant text | HG/SS | small: Pt's `text.c.patch` ported | medium | nothing (Oak's intro) | no | **done** |
| 4 | Quick save, `IN_BATTLE` (encounters) | HG/SS | none: already in source | high | overlay 123 field load | no | **done** (proven) |
| 5 | Render scale, widescreen | HG/SS | none | medium | field (3D) | no | **done** (proven) |
| 6 | Camera zoom / tilt | HG/SS | medium: fieldmap patch + Pt's maths on HG's Camera | medium | field | no (if copied, not shared) | **done** |
| 7 | Volume split | HG/SS | none: Platinum's table fits (OPED unused) | low until HG has sound | HG audio silence | no | **done** (proven) |
| 8 | Quick save | B/W | medium: the save API hunt, then `ARMREC_CALL` and a game event | high | nothing | no | **done** |
| 9 | Instant text | B/W | medium: printer hunt, then a `bl` retarget | medium | nothing | no | **done** |
| 10 | Mods via a ROM view | B/W (done, guest side); other DS games | medium-high: FAT/NARC rebuild at the cartridge read | medium | nothing | no (B/W only, `PC_BW_ROMVIEW`) | **done** for B/W |
| 11 | Mods via FS/NARC hooks | HG/SS | medium | low over #10 | field for most assets | no |
| 12 | `in_battle_app` | HG/SS | small | low (Frontier only) | field | no | **done** (proven) |
| 13 | Camera zoom / tilt | B/W | medium: GFL camera hook on the field overlay's Switching calls | medium | nothing | no | **done** |
| 14 | `IN_BATTLE` | B/W | small: the probe's POKECON | medium | nothing | no | **done** (app) |
| 15 | Rules | HG/SS, B/W | medium each, per documented bug | low | battles | no |

The same B/W groundwork serves #8, #9, #13 and #14: the per-version emitted-assembly patch step in
`pc/mk/ndsrec.mk` (in place since 2026-10-08, the startup proposal its first patch).
