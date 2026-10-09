# HeartGold/SoulSilver: handoff

The HG/SS port and shared DSProt replacement are integrated into the active
development tree, starting from `hgss` commit `7a4b6eb71`.
That source branch was rebased onto main `843e58045`; its pokeheartgold
subtree merge was re-made with `git merge -s ours` plus `read-tree --prefix`
because a plain rebase flattens it.

## Experimental DSProt replacement and remaining blocker

HG/SS call DSProt (`lib/dsprot`, overlay `ds_protect`) on every field map
load (`overlay_124.c` field init, `fieldmap.c`) and in the Pokédex,
touch-save app and overlay 27. Its tests read the card's secure area and
CRCs and the console's MAC address and owner data. A positive answer makes
the game sabotage itself (heap offsets/allocations).

This experiment replaces the assistant-added unconditional stop in
`pc/src/pc_hg_dsprot.c` with fixed results for all six exported detectors.
The three positive-name detectors return false; their `DetectNot*`
counterparts return true. Non-null callbacks run only for true results.
This substitutes outcomes rather than executing the original tests.
The same source is compiled for HeartGold and SoulSilver.

**Resolved (hgss2):** `ds_protect`'s five static initialisers are the
`NitroStaticInit` of `lib/dsprot/build/*_decoder.s` (dsprot_main_decrypter,
integrity_decrypter, encryptor, coretests_decrypter, rc4). Each only hands
its table of encoded function bodies to `Encryptor_DecodeFunctionTable`,
which rewrites those instruction words in place in the overlay's RAM image.
The port runs `lib/dsprot/src`'s C in the clear and answers the detector
entry points itself, so `pc_hg_dsprot.c` records the five as superseded with
`pc_dp_sinit_record(FS_OVERLAY_ID(ds_protect), ...)` (overlay 123 only; the
loader's exact count still stops on a sixth entry). Verified: HeartGold
headless frame 16170 loads overlays 124 and 123, frame 16176 overlays 1,
123, 2, 3 and frame 16180 overlay 27 (`PC_TRACE_OVERLAYS=1`).

## Current state (hgss-play2, 2026-10-09): New Bark Town, save, CONTINUE

Fixed on main: armrec msl.s `.type @function` routines (fe357c02a), the
cartridge IR chip (53b112d0c), the C OS_GetTick polling the timer model
(13d513cd6), and compiled C's divider / geometry-port / GXFIFO registers
routed through the armrec runtime (97814f2c5:
`pc/include/nitro/hw/ARM9/io_reg.h`; HG/SS's monolithic io_reg.h never
included Platinum's ioreg_CP/G3/G3X shadows, so `G3_SwapBuffers` never
reached pc_gpu3d and the top screen was black). HeartGold frame 16600 of
the intro schedule shows the player's bedroom (map_id 64) in 3D.

D/P/Pt gate for fe357c02a, 53b112d0c and 13d513cd6: the shared gate cores
were rebuilt at main 6ebed6312 and all 11 `tests/dp/regress.sh` cases
(D/P/Pt and R/S/E) matched their expected hashes (build-infra's
`tools/gate.sh --refresh-cores`). Before that: Diamond's and Pearl's armrec
output (576 files each) was byte-identical between the old and new
armrec.py, and the armrec bridge test passes 48/48.

Since then (HeartGold, inspected frames under /tmp/hgss2):
- Player sprite (165d72148): card reads take time (`PC_CARD_READ_TIME`,
  HG/SS only), so FieldMap_Init's VBlank-queue tasks drain and the
  overworld sprite textures upload. Ethan in the bedroom: hgD_016800.png.
- Stairs 2F -> 1F (map 64 -> 63), Mom's scene, the door to New Bark
  (map 60 at frame 20344) and Lyra/Marill's scene: f1sheet.png,
  hgF1_022000.png. The door warp trapped in wasm until 9d997a419 (an
  unprototyped 4-argument callback call in ov01_022044B0).
- In-game save from the touch menu ("Shae saved the game.", s1full.png) and
  fresh-process CONTINUE back to New Bark (ksheet.png; the main menu also
  lists CONNECT TO POKeWALKER, the IR chip answering).
- Naming screen: no SIGBUS any more (the sentinel-lr leak was the msl goto
  bug); typing ABC and OK reaches Oak's confirmation (nm2sheet.png).
- hgss-e2e's milestone 01 (bedroom -> Elm -> Cyndaquil -> Pokegear -> west
  exit, quick save) passes on HeartGold.
