# HeartGold/SoulSilver: handoff

Branch `hgss` (worktree `/Users/soham/Documents/code/nativeplat-hgss`),
rebased onto main 843e58045 (the pokeheartgold subtree merge re-made with
`git merge -s ours` + `read-tree --prefix`, since a plain rebase flattens
it). Not integrated into `main`.

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
