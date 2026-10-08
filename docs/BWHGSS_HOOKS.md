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
| patch asm | `*.s.patch`: `bl X` -> `bl PcDp_Hook`, or an instruction pair -> `bl Hook; nop`, size for size (`overlays/05/asm/ov05_021D74E0.s.patch`, `overlays/11/asm/ov11_0223D1DC.s.patch`) | n/a | `pc/patches/asm`, `lib/asm` `.s.patch` (`pc/mk/armrec.mk`) | `patch_bw_startup.py`: hash-guarded, size-neutral edits of one emitted file; nothing general yet |
| replace a function | strong C symbol over a weak SDK body (`FS_OpenFile`, `pc_dp_modfs.c`) | C | `pc/host_overrides.txt` | `tools/ndsrec/primitives.txt` `override` (signature-matched from Diamond: 71 of 136 placed, docs/BW_PLAN.md) |
| call game code | direct C call | direct C call | direct C call | `ARMREC_CALL` / `armrec_call_code(addr, r0..r3)` on a host-owned guest stack |
| read game state | C structs | C structs | C structs | RAM map, docs/BW_RAM.md (GAMESYS `*0x02146248` Black / `*0x02146268` White) |

For B/W the D/P `.s.patch` is the model: the emitted assembly is deterministic per ROM, so a per-version patch
under `games/ndsrec/pc/patches/<version>/` applied by `pc/mk/ndsrec.mk` after `ndsrec.py emit` (and guarded
by the emitted file's hash, as `patch_bw_startup.py` is) can retarget a `bl` to host C. `patch_bw_startup.py`
should become the first such patch instead of a second mechanism.

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
- **HG/SS: the same path, not yet visible.** The intro frame grows to 512x768 and 342x384
  (`heartgold-intro-widescreen.png`), but there is no 3D on screen before the field. Expect it to work as on
  B/W once the field loads. Watch for one HG-specific case: `ov01_021E6220` (`src/field/fieldmap.c`) adds a
  depth offset to `NNS_G3dGlb.projMtx` after `Camera_PushLookAtToNNSGlb`. It edits `_32` only, so the wide clip
  X should survive. Check this when the field runs.
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
- **Plan, both games, without touching D/P/Pt inputs:** let the game answer "is this archive player music". B/W:
  drop `pc_dp_snd.o` from the ndsrec link (as HG/SS already drops `pc_agb_slot.o` and `pc_dp_agb.o`) and give
  `pc_bw_e2e.c` (or a `pc_bw_snd.c`) a `pc_np_seq_player_group` that reads the same two arrays and folds
  B/W's numbers onto Platinum's classes (0, 6 -> 7; 1..5 -> 3). HG/SS: the same, folding 8 -> 7. Because
  `pc_np_options.c` only ever sees Platinum's numbering, D/P/Pt stay byte-identical. The cleaner alternative is
  a weak `pc_np_player_is_music(int)` in `pc_np_options.c`, but that changes a D/P/Pt input and **needs a D/P/Pt
  rebuild**.
- **Proof:** Black's title rms with `bgm_volume=0` drops to near 0, and with `se_volume=0` stays about 7470
  (np_headless `-o`, then the app's n2_audio case with a `bgm0` assertion). HG/SS cannot be proven until its
  core makes sound (it is silent through the title and intro, docs/FEATURE_PARITY.md).

### F1 quick save

- **D/P/Pt.** `pc_np_frame` (Pt `pc/src/pc_np_field.c`; D/P `pc/game/pc_dp_field.c` `np_frame`) waits up to
  60 frames (`QUICKSAVE_PATIENCE`) for `field_ready`, applies the start menu's refusals, and calls the game's
  own field save (`FieldSystem_Save` / `Field_SaveGame`) on a host guest stack (`pc_dp_on_guest_stack`). The
  result is saved / refused / failed.
- **HG/SS: done in source, unreachable.** `pc_hg_field.c` `np_frame`, `field_ready`, `save_allowed` and
  `quicksave_done` mirror D/P with `Field_SaveGameNormal` (overlay 1, the same call `TouchSaveApp_SaveGame` and
  `ScrCmd_SaveGameNormal` make). It needs nothing more than the field: the overlay 123 `ds_protect` fix.
  Then the app's n2_quicksave case should see "Saved" and the slot file change.
- **B/W: needs the save routine.** The B/W gate already exists (`bw_frame`: `field_ready`). Missing is the
  entry point the X menu's SAVE runs. Find it the way docs/BW_RAM.md found the terrain query. Set an LLDB
  write watchpoint on the save footer's count (Black `0x0221BBAC + 0x23F8C`, White `+0x20`) during the parity
  schedule's X-menu save (`tests/bwhgss/bw-save.sched`). The backtrace gives the writer and the chain up to
  the menu's save event. The heap allocator's file stamps (`game_data.c`, and the save system's own file)
  name the blocks along the way. Two shapes are possible:
  1. a synchronous "save the game" function (D/P's shape): `ARMREC_CALL` it from `bw_frame` when ready;
  2. an asynchronous save event (likely for Gen 5's two-slot 0x24000-byte writes): start the same `GMEVENT`
     the menu starts. That needs the event-push function (its address appears in the menu's backtrace) called
     with GAMESYS. Report the result when the event pointer (GAMESYS+0x18) returns to 0 and the footer count
     has advanced.

  Refusals: the X menu's own conditions. Read them from the menu's list builder, found from the same
  backtrace (expected, not yet seen: link rooms, Entralink, the Battle Subway, the Musical). Until those
  are known, refuse outside the zones the e2e milestones have saved in. Never write the save image directly:
  that bypasses the game's CRCs and slot rotation.
- **Proof:** np_headless with `-o quicksave_seq=1` at a bedroom frame, then `np_save5 verify` and the footer
  count +1; the app's n2_quicksave "Saved".

### Instant text

- **D/P/Pt.** Pt `pc/patches/src/text.c.patch` wraps `RenderFont`'s result in `PcNp_RushPrinter`: while the
  option is on and the printer is only handing out characters (`RENDER_PRINT` / delay), it keeps rendering
  within the frame. It stops at waits, scrolls, pauses and the finish, so input and callbacks are unchanged.
  D/P: the same rule on D's printer (`pc/patches/arm9/src/text.c.patch`).
- **HG/SS: a direct port.** HG's `src/text.c` has the same `RunTextPrinter` / `RenderFont` pair (lines 201 and
  229) as Platinum's. Apply Pt's patch as `games/heartgold/pc/patches/src/text.c.patch` with HG's enum names.
  This can be **tested now**: Prof. Oak's introduction is printed text before the field
  (`tests/bwhgss/hgss-intro.sched`). Compare frames with `-o text_instant=1` and without: the first page
  completes in the first frame of the box.
- **B/W: find the printer.** Gen 5's printer is not mapped. Find it with a write watchpoint on the message
  window's character buffer (or on VRAM of the window's BG) while the bedroom's first dialogue prints. The
  callee that advances the string pointer by one character per wait is the target. Then either (a) a `bl`
  retarget in its caller to a host `PcBw_RushPrinter` that loops the original while it returns "printed a
  character" (Pt's rule, calling the original through `ARMREC_CALL`), or (b) if the per-character wait is a
  counter in the printer object, zero it from `bw_frame` while the option is on. (a) is the faithful one; (b)
  is simpler but also skips waits the game meant (scroll timing). Prefer (a).
- **Proof:** the bedroom's opening dialogue (parity `bw-save.sched`) with `-o text_instant=1`: the box's full
  page at its first frame.

### Camera zoom and tilt

- **D/P/Pt.** Around the field's 3D draw, never touching the game's camera. Pt: `fieldmap.c.patch` calls
  `pc_np_camera_begin(fieldSystem->camera)` after `Camera_ComputeViewMatrix` and `pc_np_camera_end` after
  `G3_RequestSwapBuffers`. `pc_np_field.c` `pc_np_camera_begin` copies the camera, scales the distance by
  `zoom/256`, adds the tilt to the pitch, adjusts near/far, and loads `NNS_G3dGlbLookAt` / the projection;
  `end` restores. D/P: `ov05_021D74E0.s.patch` retargets `bl Camera_PushLookAtToNNSGlb` -> `bl
  PcDp_FieldPushLookAt` and `bl sub_020222B4` (the swap) -> `bl PcDp_FieldSwapBuffers` (`pc_dp_field.c`).
- **HG/SS.** `src/field/fieldmap.c` `ov01_021E6220` is the field draw: `Camera_PushLookAtToNNSGlb()` then the
  map, props and the projection depth tweak. Patch it like Pt (`pc/patches/src/field/fieldmap.c.patch`). Put
  `pc_np_camera_begin/end` in `pc_hg_field.c`, porting Pt's maths onto HG's `Camera` (`src/camera.c`:
  distance, `CameraAngle`, `Camera_ApplyPerspectiveType`). Copy rather than share Pt's function: moving it into
  a shared file **would change a D/P/Pt input**. Two HG specifics: the depth tweak after the push must see the
  hook's projection (so call `begin` instead of the push, as D does, before the tweak), and the cutscene / photo
  cameras (`field_take_photo.c`, title, starter app) stay untouched. Testing needs the field.
- **B/W: the most unknown.** No camera structure is in docs/BW_RAM.md. Gen 5's field camera is likely its own
  library (with its own heap-stamped file name). Method: find the field draw's look-at load the way D/P's was
  found. Signature-match `NNS_G3dGlbLookAt` (add it to `tools/ndsrec/primitives.txt` as `ref`). If B/W does not
  call it, watch writes to `NNS_G3dGlb`'s camera matrix during the bedroom's draw, then find its caller in the
  field overlay. Then retarget that `bl` to a host function that rebuilds the look-at from the camera's
  position and target (read from the same arguments) with zoom/tilt applied, and restore after the swap (a
  second `bl` retarget). Size: like D's two-line patch once found; finding it is the cost.
- **Proof:** bedroom frames at `camera_zoom=512` / `camera_tilt=160` against default (the room smaller / seen
  more from above), default byte-identical.

### Fix cartridge bugs (`NP_OPT_RULES`)

- **D/P/Pt.** Each documented bug is gated at its decision point, faithful with the bit off. Pt:
  `battle/battle_lib.c.patch` (Fire Fang's effect in the multi-turn list where Shadow Force's belongs, for
  Wonder Guard), `battle/battle_controller_player.c.patch` (Rage clears every other volatile),
  `trainer_data.c.patch` (form stats). D/P: `pc/game/pc_dp_rules.c` with `bl` retargets in
  `overlays/11/asm/ov11_02242B78.s.patch` and a `bl PcDp_RageAfterOtherMove; nop` pair replacement in
  `ov11_0223D1DC.s.patch`. A `PC_NP_RULES_CHECK` diagnostic runs the patched code with the bit off and on.
- **HG/SS.** HG's battle engine descends from Platinum's, so check each Pt fix against `src/battle/` and the
  pokeheartgold bug documentation. Apply only the bugs HG still has, each as a `.c.patch` guard reading
  `pc_np_opt.rules`, plus a `pc_hg_rules.c` check modelled on `pc_np_field.c`'s. Testing needs a battle, so
  the field first.
- **B/W.** No B/W battle runs (overlay 93), and there is no Gen 5 bug list here yet. Once battles run, list the
  documented Gen 5 cartridge bugs, then gate each at its function with the D/P asm technique. Last in order.

### Mods and custom carts

- **D/P/Pt.** The packages, load order and claim tables are shared (`games/platinum/pc/src/pc_modfs.c`,
  `pc/include/pc_modfs.h`). The game half intercepts whole files and NARC members: D/P `pc/game/pc_dp_modfs.c`
  (a strong `FS_OpenFile` over the weak SDK one serves claimed files from a memory archive) and
  `pc/patches/arm9/src/filesystem.c.patch` (NARC member stat / read / object paths). Pt adds cooked formats
  (maps, billboards). Carts pin a package set to a slot.
- **HG/SS.** The same split: HG's FS is NitroSDK 4.2 assembly and its NARC layer is C (`NARC_New`,
  `NARC_ReadWholeMember`, `NARC_AllocAndReadWholeMember`). Replace `FS_OpenFile` through `pc/host_overrides.txt`
  with a 4.2 `FSFile` version of D's memory-archive opener. Add a `.c.patch` on HG's NARC functions calling
  `pc_modfs_member_*`, binding each NARC to its path at open (`pc_modfs_bind_narc`). Medium effort, and only
  title-screen assets can be checked before the field.
- **B/W, and any DS game: a ROM-level view.** B/W has no FS symbols placed, and its SDK is TWL-SDK 5. Instead
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
- **HG/SS.** `in_encounter` is already in (`pc/patches/src/encounter.c.patch`). Missing: `in_battle_app` for
  battles that do not come from an `Encounter` (the Battle Frontier). Patch `Battle_LaunchApp` /
  `gOverlayTemplate_Battle`'s init and exit (`src/launch_application.c`) to set and clear it. Testing needs the
  field and a battle.
- **B/W.** No game patch: GAMESYS+0x18 is the running `GMEVENT`. Once the battle-call event's function address
  is known (the first battle's event, readable when it starts), `bw_frame` can compare the event's function
  field against it. The event's layout is to be established; the pointer alone is also set for menus and
  dialogue. Before the battle overlays run, report 0. This needs the overlay 93 fix (docs/BW_PLAN.md: the
  bx-pc veneer at `ov93_021BB9D8`).

## Ranking (value / effort)

| # | Feature | Game | Effort | Value | Blocked by | Touches D/P/Pt inputs |
|---|---|---|---|---|---|---|
| 1 | Render scale, widescreen | B/W | none in game code: app cases + rows | high (works today) | nothing | no |
| 2 | Volume split (player table) | B/W | small: `pc_np_seq_player_group` in ndsrec, drop `pc_dp_snd.o` | high (B/W makes music) | nothing | no (weak hook alternative: yes) |
| 3 | Instant text | HG/SS | small: Pt's `text.c.patch` ported | medium | nothing (Oak's intro) | no |
| 4 | Quick save, `IN_BATTLE` (encounters) | HG/SS | none: already in source | high | overlay 123 field load | no |
| 5 | Render scale, widescreen | HG/SS | none | medium | field (3D) | no |
| 6 | Camera zoom / tilt | HG/SS | medium: fieldmap patch + Pt's maths on HG's Camera | medium | field | no (if copied, not shared) |
| 7 | Volume split | HG/SS | small (fold 8 -> 7) | low until HG has sound | HG audio silence | no |
| 8 | Quick save | B/W | medium: watchpoint hunt, then `ARMREC_CALL` or event push | high | nothing | no |
| 9 | Instant text | B/W | medium: printer hunt, then a `bl` retarget | medium | nothing | no |
| 10 | Mods via a ROM view | all DS | medium-high: FAT/NARC rebuild in the host's ROM read | medium | nothing | shared host code: shell tests |
| 11 | Mods via FS/NARC hooks | HG/SS | medium | low over #10 | field for most assets | no |
| 12 | `in_battle_app` | HG/SS | small | low (Frontier only) | field | no |
| 13 | Camera zoom / tilt | B/W | high: no camera map yet | medium | nothing | no |
| 14 | `IN_BATTLE` | B/W | small after the event layout | medium | overlay 93 | no |
| 15 | Rules | HG/SS, B/W | medium each, per documented bug | low | battles | no |

The same B/W groundwork serves #8, #9, #13 and #14: a general per-version emitted-assembly patch step in
`pc/mk/ndsrec.mk`, with `patch_bw_startup.py` folded into it. Build it once before the first of them.