- First battle (Route 29 grass): aborted in pc-gpu3d (MTX_LOAD_4x4 short of
  parameters) because recompiled GX_SendFifo64B had no geometry hook; fixed
  by making it a host override (2638875c6). Verified: CONTINUE from hgss-e2e's
  west-exit save, Route 29 grass, wild Sentret vs Cyndaquil at the command
  menu (btsheet.png).

Since then (hgss-play3, 2026-10-09; frames under /tmp/hgss2):
- Route 30 trainers (Youngster Joey froze the field, sprites drew as
  untextured blocks after some battles). Two defects. (1) Every field load's
  FieldEffectManager_InitRenderers creates 56 one-shot VBlank tasks
  (sub_02069714) in one logical frame against a 32-entry queue; only the
  card time model delivers VBlanks inside a load, and counting bytes (0x8000
  a frame) let 33-35 tasks fall between two VBlanks after some battles. The
  refused one that mattered was ov01_021FA6E0, the map objects' texture
  loader, so nothing loaded after it got a texture. pc_card_rom.c now charges
  0x200-byte pages with the SDK's one-page cache, at the gap and clock of the
  cartridge header's ROMCTRL (about 52 pages a VBlank); the worst stretch is
  16 tasks. (2) ScrCmd_TrainerStepTowardsPlayer calls sub_02064598() with no
  argument where the assembly reads the approach task from r0 (on ARM still
  there from the test before it); through the bridge r0 was 0, the assert
  fired every frame and the approach never ended. It and five more such calls
  (ov35_02259DB8, sub_0202D3DC, sub_02005448, sub_0200F478, sub_02031188;
  found by comparing every C call site's argument words in the bridge's .sigs
  `W` records with the r0-r3 the assembly reads) pass the argument now
  (pc/patches/src). Verified: Joey's "Wait! You look weak!", his Rattata vs
  Cyndaquil, all sprites textured (jm2/s.png).
- In-battle bag/party SIGBUS: the battle controller's three calls to the
  sub-menu (overlay 8, loaded by ov12_02237B0C just before) are spelled
  `bl ov10_0221BE20` in pret's assembly, the trainer AI at the same address;
  armrec bound the name. The class is fixed in armrec: with
  `--overlay-dispatch` (HG/SS and D/P pass it) a call into another overlay
  at an address other overlays can occupy is an armrec_dispatch() of the
  address, resolved by residency as the hardware resolves it, and decompiled
  C reached that way is registered at its guest address under its overlay.
  HG/SS: 3512 such calls (438 into C); D and P: 1910 each, every one checked
  mechanically to dispatch the named function's own address, and the D/P/Pt
  regression hashes are unchanged. Verified: Potion, USE, target screen,
  "It won't have any effect." (bag/bag3.png).
- call_indirect type mismatches: every C indirect call site's reachable
  callees (an -O0 IR field-based flow over all game TUs) against their wasm
  types; two real ones patched (communication_club.c's task callbacks,
  custom_safari_zone.c's comm callback); the rest unreachable (a DSProt
  positive answer, the OS IRQ path the port does not take).
- Music: every BGM was silent (title, field; only some sound effects were
  heard). SND_WORK, which holds the sound heap, was a wasm C static at
  0x0B1xxxxx, and SOUNDxSAD's 27 bits turned each wave address into
  0x031xxxxx. It moves into the port window (pc/patches/src/sound.c.patch),
  as D/P's SoundData and Platinum's SoundSystem do. CONTINUE runs now
  measure rms ~3400 (they measured 0).
- With these, hgss-e2e's chain 04-11 (Route 30 to Goldenrod, Falkner,
  Bugsy) passes on this core (/tmp/hgss2/e2e).

The make 3.81
build cost did not reproduce: on a 437-object rebuild make itself used 2.4 s
of CPU (GNU make 4.4.1: 1.5 s), so make is not where a guest rebuild's time
goes. Checkpoint runs: np_headless --fork-at without --lockstep
(/tmp/hgss2/forkrun.sh).

## Previous blocker (hgss2, 2026-10-08): field init hangs in FieldMap_Init

Same schedule as below, HeartGold, native core built from hgss2:
`FieldMap_Init -> ov01_021E662C (fieldmap.c:774) -> CARD_SpiWaitGetStatus`
(twice) spins forever in the second call's `OS_LockCard` (sampled; 0% guest
progress after frame 16180). LLDB watchpoint on the card lock word
(guest 0x027FFFE0): the first call writes lockFlag=0xFFFD, ownerID=0xFFFD,
and nothing writes it again before the second call; no `armrec_dispatch` to
`OS_UnlockCard` happens in between. Root cause, read in the generated
`build/pc-wasm/heartgold/armrec/c/nitro.c`: `bl _ll_udiv` in
`CARD_SpiWaitGetStatus` is emitted as `goto L__ll_udiv;`, because the
msl.s helper bodies (`_fadd`, `_ll_udiv`/`_ull_div`, `_ll_shl`, ...) appear
as local labels at the end of the calling function (`emit_branch` in
armrec.py: a target in `func.labels` and not in `func.entries` becomes a
goto). The helper's `bx lr` then returns from the whole caller, so the
first call returns before `OS_UnLockCard`. The same `goto L__ll_udiv`
pattern occurs in 18 generated files (nitro.c 7, overlay_00_arm.c 33,
nnsys.c 4, overlay_39_arm.c 6, overlay_40.c 5, MSL_*, wifi.c, ...), so it
breaks every recompiled 64-bit division caller, not only this check.
Suspects: armrec `33a712360` (bodiless function starts become C aliases of
the function that follows) or the HG staging of msl.s; find why msl.s's
`.type X, @function` labels land in a nitro.s function's label set, fix it
in armrec, then gate D/P/Pt (`tests/dp/regress.sh --only` the cases whose
armrec output changes) before integrating.

Next, after that fix: the same function needs the cartridge's IR chip on
the ARM9's AUXSPI bus (0x040001A0/A2): command 0x08 must answer 0xAA, or
`ov01_021E662C` returns FALSE and fieldmap.c queues the anti-piracy
`Task_AntipiracyMath` tasks. Planned model (agreed with BWPlay1, who wants a
ping when it lands): `games/platinum/pc/src/pc_card_rom.c`
`armrec_card_store/load`, CS held by AUXSPICNT bit 6, first byte the IR
command (0x08 -> 0xAA, 0x00 pass-through to the flash, 0x01/0x02 nothing in
range), enabled when the header game code starts with 'I' (HG/SS, B/W).

Naming-screen SIGBUS (task pointer 0xFFFFFFF0): 0xFFFFFFF0 is armrec's
`ARMREC_LR_SENTINEL`, the `lr` value recompiled code starts with when C
calls it, so a sentinel `lr` reached guest memory and became a SysTask
link; not yet traced to the storing instruction. naming_screen.c's
`SysTask_NamingScreen_WiggleEffect` also has a documented use-after-free
(`++data->state` after `DestroySysTaskAndEnvironment`) to rule in or out.

Tooling for the next session:
- LLDB cannot open ROMs under ~/Documents (TCC): debug with the APFS clones
  `/tmp/nativeplat-proposal-runtime/hgss/poke{heartgold,soulsilver}.debug.nds`.
- `breakpoint command add -o` keeps only the last `-o`; drive LLDB from a
  Python module instead (`/tmp/hgss2/lockdrv.py`: breakpoint, then a
  watchpoint at memory base + guest address, base = `*(u64 *)(x0 + 0x18)`
  in any `w2c_heartgold_*` frame). Small recompiled callees are inlined
  natively, so name breakpoints miss them; watchpoints do not.
- `/tmp/hgss2/np_headless-fork-at.patch` (unbuilt) lets `--fork-at
  F:CTL` work without `--lockstep`: run once to a frame, then fork one child
  per control-file line, so experiments skip the 16000-frame intro.
- Long jobs: start detached (`/tmp/hgss2/detach.sh LOG cmd...`); a tool
  call's own deadline kills an attached `tools/heavy.sh` waiting for a slot.

## Current state (supersedes "Where it stopped" below)

- Both guest wasm builds and the shared native executable passed. Each
  guest's `check_module` result was `ok`.
- The ROM SHA-1s match: HeartGold
  `4fcded0e2713dc03929845de631d0932ea2b5a37`, SoulSilver
  `f8dc38ea20c17541a43b58c5e6d18c1732c7e582`.
- Both games independently pass the touch tutorial, Oak's introduction,
  boy selection and default-name confirmation (`Chase`). The last
  content frame is the boy's shrinking silhouette at frame 16101; the
  first field-load attempt stops at headless frame 16169, exit 1.
- LLDB confirms `Field_NewGame_AppInit -> FieldSystem_New ->
  FieldSystem_Init -> FS_LoadOverlay -> FS_StartOverlay ->
  pc_dp_overlay_sinit -> fatal`. All six detector export breakpoints
  and their wrappers have zero hits. Thus the substituted results and
  callback behavior are still **not verified in-game**.
- Overworld entry, map transitions, in-game saving and fresh-process
  Continue reload are **blocked**, not passed. No save was produced.
- Separate defect: leaving the naming screen open until frame 14663
  produces SIGBUS in `SysTaskQueue_RunTasks+160`, with guest task pointer
  `0xfffffff0`. Corruption origin is unproven. The game's supported
  empty-name/default-name acceptance, used promptly, gets past this
  screen without code or state modifications.
- Verification used the original proposed source unchanged; the
  subsequently committed first-comment correction has identical
  preprocessed C. No later armrec fixes or other port repairs were added.
- These runtime findings predate integration with the latest main. No
  additional test runs were performed for publication, at the user's request.
  This is not a release-ready or playable-port claim.
- armrec changes this round: the `cc -E -x assembler-with-cpp`
  preprocessor (checked: Diamond's and Pearl's 575 inputs each preprocess
  token-identically to the old cpp), file-local `.macro` expansion,
  `.balign 4` at func-start macros, `.space` from `.`, counter addresses
  for local data labels, bodiless function starts emitted as C aliases
  (`__attribute__((alias))`; msl.s `_fadd`/`_f_add`, `_dadd`/`_d_add`).

### Reproducing the first field-load failure

Build each guest with `make -C games/heartgold -f pc/Makefile.wasm -j4
GAME_VERSION=HEARTGOLD` or `SOULSILVER`, through `tools/heavy.sh`. Configure
the native core with both `NP_GUEST_WASM_heartgold` and
`NP_GUEST_WASM_soulsilver`, pointing to the corresponding
`games/heartgold/build/pc-wasm/poke<game>.wasm`, and
`NP_GUEST_POSTPROCESS=$PWD/tools/wasm2c_postprocess.py`.

Save the following as a schedule outside the source tree:

```text
1300:start
1700:start
2100:a:4:40:10
3100:tap:220:170:4:80:30
6100:tap:128:72:4
6500:a:4
6800:tap:128:147:4
7200:tap:220:170:4:80:50
11500:tap:70:140:4
11900:tap:220:180:4:80:3
12500:tap:60:80:4
12800:tap:220:180:4
13200:tap:190:55:4
13500:tap:220:180:4:80:3
14000:start:4
14100:a:4
14500:tap:220:180:4
14800:tap:190:55:4
15100:tap:220:180:4:80:20
```

For each game, run the native `np_headless` with its corresponding ROM,
`--frames 17000 --schedule <schedule> --save <fresh-save-path>
--dump <output> --dump-from 16150 --dump-every 1 --progress 100
-e PC_TRACE_OVERLAYS=1`, through `tools/heavy.sh --run`.
Use separate save/output paths and a fresh process per title.

The verification record is `/tmp/nativeplat-proposal-runtime/hgss/report.json`
on the test workstation, alongside exact commands, exit codes, screenshots,
debugger traces and build configuration. Those local artifacts are not
required build inputs and are not bundled as ROMs or binaries in Git.

Tested SHA-256 identities:

| Artifact | SHA-256 |
|---|---|
| Original proposed C, before comment-only publication edit | `d3a7d6679b551e9499ca103071770c83af3f3d9c395e258414a8299b8a575f6d` |
| HeartGold wasm | `22da9746f74c8b24b87d7f711937deb3a33cb8054157b992753805f6167411d6` |
| SoulSilver wasm | `71d1bf782ce597ac8b6554cf12d9dc1369c82377a3f9efa6f634e983b469c775` |
| Shared native executable | `2cc664a47d1dd42ee337083da0a5784bffea726d3c99eb6fad5e4f1db8d43a24` |

## Done

- **tools/ntr reproduces Diamond and Pearl** bit-exactly from fresh objects
  (arm9/arm7 build dirs moved aside, every `.lz` deleted first):
  `a46233d8...` and `99083bf1...`.
- **HeartGold ROM matches** `4fcded0e2713dc03929845de631d0932ea2b5a37`
  (filesystem, main, ichneumon_sub and rom sha1 checks all pass). Two fixes:
  - `tools/ntr/specfiles/mwldarm.response.template`: each overlay opens
    with `-og <OVERLAY.GROUP>,0 -ol <OVERLAY.NAME>`, as the SDK's own
    template does. Without it mwldarm stops on `_0223F900`, which overlays
    13 and 70 both define (`.public` in their `.inc` makes a local label
    global; every mwasmarm version does this).
  - `tools/ntr/setup.sh heartgold`: the ARM7 template's
    `SDK_SUBPRIV_ARENA_LO` keeps only `SDK_AUTOLOAD.MAIN.BSS_END`
    (Diamond's adds an `EXT` autoload HG/SS call `EXT_WRAM`, whose size
    pokeheartgold's common.mk appends itself).
- **SoulSilver**: `tools/heavy.sh tools/rom_build.sh soulsilver` was still
  compiling when this was written (log `/tmp/hgss/rom-soulsilver.log`).
  Target `f8dc38ea20c17541a43b58c5e6d18c1732c7e582`.
- **Game ids**: `NP_GAME_HEARTGOLD = 5`, `NP_GAME_SOULSILVER = 6` in
  core (np_core.h, contract-v2, NpGuestModule.cmake registry, CMake guest
  list, np_core.c, stub), np_headless, np_gp, shell (launch, options,
  romdb with both No-Intro SHA-1s, titles, autotest header codes
  IPKE/IPGE, colours, slots comment), shell test. Not compiled yet.
- **wasm build files** (`games/heartgold/pc/`), D/P's design:
  - `Makefile.wasm`: D/P's `bridge.mk` and `host.mk` by reference (as
    games/ndsrec does), HG's own `mk/game.mk` and `mk/armrec.mk`;
    `-DPC_HOST_SDK42` for the host (pc_card_rom.c then uses the 4.2 card
    command block); the overlay-statics table from main.lsf; the inert
    D-lab hooks and the empty GBA slot from `games/ndsrec/pc/src`.
  - `tools/hg_lsf.py`: the object list of the ARM9 link from main.lsf
    (C, asm, overlay ids; ds_protect's encrypted objects map to the plain
    `lib/dsprot/src/*.c`).
  - `mk/armrec.mk`: overlay asm is staged (symlinks) under
    `$(BUILD)/armrec/stage/arm9/overlays/NN/asm/`, the path armrec reads
    an overlay id from; everything is placed from `main.elf.xMAP`.
  - `host_overrides.txt`: primitives.txt `override` + D's list.
  - `include/pc_prelude.h`; game C uses Platinum's 4.2 SDK shadows
    (types.h, ioreg_CP/G3/G3X) copied into `$(BUILD)/shadow`.
  - Patches: `src/unk_02037C94.c` (const array), `lib/NitroSDK/src/os/
    os_exception.c` (statics un-static'd, OSi_DisplayExContext's mode-switch
    asm block replaced by the plain call).
  - A syntax-only pass over all 437 linked C files (before the ROM build
    generated headers) failed only on missing generated headers, the
    asm-in-C files (handled by strip_asm/extraction) and the two patched
    issues.
- Shared tool changes (gate needed before merge): armrec `adr rd, label
  +/- const`; extract_asm skips `asm` prototypes; dp_extract_asm folds
  mwcc's `#Type.member[...]` immediates via `__builtin_offsetof` and
  `split_operand` keeps brackets inside an operand.

## Where it stopped

`make -f pc/Makefile.wasm -j4 extracted-asm` (run in games/heartgold)
fails on `os_exception.c`: `#OSiExContext.debug[1]` cannot be probed
because `OSiExContext` is a typedef inside the .c itself and the probe
(dp_extract_asm.py `Folder.probe`) only copies the TU's `#include` lines.
Fix: let the probe include the TU's own typedefs (or have the patch spell
the offsets, as D's OS_exception.c.patch does: debug[1] 116, exinfo 108,
debug[0] 112, cp15 100, spsr 104/124 — measure HG's layout). Then:

1. `make -f pc/Makefile.wasm -j4 armrec-classes` (one armrec run over
   375 .s + extracted bodies; an earlier `--scan` without the xMAP was
   98.3% clean, mostly unplaced data the xMAP fixes).
2. `NP_HEAVY_SLOTS=1 tools/heavy.sh make -f pc/Makefile.wasm -j4` and
   work through the link: expected work is asm-in-C TUs whose statics the
   recompiled bodies load by name (un-static patches as on D/P), host refs
   to names HG's asm leaves unnamed (pc_dp_snd.c's `sSeqPlayer`/`sPlayer`;
   size-neutral `.s.patch` label renames), duplicate host/asm definitions
   (add to host_overrides.txt).
3. Native core: `cmake -S core -B build/core-hgss -G Ninja
   -DNP_GUEST_WASM_heartgold=$PWD/games/heartgold/build/pc-wasm/pokeheartgold.wasm
   -DNP_GUEST_POSTPROCESS=$PWD/tools/wasm2c_postprocess.py` then
   `tools/heavy.sh cmake --build build/core-hgss -j4`;
   `build/core-hgss/np_headless heartgold games/heartgold/build/heartgold.us/pokeheartgold.us.nds --frames 1200 --dump /tmp/hg --dump-every 60`.
4. SoulSilver: `GAME_VERSION=SOULSILVER`, same steps.
5. Gate: `tests/dp/regress.sh` (7 cases, fresh D/P/Pt cores) and the
   ROM-only Diamond core vs tests/dp/expected.txt, since armrec.py,
   extract_asm.py, dp_extract_asm.py and pc_card_rom.c changed.

## ARM7 / overlay differences from D/P

- SDK: NitroSDK 4.2 (071210), not 3.2; almost all of it is assembly
  (lib/asm/nitro.s), so host overrides displace recompiled functions;
  only os/mi are C (weak). Card command block is 4.2's (PC_HOST_SDK42).
- No GBA backup library (no CTRDG_IdentifyAgbBackup): empty slot.
- Overlays: 129 (ids by main.lsf order, OVY_0, field = 1, ...), flat
  sources; ds_protect (123) is encrypted on the cartridge and compiled in
  the clear here. No `sinit.h` users in C.
- ARM7: ichneumon_sub (WM via ichneumon component, EXT_WRAM autoload);
  the host replaces the ARM7 as on D/P. Pokéwalker IR is not modelled.
- Disk: ~10 GB free; HG build tree is under games/heartgold/build.
