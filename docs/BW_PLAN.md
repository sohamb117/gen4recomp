# Pokémon Black / White: plan

There is no usable Black/White decompilation (squiddonaut/pokeblack is a
fraction of a percent C, Black only, and unlicensed, so it is neither a
source nor something to vendor). Black/White therefore go the way this
repository proved on Diamond: pure static recompilation of the cartridge
image (`tools/ndsrec` + armrec), linked with the shared machine model
(games/platinum/pc, tools/armrec/armrec_rt.c). This file says what is
already done, what is different about a TWL-SDK 5.3 hybrid cartridge, and
the exact steps.

## Where Black and White stand (2026-10-10)

The dated sections below are a log, kept as written; this is what is proven now, on the core with the ARM7 VBlank
count (build/core-bwm2, "The Musical show"):

- **The story.** tests/e2e's chain (black/white chain.txt, 01-32 and 34) plays a new game through the credits
  (32 ends in the game's own `OS_ResetSystem`) and the post-credits lab, on both games ("The chain and the side
  systems on the VBlank-count core"). Evidence: `build/evidence/bw/<game>-<milestone>/` (contact sheets, end saves).
- **Side systems.** All 29 in each systems.txt pass: HMs, fishing, the day care and an egg, evolution, an NPC trade,
  the PC, the Battle Subway, the Hall of Fame PC, the Musical, the Entralink, Royal Unova, TV, Black City / White
  Forest, the legendaries, the roamers, and Landorus by link trade. The excluded ones (online, event, the
  Xtransceiver's calls between players) are listed with reasons at the end of black/systems.txt.
- **Link play.** The Union Room trade and battle-room battle (bw_trade, bw_battle; "Link play"), and the post-game
  trades between the chain's players: trade evolution (a Boldore becomes Gigalith on White's station) and both
  roamers to either game (bw_trade_landorus, bw_trade_landorus_white), each continued by the e2e Landorus run
  (black/white 88-89; "The post-game trade"). Evidence: `build/evidence/bw/link-bw_trade_landorus*/`.
- **Mystery Gift.** `tests/bwhgss/run_features.py mystery_gift` passes on both games on core-bwm2 (an item card and
  a Pokemon card delivered by the Pokemon Center's deliveryman, both marked used in the game's save).
- **Poké Transfer** is in progress ("Poké Transfer", maintained separately).

## Accepted ROMs

| Game | File (No-Intro name) | SHA-1 | Source |
| --- | --- | --- | --- |
| Black (USA, Europe) | `Pokemon - Black Version (USA, Europe) (NDSi Enhanced).nds` | `26ad0b9967aa279c4a266ee69f52b9b2332399a5` | the project's expected hash; matches the user's dump |
| White (USA, Europe) | `Pokemon - White Version (USA, Europe) (NDSi Enhanced).nds` | `bc696a0dfb448c7b3a8a206f0f8214411a039208` | [TASVideos game version #3246](https://tasvideos.org/Games/3049/Versions/View/3246) ("Good", MD5 `77c34ba77f8fa44e7caf04f695db0560`); the user's reference also gives CRC32 `b552501c` |

Both are 256 MiB, header `POKEMON B` / `IRBO` and `POKEMON W` / `IRAO`, unit
code 2 (DSi-enhanced), revision 0. They live in the gitignored `roms/` of a
checkout. Nothing extracted from them (assembly, data, symbol maps) is ever
committed: every ndsrec output goes under a `build/` directory. When Black
and White get a core, these two SHA-1s go into the shell's ROM table
(`shell/src/romdb.c`), White with the MD5/CRC32 above.

## What the front end already reads

`python3 tools/ndsrec/ndsrec.py info -v <rom>` on both cartridges (all
numbers below are its output):

| | Black | White |
| --- | --- | --- |
| SDK version word (`_start_ModuleParams`) | `0x0503757C` (TWL-SDK 5.3) | same |
| ARM9 static | ROM 0x4000, RAM 0x02004000, entry 0x02004800, 0x6F898 bytes BLZ-compressed, 0xA5E80 (679 KB) plain | 0x6F8A4 compressed, static to 0x020A9EA0 |
| ARM9 BSS | 0x020A9E80-0x021542C0 | 0x020A9EA0-0x021542E0 |
| autoloads | ITCM 0x01FF8000 (0x820), DTCM 0x02FE0000 (0xA0 + 0x20 BSS), 0x02400000 (0x20), 0x06898000 (0x20) | same |
| ARM7 | ROM 0x2C7E00, RAM 0x02380000, 0x28F84 bytes | same |
| overlays | 237 (229 BLZ-compressed), 3.26 MB plain; 57 are 32-byte empty stubs | 237 (228 compressed) |
| TWL sections | ARM9i ROM 0xC403000 → 0x02400000 (0x12F24), ARM7i 0xC416000 → 0x02E80000 (0x470F8) | ARM9i 0x12F1C |
| discovery | 35,941 functions (28,779 Thumb, 7,162 ARM), 1,288,513 instructions, 7 undecodable words; ov230 opaque (below) | 35,938 functions, 1,288,538 instructions |
| primitive map (`sigs.py match`, learned from Diamond) | 71 of 136 placed (bytes 56, sequence 7, call 3, literal 3, crt0 1, similar 1) | not run |
| emit + armrec | 316 files, 35,927 functions, 1,287,136 instructions; 5,252 cross-overlay calls bound by name, 7,457 left to run-time dispatch; armrec translates 35,871/35,871 cleanly (the 65 placed host overrides dropped) | not run |

For scale, the ROM-only Diamond build emits 26,508 functions and 1,046,608
instructions (armrec 26,399/26,399 clean); Black/White are 23% larger.

## Code size

Measured on the ROM-only Diamond core and scaled by instruction count
(x1.23): armrec's C is 129 MB for Diamond, so about 160 MB for Black; the
wasm module (all of it, host included) 21.5 MB, so about 26 MB; the
wasm2c'd guest library 34 MB and np_headless 28 MB natively, so about
42 MB and 34 MB. A shell build carries one core per game, so Black and
White together add two of those (their code differs by a few dozen bytes
per module, but each core is built from its own ROM).

## Plaintext and encrypted regions of the dumps

Determined from header fields and from whether the stored bytes read as
code; nothing is decrypted and no key is used.

| Region | Stored as | Evidence |
| --- | --- | --- |
| header 0x000-0x1FF, TWL header 0x200-0xFFF | plaintext | header CRC16 at 0x15E matches |
| ARM9 secure area, first 2 KB (0x4000-0x47FF → 0x02004000) | the decrypted form NTR dumps carry | starts `FF DE FF E7 FF DE FF E7` (the destroyed-ID marker of a decrypted dump); 17 plaintext Thumb `swi #N; bx lr` veneers (N = 0x00, 0x03-0x06, 0x09, 0x0B-0x15) among high-entropy filler (7.1 bits/byte per 256 bytes). Diamond, whose link map names the region (`libsyscall.a secure.o`: SVC_GetCRC16 = `swi 0xE`, SVC_CpuSet = `swi 0xB`, SVC_WaitByLoop = `swi 0x3`, SVC_Sqrt = `swi 0xD`, the rest `$d`), has the same layout and entropy. The header's secure-area CRC (0x6C) does not match the stored bytes, as expected: it covers the encrypted transfer form. |
| ARM9 rest of the static, crt0 at 0x02004800 onwards | plaintext (BLZ-compressed from the end) | decodes as code; discovery reaches it from the entry point |
| ARM7 (0x2C7E00, 0x28F84 bytes) | plaintext | 6.0 bits/byte, decodes as ARM |
| overlays 0-236 | plaintext, 229/228 BLZ-compressed | decode as code, except ov230 (below) |
| ARM9i (0xC403000) | modcrypt area 1 (0xC403000, 0x4000 bytes, header 0x220) | 8.0 bits/byte over its first 16 KB; TWL-only, never loaded in NTR mode, ignored |
| ARM7i (0xC416000) | outside modcrypt area 1 | TWL-only, ignored |

**Secure-area veneers.** Only the 2 KB the cartridge KEY1-encrypts needs
care, and these dumps store it in the decrypted form, so the SDK's syscall
veneers there are plain `swi #N; bx lr`. Discovery enters the range only
through calls from plaintext code (Black calls three: 0x0200421A `swi 0xB`
CpuSet, 0x02004490 `swi 0x3` WaitByLoop, 0x02004632 `swi 0xE` GetCRC16),
never fills gaps or follows pointers into it, and `Module.check_secure_area`
stops the build if anything other than such a veneer is found there; armrec
turns each veneer into the host's `armrec_swi(N)`, the BIOS call the
machine model already implements. The filler is never decoded. A dump that
stored this area encrypted would fail that check, and the calls would then
be routed to host SVC handlers named from their call sites (the 2-instruction
veneer contract is the SDK's public libsyscall layout) rather than read.

**ov230 is self-modifying.** Its five static initialisers start by taking
their own address (`orr r0, pc, #0`), and one routine cleans and
invalidates cache lines over a range (`mcr p15, 0, rN, c7, c5, 1` and
`c7, c14, 1`): the overlay rewrites its own code when loaded, the shape of
a protection scheme. Its bytes at rest are not the code that runs, so no
static recompiler can translate it, and decoding it is out of bounds.
`Module.check_self_modifying` leaves any such overlay opaque (data only;
`info` and `emit.txt` list it), so a call into it reaches run-time dispatch
and stops there by name. No other module has this shape (the only other
I-cache line invalidations are crt0's and the SDK's IC_/DC_ functions in
the static). Whether the game loads ov230 on the boot path is the first
thing a Black boot will show; if it does, that is a hard blocker for this
approach, to be reported, not worked around.

## The ROM-only Diamond core (the proof)

`games/ndsrec` builds Diamond from its ROM alone (`make -f pc/Makefile.wasm`,
then the native core). Against the decompilation-built core:

- `d-boot` (600 frames) and `d-intro` (3,000 frames, `tests/dp/intro.sched`)
  give exactly the hashes in `tests/dp/expected.txt` (49e21389a9f73440,
  74de04024a1ba0df): copyright, title and Rowan's intro are identical,
  audio included.
- The new game plays to the first save (`tests/dp/first_save.sh`, with
  `NP_HEADLESS=build/core-ndsrec/np_headless`; the script builds np_save4
  from the checkout it runs in): the stored save holds the trainer NATIVE,
  CONTINUE loads it and saves again. No run-time dispatch miss (a miss
  aborts and names the address).
- Every Diamond case of `tests/dp/expected.txt` matches
  (`NP_DP_CORE=build/core-ndsrec tests/dp/regress.sh --no-build --only
  d-boot --only d-intro --only d-state-boot --only d-state-save`),
  d-state-save (12,300 frames, the bedroom and the first save) included.
  The player's avatar was missing from the bedroom on until the host's
  fixed-size MI copies modelled the geometry engine: NNS G3d's billboard
  handler reads the clip matrix through the recompiled `G3X_GetClipMtx`,
  which passes the register address 0x04000640 itself to `MI_Copy64B`
  (decompiled C passes a block `armrec_gx_reg()` has just refreshed), so
  the host copied a stale matrix and the billboard was clipped away.
  `MI_Copy16B`..`64B` (games/platinum/pc/src/pc_mi.c) now refresh a source
  in the result block and push each word stored into the command window
  (`G3_MultMtx33`'s `MI_Copy36B` to GXFIFO).

## What is different from Diamond

**TWL-SDK 5.3, hybrid cartridge, NTR mode.** On a DS (and on this port,
which models a DS) a DSi-enhanced cartridge boots like any DS game: the
ARM9i and ARM7i modules at header 0x1C0/0x1D0 are only loaded by a DSi in
TWL mode and are ignored. The NTR-mode binaries still differ from
NitroSDK's in ways the front end already handles:

- the ARM9 static loads at 0x02004000, not 0x02000000 (the first 16 KB is
  reserved), with crt0 at 0x02004800;
- autoload list entries are 16 bytes (`ram, size, sinit, bss`) instead of
  12 (`tools/ndsrec/nds.py` picks the size from the SDK version word);
- overlays are BLZ-compressed (flag bit 0 of the overlay table entry), which
  `nds.py` decompresses. The run-time half is not done: Diamond's host
  refuses compressed overlays (`games/diamond/pc/src/pc_dp_overlay.c`),
  because on hardware `FS_StartOverlay` decompresses in place with
  `MIi_UncompressBackward` before the static initialisers. The ROM-only
  core needs that call in its `FS_StartOverlay` override (the host already
  implements `MIi_UncompressBackward`).

**Memory map.** The DTCM autoload is at 0x02FE0000 and TWL-SDK addresses
the system shared area at 0x02FFFxxx, both in the main-RAM mirror region
(0x02000000-0x02FFFFFF mirrors the 4 MB every 4 MB on a DS) that
armrec_rt.c leaves unmapped (it maps 0x027E0000-0x02800000 and a port
window at 0x02A00000). Black/White need the mirror modelled: 0x02FE0000 as
the DTCM, and 0x02C00000-0x02FFFFFF (at least the shared page) aliased to
main RAM, done the way VRAM aliasing is done (mmap on POSIX, a copy model
on wasm32). The 0x02400000 and 0x06898000 autoloads are 32-byte NTR-mode
stand-ins (the first is the start of TWL-only memory, the second LCDC
VRAM bank H); both must be written where the hardware loader would.

**ARM7.** Black/White ship their own NTR-mode ARM7 component (0x28F84 bytes
at 0x02380000; no SDK version string in it, unlike the ARM9's
`[SDK+NINTENDO:WiFi3.3.30052...]`). The port does not run any ARM7 code:
the host answers the ARM9's PXI requests in C (games/platinum/pc/src
pc_pxi_*, card, RTC, touch, power, wireless responders) and runs D's ARM7
sound driver as host C (pc/arm7snd). Each responder has to be checked
against the TWL-SDK 5 protocol before it is trusted:

- *PXI tags and FIFO*: the tag numbering and the IPC FIFO protocol are
  shared by NitroSDK and TWL-SDK in NTR mode; confirm by reading the tags
  the recompiled `PXI_SetFifoRecvCallback` callers pass.
- *sound*: the ARM9 NNS sound archive player sends SND commands through a
  shared command buffer whose layout is SDK-version specific; pc/arm7snd is
  Diamond's ARM7 SND driver and is the matching pair only for NitroSDK 3.x.
  Compare the ARM9 SND command structures (recompiled `SND_*` callers) and
  adapt the driver if they moved.
- *backup*: Black/White save to a 512 KB SPI flash; the card backup
  responder and save file size have to accept it.
- *RTC / touch / power*: same PXI tags in NTR mode; verify the command
  words the recompiled `RTC_*`, `TP_*`, `PM_*` send.
- *C-Gear*: the wireless features (C-Gear survey radar, Entralink link
  play, Union Room) use the DS wireless (WM) through the ARM7 like every
  other DS game and the existing WM responder; the infrared features use
  the IR transceiver on the cartridge and are out of scope (the IR
  requests must answer "no partner", not hang).

**Signatures.** `tools/ndsrec/sigs.py` learns the 136 host primitives from
Diamond (NitroSDK 3.2) and places them in Diamond 136/136 and in
Platinum's ROM 112/131 (0 wrong), using byte patterns, normalised
instruction sequences that survive toolchain differences, callers and
literal pools. In Black it places 71 of 136: the OS core (context save and
load, reschedule, IRQ handler, halt, interrupts, lock IDs, alarm and
exception handlers, tick), cache and protection-unit control, MI copies and
fills, the matrix/G3 helpers, the three secure-area SVC veneers,
`PXI_SendWordByFifo`, `OS_UnLockCartridge`, the NNS sound players and
NitroMain. Not placed, i.e. rewritten in TWL-SDK 5: the card layer
(`CARD_Init`, `CARDi_ReadRom`, `CARDi_Request`, `CARDi_SetTask`,
`CARD_WaitRomAsync`, `CARDi_InitCommon`, `cardi_common`), `FS_StartOverlay`,
`MIi_UncompressBackward`, `MI_UncompressLZ8`, the MI DMA and GX-command
helpers, `PXI_Init`/`InitFifo`/`IsCallbackReady`/`SetFifoRecvCallback`,
`PMi_*`, `TP_*`, `OS_GetIrqFunction`, `OS_WakeupThread`, `OSi_ThreadInfo`,
`OS_ResetSystem`, `OS_SetDPermissionsForProtectionRegion`, `SVC_Sqrt`,
`NNSi_SndCaptureStart`, `WMi_StartMP`, `MTX_RotX43_`/`Rot*44_`, abort and
the assertion handler; the CTRDG/AGB-flash group is Pal Park's and has no
counterpart. Where a primitive cannot be placed, the fallback is to leave
it recompiled: most of the host's overrides exist for speed or for
hardware the machine model also emulates at the register level. The ones
the boot cannot do without are the card ROM reads (the host serves ROM
reads from the file; the machine model has no card protocol), overlay
loading with in-place BLZ decompression, and the PXI FIFO set-up: those
have to be placed in Black by hand-reading the recompiled SDK (their
callers are placed: `FS_StartOverlay` is reached from the overlay loader,
the card layer from FS) and recorded as anchors in sigs.py.

## Steps, now that the ROMs are here

1. Front end on both ROMs (done): `ndsrec.py info -v <rom>` (table above).
2. Emit and translate (done for Black): `make -f pc/Makefile.wasm
   ROM=<path without spaces, e.g. a symlink under build/> VER=black` in
   games/ndsrec runs sigs, emit and armrec; stage 1 is 100% clean.
3. Primitive map (in progress): 71/136 placed automatically. The link of
   the Black core stops on 13 symbols the host references and the map does
   not give: `OSi_ThreadInfo`, `OS_GetIrqFunction`, `OS_WakeupThread`,
   `OS_UnlockCartridge`, `TP_GetCalibratedPoint` (TWL-SDK SDK code) and the
   Pal Park group (`CTRDG_*`, `CTRDGi_ReadFlashID`, `AgbFlash`; Diamond-only
   host code that a Black host fragment must leave out). Read off the
   recompiled SDK, by the same instruction shapes as Diamond's: the five
   are at 0x02150FEC (OSi_ThreadInfo: OSi_RescheduleThread's `strh` target,
   irqDepth at +2 as in NitroSDK), 0x02084818 (OS_GetIrqFunction; TWL-SDK
   scans 32 sources, the table is at DTCM+0x20 and timer slots are +5),
   0x02085800 (OS_WakeupThread), 0x02084D78 (OS_UnlockCartridge, the target
   of the placed OS_UnLockCartridge veneer; its lock word is 0x02FFFFE8) and
   0x0208B104 (TP_GetCalibratedPoint; calibration flag at +0x34, not +0x30).
   These are rules for sigs.py (a placed veneer's literal names its target;
   an object is a placed caller's store target), not a committed map.
   Then the boot-critical card/overlay/PXI primitives (above).
4. Runtime: the main-RAM mirror and 0x02FE0000 DTCM in armrec_rt.c (under a
   per-game switch so Diamond/Pearl/Platinum stay byte-identical: re-run
   Platinum's 600-frame hash and Diamond's scenarios), overlay
   decompression in the ROM-only `FS_StartOverlay`, and the 0x02004000
   static base in the boot (crt0's literal pool, `ndsrec.py lcf`).
5. Core: `make -f pc/Makefile.wasm ROM=roms/<Black>.nds VER=black` in
   games/ndsrec (the Makefile already takes the ROM as its only input),
   `cmake -S core -B build/core-bw -DNP_GUEST_WASM_<id>=...`, boot with
   np_headless and dump frames to the title screen.
6. ARM7 protocol checks (sound, backup, RTC, touch, power, WM) as listed
   above, one responder at a time, each against a recorded scenario.
7. Shell: add both SHA-1s to `shell/src/romdb.c` with the new core id.

## Status of the Black core (2026-10-07)

- Primitive map: `sigs.py` places 103 of 136 in Black (Platinum's ROM 132
  found, 129 verified against its link map, 0 wrong; Diamond 136/136,
  map unchanged). Rules beyond the byte/sequence ones: the literal a placed
  veneer branches through (OS_UnlockCartridge), the literal at a placed
  function's aligned load (OSi_ThreadInfo), the unique caller of placed
  callees (4b: FS_StartOverlay, MI_WaitDma, MI_StopDma, MI_DmaCopy32Async,
  MI_SendGXCommandAsync(Fast)), module-wide similarity once the rounds stall
  (6: OS_GetIrqFunction, OS_WakeupThread, TP_GetCalibratedPoint, SDK
  helpers such as the callers of CARDi_ReadRom and MI_DmaFill32), and the
  gap rule (5b): TWL-SDK lays MI_dma.o out in the reverse order, and the
  rewritten MI_DmaCopy32 (0x02082244), MI_DmaCopy16 (0x020822E8) and
  MI_DmaFill32Async (0x02082390) are placed by their position between the
  placed MI_DmaFill32 and MI_DmaCopy32Async. Not placed: CARD_Init,
  CARD_WaitRomAsync, CARDi_Request, CARDi_SetTask, MI_SendGXCommand,
  MI_UncompressLZ8, MTX_Rot*4x_, PMi_WriteRegister(Async), SVC_Sqrt,
  NNSi_SndCaptureStart, abort/assert and the Pal Park group.
  OSi_AlarmHandler is no longer a primitive: the host's is a trap, and
  Black's NVRAM driver needs the SDK's (below).
- Memory map, card bus, backup, OS_InitLock handshake: commit 3a8f74b4b
  (ARMREC_TWL shared region at 0x02FE0000, ARMREC_CARD_HOOK, the tag-11
  backup responder on the SDK 4.2 command block, 512 KiB flash).
- The first-frame SIGBUS was the host stack overflowing (lldb: EXC_BAD_ACCESS
  on `stp` at the entry of sub_02084204, the NVRAM tag-4 receive callback,
  in an endless sub_02084204 -> sub_020839E8 -> sub_020844A8 ->
  PXI_SendWordByFifo -> responder -> sub_02084204 chain). TWL-SDK's NVRAM
  state machine (DWC writing its user ID to the firmware flash at 0x7FBF8)
  sends the next command from the callback and only then stores its new
  state; the reply, delivered inline, found state 0 and sent the same page
  write again. pc_pxi.c now holds replies raised inside a receive callback
  until the callback returns, as the hardware's non-reentrant FIFO
  interrupt does. The write then waits out an OS alarm, which the SDK's
  OSi_AlarmHandler handles from the timer model's timer-1 interrupt.
- Boot runs: the Pokémon Company / Nintendo logo (frame 100), the
  copyright (300), the Game Freak logo and the opening movie (500-4500),
  the title with the 3D Reshiram (4750 on), sound audible (rms ~6000).
  PXI tag 13 (CTRDG) is dropped without harm; tag 0x17 is TWL's SCFG clock
  tag (its callback writes 0x04004004) and is never sent to.
- Two machine-model gaps the movie and the title menu hit, both general:
  - flags returned from a call: TWL-SDK's EABI soft-float comparisons
    (sub_0209BC74 and kin) return their result in the flags with
    `msr cpsr_f`, and the caller branches on them after `bl` (ov88's angle
    normalisation looped forever). armrec now merges a function's local
    flags into `mrs`, loads them back on `msr` to the flags field, and
    after a call to any function writing the CPSR flags
    (FLAG_RESULT_FUNCS) reloads them from the CPSR;
  - busy waits on the tick: Black's IR-chip probe (ov231, loaded when
    Start is pressed at the title; it drives AUXSPI and reads 0x08 back,
    not the chip's 0xAA, so the game takes it as absent) spins on
    OS_GetTick for 50-60 us. Files naming a timer counter get
    ARMREC_TIMER_HOOK; after 256 counter reads within one frame each read
    advances the timers by 256 cycles (pc_timers.c), overflow interrupts
    pending until the VBlank.
- New game: the professor's intro runs (her lines, the boy/girl choice, the
  name, "Let's go meet the world of Pokémon!", frames 5300-10000 with A
  held every 40 frames from 5300). Her sprite is not drawn in most of those
  frames (top screen white with the text box): not investigated.
- **Original blocker before the experimental proposal: ov230.** At frame
  10044, right after the intro, the static
  module's sub_02011D9C (Thumb, reached through a function pointer from the
  state runner sub_020315F0 <- sub_020313EC <- sub_020055F8 <- NitroMain)
  loads overlay 230 through sub_02034AC4 -> sub_02079264 -> FS_StartOverlay
  and then calls into it (0x021882A0 and 0x02188354, run-time dispatch,
  comparing the first result with the complement of a literal). ov230 is the
  self-modifying overlay left opaque; its five static initialisers are not
  recompiled, so the unmodified run stops in pc_dp_overlay_sinit by name.
  The experiment below leaves the overlay opaque and changes its caller
  instead. `PC_TRACE_OVERLAYS=1` (np_headless `-e PC_TRACE_OVERLAYS=1`)
  prints every overlay made resident.
- White's original core reaches its title (Zekrom) by frame 4500 with the
  same overlays (10, 11, 9, 13, 228, 88, 179, 15).

## Experimental startup proposal and runtime verification (2026-10-07)

The active development build deliberately substitutes outcomes in `sub_02011D9C`; it does
not faithfully implement overlay 230 or establish full playability.
`pc/patches/<black|white>/arm9/asm/ndsrec_arm9_004.s.patch` makes the five reviewed call-site substitutions
in Black and White's generated `ndsrec_arm9_004.s`: omit this caller's
overlay load/unload and third call, return zero from the first check, and
return the complement of `r1` from the second. Each four-byte Thumb BL
becomes two two-byte instructions, preserving guest addresses. Making
both comparisons equal would be a different, untested proposal.

`pc/mk/ndsrec.mk` applies it after assembly emission and before armrec
(the per-version assembly patch step: every `pc/patches/<VER>/*.s.patch`,
no fuzz), only for `VER=black` or `VER=white`; `pc/patches/<VER>/SHA256SUMS`
pins each patched file to the reviewed output, so an emission a patch no
longer fits, or a result other than the reviewed one, fails the build.
Regeneration reapplies the same proposal. The two checked-in generated
caller files are exact tested snapshots, not substitutes for this build
step. No ROM bytes are changed and overlay 230 is not decoded. (Until
2026-10-08 a hash-guarded script, `pc/patch_bw_startup.py`, made these
edits; the patches reproduce its output byte for byte, the SHA256SUMS
lines being its "proposed" hashes.)

Independent isolated runs used Black ROM SHA-1
`26ad0b9967aa279c4a266ee69f52b9b2332399a5` and White ROM SHA-1
`bc696a0dfb448c7b3a8a206f0f8214411a039208`.
Both wasm guests and one native core containing both games compiled and
linked successfully. Native configuration was Release,
`CMAKE_C_FLAGS_RELEASE="-O1 -g0 -DNDEBUG"`, `NP_BUILD_TESTS=OFF`,
and this checkout's `tools/wasm2c_postprocess.py`; the existing guest
module rule appends `-O2` to generated native C. Builds used `heavy.sh`
with `-j4`, and runtime invocations used `heavy.sh --run`.

Observed independently for **both** games:

- New-game intro and controllable starting bedroom passed; the original
  frame-10044 stop was passed without loading overlay 230.
- Movement and gift interaction selected Tepig. The first battle's
  preceding dialogue rendered, but the battle itself did not start.
- The real X-menu SAVE operation before choosing a starter wrote a
  524288-byte backup file. The game displayed its saved-game message.
  A fresh process displayed CONTINUE with the saved trainer, Nuvema Town,
  zero badges and time 0:03, and restored the bedroom.
- Repeated map transitions are **blocked, not verified**. Trying the
  staircase before opening the gift triggers Cheren's “Aren't you going
  to check out the gift box? Where are you going?” story gate. Following
  the mandatory starter sequence instead hits the battle failure below.

The extended new-game input was
`5000:start:4:60:2;5300:a:4:40:400;22000:down:12;22030:a:4:40:1000`.
Both runs failed with exit 1 at frame **24802**, while entering the first
battle. Black reports an unowned indirect target **0x021BBB0C**; White
reports **0x021BBB2C**. Resident overlays are 10, 11, 18, 20, 93, 94, 95
and 96. Separate debugger runs of the same executable locate the callers
in overlay 93 (`ov93_021BB9D8` in Black, `ov93_021BB9F8` in White).
The emitted overlay-93 assembly has `bx pc` at 0x021BBB08 / 0x021BBB28,
with the following ARM veneer left as `.byte` data rather than registered
code. This mixed-ISA translation gap is the immediate observed blocker;
the startup patch does not repair it or replace it with another bypass.

The tested proposed source SHA-256 values are:

- Black: `6ac07a4b418fd9e22ca11402bc1f9341fb01718c62f73c4aea8ed9e833982b3e`
- White: `95777aa0b21db0f416c3fd6d30220a697476bd9a1e66fee82b170fe40c533597`

The runtime result demonstrates limited startup/save/reload success,
not reliable wider-world gameplay, postbattle saves, or harmlessness of
all omitted overlay effects. Generic `field_ready` / `map_id` headless
statistics were not used as BW gameplay evidence; actual frames,
independent logs, game-menu saves and fresh-process reloads were.

The patches and regeneration step are integrated into the active development
tree. The results above predate integration; no additional test runs were
performed for publication, at the user's request.

## Save data: `save5` and `np_save5` (2026-10-08)

`features/save5` reads and edits Black/White saves the way `save4` does for
D/P/Pt, and `features/tools/np_save5` mirrors `np_save4` (`dump [rom]`,
`gamedata`, `verify`, and the edit verbs except `set-coins`,
`set-dex-obtained` and `set-mystery-gift`, which have no Black/White
counterpart). `ndsdata` reads Black/White ROMs for names, zone names and the
battle tables (Gen 5 text banks, personal/growth/move NARCs, the type chart
in BLZ-compressed overlay 93).

- Layout: primary copy at 0x00000, backup at 0x24000; 69 data blocks, each
  followed by {u16 write counter, u16 CRC-16-CCITT}, and a checksum block at
  0x23F00 mirroring every block CRC, ending in {u32 save counter, u32
  0x23F9C, u32 magic 0x31053527, u16 0, u16 CRC of the mirror table}.
  Every check (footer, mirror, table) is verified per copy and mismatches are
  reported per block. The game writes both copies; the two real saves have
  identical copies (save counter 2).
- The runtime saves of both games (pre-starter, X-menu SAVE) validate in
  both copies and dump as AAAAAAA, Nuvema Town (zone 391), 0 badges,
  0:03:03, money 3000. Their 1,440 empty box slots per game equal save5's
  encryption of an empty Pokémon.
- Edits recompute the block CRC, its mirror entry and the table CRC, and are
  mirrored to the other copy when that block was identical in both; an
  unmodified load/store is byte-exact.
- Tests: `pkm5`, `save5`, `cli5_e2e` (synthetic saves), `gen5_rom` and the
  ROM half of `cli5_e2e` with `-DNP_BLACK_ROM=<black.nds>`.
- The shell's save editor, slot summaries, slot import/export and Mystery
  Gift import read Black/White saves through `shell/src/edsave.c`, one
  interface over `save4` and `save5`: trainer, money, badges (Unova's),
  party/boxes with Add Pokemon (needs the imported ROM), bag, Pokédex,
  event flags/vars, `.pgf` Wonder Cards into the twelve slots, Trainer Card
  and diploma. Tests: `shell_unit` (the interface on synthetic saves) and
  `shell_editor_bw` (the app on SDL's dummy driver, stub core: every tab,
  edits saved and checked with `np_save5`, slot import from the Black card;
  Add Pokemon with `-DNP_BLACK_ROM=<black.nds>`).
- `tests/e2e` judges Black/White end saves with `np_save5 dump`; `auto_battle`
  would still need `np_save5 gamedata`, Gen 5 type ids (17 types) and Gen 5
  move effect ids in `bots.py`.

## End-to-end probe (2026-10-08)

`games/ndsrec/pc/src/pc_bw_e2e.c` publishes the np_e2e.h block for both
games from host code that reads the recompiled game's structures at the
frame boundary and calls the game's own terrain query and object movement
check; docs/BW_RAM.md lists every address and offset with how it was
proven. `tests/e2e/run.py --game black|white` runs milestones on it
(tests/e2e/README.md, "Black and White"): 01 plays a blank chip through
the intro to the bedroom, walks it with `walk_to`, talks to Cheren and saves
through the X menu. In a battle the probe sets `in_battle` and reports the
battlers and the player's party (species, level, HP, types, moves, PP) from
overlay 93's POKECON, shown on both games in Bianca's battle; not reported
yet: the battle's menus (`ui`, needed by `auto_battle`) and warps.

## First battle: discovery fixes and the current stop (2026-10-08)

Five general ndsrec defects stood between the bedroom and the first battle.
All are in `tools/ndsrec` (discover.py, emit.py), so they change what ndsrec
emits for every ROM; D/P/Pt's decompilation-built cores do not use it.

- **Thumb `bx pc` veneers.** mwldarm's Thumb-to-ARM/far call stub is
  `bx pc; nop; ldr ip, [pc]; bx ip; .word T`. Discovery stopped at `bx pc`
  and the ARM half was emitted as `.byte`, so the first battle's
  `ov93_021BB9D8` aborted on 0x021BBB0C (White 0x021BBB2C), the veneer into
  ov95, which loads into LCDC VRAM at 0x06898020. A word-aligned `bx pc`
  now tail-enters an ARM function at +4, and a BL to a `bx pc` is never a
  long branch. **901 sites in each ROM** (static 3, ov93 386, ov95 512).
  With all five fixes below Black's discovery gains 1,247 functions (ov93
  621, ov95 602, static 19, ov121 3, ov29 1, ov53 1) and 6,081 instructions,
  most of them ov93 code lost behind veneer calls misread as long branches.
- **Halfword-aligned Thumb entries.** main's armrec now follows
  `thumb_func_start`'s `.balign 4`, which shifted every label after the
  secure-area SVC veneers (0x0200421A) and aborted at boot ("two different
  functions are registered at 0x020083B4"). ndsrec emits
  `non_word_aligned_thumb_func_start` for those.
- **Static helpers only overlays call** (0x0207AD58, MTX_Scale22_'s shape,
  behind the ARM MTX_Identity22_; 0x0207AE24): the overlays' call census now
  also feeds a second static round.
- **Detached blocks**: a block a function reaches past its neighbour's start
  (the static's unaligned MI copy loops, 0x02082F84 and five more) becomes
  a function entered by a tail branch.
- **Long branches**: a Thumb BL within 2,000 bytes is a call (0x02014A10 was
  read as a long branch, losing the rest of sub_0201493C); a long branch is
  emitted as `b label` plus the BL's second halfword as `.byte`, because
  armrec reads a BL to a local label by B's exact reach and mwcc used BL
  for a target 2,048 bytes back (0x0201558C).

The static's `ndsrec_arm9_004.s` is unchanged by all of this: the startup
patch's SHA256SUMS and both tracked snapshots stand. Diamond's ROM-only
emission gains 4 functions (static 3, ov13 1) and the new spellings; its
hash check is in the report of this change.

Result, both games, from the pre-starter save (`;5000:start:4:60:2;
5300:a:4:40:20;7000:down:12;7030:a:4:40:1500`): gift box, Tepig chosen,
overlays 93-96 load at frame 9801, the battle backdrop and "You are
challenged by Pkmn Trainer Bianca!" with both party bars (frame 10001), no
abort through frame 20000. **Current stop:** the intro never sends out a
Pokémon; from about frame 10200 the screen stays on the backdrop with the
bottom-screen Poké Ball while the clock runs. All game threads are idle
(the main loop waits for VBlank each frame); every frame the battle proc
`ov93_021B6434 -> 021B83F4 -> 021CDA18 -> 021CDDDC -> 021E9104 ->
021EB52C -> 021EB870` polls `ov94_021F7EB4`, which reports busy while any
count in the object at `*(0x0220AF20) + 0x1E8/0x1EC` is non-zero (writers:
ov94_021F8884..021F903C, 021F9478, 021F94F8, 021FC374/448). Which pending
job never completes (sound, an async load, an effect) is the next thing to
establish. Guest memory in np_headless starts at host 0x300000000 (lldb
`x/wx 0x300000000+ADDR`).

## First battle runs: VCOUNT, three ndsrec rules, and the other overlay-230 callers (2026-10-08)

The send-out wait above was the machine model. `ov94_021F78E4` stores the
battle effect VM's busy state (`ov94_021F9478` -> `sub_02011298`) into
`*(0x0220AF20) + 0x1E8` every frame; the send-out effect sat in VM wait mode
2 on `ov94_021FC9F4`, which waits for bit 16 of the VM's work word. Only
the VBlank task `ov94_021FE9BC` clears it (registered with `sub_020056A0`;
the main loop runs these tasks right after its VBlank wait, in
`sub_0200567C`), and it transfers only while REG_VCOUNT is 192..200. The IO
window is plain RAM and nothing counted lines, so VCOUNT read 0 forever
(the static's `sub_02016880` / `sub_0201691C` have the same check). Proven
with a write watchpoint on the busy word and, without a rebuild, by
storing 0xC0 into 0x04000006 with lldb: both Pokémon were sent out. Fix
(`pc_os_lite.c`, OS_Halt): VBlank delivery sets VCOUNT to 192 and
DISPSTAT's VBlank flag (Black's `ov10_02166A6C` updates BG2's affine only
while that bit is 1). HBlank is still not modeled, so per-line readers
(D/P/Pt's window and scroll effects) still don't run.

Past the send-out the battle aborted at 0x021CCE60 (White 0x021CCE80).
Three general discovery rules in `tools/ndsrec/discover.py`:

- **Thumb switch bound**: the bound is the compare that tests the upper
  bound. In `ov93_021CCC0C` a signed `cmp r6, #0x34; bgt default` comes
  first and a `cmp r6, #0; bge` into the switch follows; the nearest
  compare was taken as the bound (one case), so the other cases were
  `.byte`.
- **ARM block jumps**: in `add pc, pc, rN, lsl #3` (or #4, #5), every block
  start from a + 8 is an entry point. These are the static's small-copy
  tails `sub_02082EA4` / `sub_02083194`, which overlays 9, 21, 119, 135
  and 172 call; armrec's multi-entry merge handles the fall-through.
- **Gap ISA from pointers**: a pointer word to a gap's start decides
  whether the gap is ARM or Thumb. `ov121_021DDC30` / `021DDC44` are ARM
  leaves called by `blx` through even literals; six bogus Thumb functions
  disappear.

Black has 37,181 functions (was 37,167). `ndsrec_arm9_004.s` is
byte-identical, so the startup patch and both snapshots stand. bw-coverage's
census finds nothing undecoded left in overlays 93-96. Diamond's ROM-only
emission changes only through the switch rule (8 files, new cases only).
Its four cases keep their hashes (d-boot 49e21389a9f73440, d-intro
74de04024a1ba0df, d-state-boot ae26086a86be9952, d-state-save
2dc2ff9408c62d1a).

Result, both games, same save and schedule as above: Snivy and Tepig are
sent out (frame ~10400). "What will Tepig do?" appears with
FIGHT/BAG/RUN/POKÉMON on the touch screen (Black frame 10609). Turns run
("The foe's Snivy used Tackle!", frame 10801), and mashing A wins: "got
$500 for winning!" at frame 13701 (Black Tepig 14/22, White 12/22).

**After the win**, overlay 92 loads at 0x021B95A0, and `ov10_0216EB98`
(White `ov10_0216EBB8`) calls `sub_02034AC4` (White `sub_02034ADC`) to
start overlay 230: frame 13756 in Black, 13796 in White. ov230's five
static initialisers have no translation, so the core trapped. By the
user's decision, every ov230 caller now gets the startup treatment
(`pc/patches/<VER>/arm9/overlays/{10,20}/asm/*.s.patch` beside the static's
`arm9/asm/ndsrec_arm9_004.s.patch`). ov230 itself is never decoded, decrypted or
recompiled. Its load, unload and calls are removed, each four-byte BL
becomes two two-byte instructions (no guest address moves), and each
caller gets the outcome a genuine cartridge produces, read from its own
comparisons. `SHA256SUMS` pins each file, the same as
`ndsrec_arm9_004.s`.

| Caller (Black / White) | ov230 call | Substituted outcome |
| --- | --- | --- |
| `sub_02011D9C` (startup) | load, 0x021882A0 / 0x021882C0, 0x02188354 / 0x02188374, 0x02188390 / 0x021883B0, unload | as before: load/unload and third call omitted, first returns 0, second `~r1` |
| `ov10_0216EB98` / `ov10_0216EBB8` (after a battle, `ndsrec_ov010_005.s`) | load (l. 3379), 0x021882DC / 0x021882FC (l. 3393), 0x02188354 / 0x02188374 (l. 3426), 0x02188390 / 0x021883B0 (l. 3445), unload (l. 3476) | load/unload omitted; returns `~r1`, `~r1`, 0 |
| `ov20_021841C0` (field, `ndsrec_ov020_000.s`, both games) | load (l. 840), 0x021882A0 / 0x021882C0 (l. 870), 0x02188318 / 0x02188338 (l. 909), 0x021883CC / 0x021883EC (l. 929), unload (l. 956) | all omitted: the caller reads none of the results |

How `ov10_0216EB98`'s outcome is derived: r7 starts at 0x013A1AB5, which
is 0x1933 × 3191. At the state machine's exit, any r7 % 0x1933 other than
0 queues the `0x02011D35` VBlank task and bumps the counter at
0x0214624C. The startup caller registers the same task on its failure
path. The first two checks add 0 to r7 only when the result equals `~r1`
(otherwise 0x3D1 or 0x5D1). The third adds 0x9D when the result equals
`~r1` (key 0x0216F07D), so it returns 0.

Search: these three functions are the only code that loads overlay id
0xE6 (a literal 0xE6 before the overlay-load call), in both games. The
other `sub_02034AC4` callers in ov10 and ov20 load other overlays.

Result with the substitutions, both games, same save and schedule, run to
frame 30000 with A every 40 frames. After Bianca's win, overlay 92 loads
and the field returns (overlays 20/21/18/24/30 at frame 13920 in Black,
13960 in White) with no ov230 load. Cheren's battle follows: overlays
93-96 at 17042 / 17082, Oshawott sent out. Tepig (now Lv 6) wins it: Black
10/25, White "Player defeated Pkmn Trainer Cheren" at 3/24 (frame 20501).
The wrecked bedroom with the player free follows at frame 25001 (zone 391,
field_ready 1), and the run exits 0.

## Past the first battles: Nuvema to Striaton on the core (2026-10-08)

The core with the VCOUNT fix, the three ndsrec rules and the ov230
substitutions (main c2b11893e) was played on with tests/e2e's bots, each
leg a fresh np_gp process that CONTINUEs from the previous leg's in-game
save:

- **Bianca's and Cheren's battles.** Both are won by auto_battle on the
  probe's battle menu, then saved through the X menu. The menu now lists
  POKéMON first, so SAVE is at (192,94). Results: Black 17663 frames, White
  17339. This is milestone 02, which bw-script landed.
- **Home and the lab.** A fresh process CONTINUEs from that post-battle
  save. Then the stairs to 1F (zone 390) and Mom's scene, the door to Nuvema
  (389), Bianca's house (392), Cheren at the lab door, and Juniper in the
  lab (396): Pokédex, and the nickname typed by A. Back outside, Mom's Town
  Map, then a save. The end save has vars 0x4085 1, 0x4078 1, 0x4079 1,
  0x4080 2, 0x4081 2 and flags 0x217, 0x962, 0x2A7. Passes on both games.
- **Route 1 to Accumula.** Script 14 walks the player onto Route 1 and runs
  Juniper's Minccino-catches-Patrat battle. Then Bianca's comparison, and
  Accumula Town (397) with its banner, then a save. Passes on both games.
- **Accumula.** Juniper's Pokémon Center tour and the nurse's heal, then
  the Plasma speech and N's battle (Purrloin 7, won; Tepig Lv 8), then a
  save. Passes on both games.
- **Route 2 to Striaton (Black).** The gate (320), Route 2 (319): the
  Xtransceiver call and Mom, three wild battles fought, Bianca's battle
  won. Then the "Striaton City" banner and a save, 24512 frames. White
  blacked out in a wild battle on the way (it draws other encounters).
  The game's own whiteout returned the player to Accumula's Pokémon
  Center. That is play, not a port defect.

No new port defect appeared on this route. Two White runs elsewhere stopped
with np_gp's "DEFECT hang: run_frame did not return in 30s" while the
machine was at load 30-45 with under 1 GiB of free disk. Four parallel
reruns of the same leg and the same CONTINUE save at normal load gave the
same 22905 frames and the same end, with no stall. The watchdog measures
wall time, so a host stall under that much memory pressure explains it.
The C-Gear (Fennel, after the Dreamyard's Dream Mist) and its wireless and
IR features are not reached yet.

The dark teal striped bottom screen in the field before the C-Gear is
what the game draws, not a rendering gap. Checked at Black frame 25400, the
wrecked bedroom:

- DISPCNT_B is 0x00011710, so BG0, BG1, BG2 and OBJ are on, in mode 0.
- The sub-screen BGs are in VRAM H at 0x06200000.
- A separate Python render of those three layers matches the core's dump
  of that screen pixel for pixel. It was made from the game's own VRAM,
  sub palette and BGxCNT/offset registers: 0 of 49152 pixels differ
  (/tmp/bw2/sub/render.py).
- BG2 holds the stripes: 9 tiles, palettes 0 and 7.
- BG0 and BG1 maps are all tile 0. The character block already holds
  758 tiles, among them the C-Gear's hexagon icons. The C-Gear's screen
  is loaded but not shown until the game hands the C-Gear over (Fennel,
  scr 0020 @0x048F, Cmd19E 1).

## Run-ahead: Striaton to Nacrene, and the C-Gear (2026-10-08)

To look for port defects past the story chain, I made a scouting copy of
bw-script's 06 save. `np_save5 add-mon 500 50 53 15 89 157` added an Emboar
with Cut, and an in-game party SWITCH put it in front. add-mon takes moves
only by id; without them the Pokémon fights with Struggle. The game reads
the added Pokémon everywhere:

- POKEPARTY's count is 2 after CONTINUE.
- The X menu's Pokémon screen lists it.
- It is in the battle client's party.

With that party, bw-script's 07, 08 and 10 drafts and my legs for 09 ran
on the core. That covers the Trainers' School, the Dreamyard (Cut, Plasma,
Musharna), the gym's curtain switches (Cress, the Trio Badge), Fennel and
the C-Gear, Route 3, Wellspring Cave, and Nacrene: N, the museum and the
library gym's first bookshelf switches. No port defect appeared.

The C-Gear, checked on the core:

- It powers on with its start-up animation ("You see! The C-Gear was
  activated").
- Its IR, ONLINE and WIRELESS hexes, the clock and the battery draw on the
  sub screen.
- Touch works:
  - "?" opens the help (overlay 124); its PREV/NEXT/QUIT bar takes taps
    at y 168..192, as the hit table at 0x021D4BE4 says.
  - IR opens the IR menu, and Battle saves the game.
  - Battles for Two, then Single Battle, reaches the infrared search
    screen, which waits for a partner.
- The C-Gear's sub-apps answer touch only.
- `ov20_021841C0`, the substituted field caller of ov230, did not run
  anywhere on this path.
- The area-name banner's letters fly in and settle ("Dreamyard"); that is
  the game's animation, not garbled text.

## Run-ahead past Nacrene: seasons, rail maps, the Musical, the late game (2026-10-08)

`np_save5 set-location <save> <zone> <x> <y> <z>` (main 3c0812a8d)
moves a save's player, and the game CONTINUEs there. A zone that has its own
field-gimmick overlay must still be entered through its door. A save placed
directly in the Nimbasa or Icirrus gym (zones 63 and 114) loads without
that overlay (39 or 45). It aborts on the first gimmick call, an
"indirect branch to ov39_021F3D74 (not loaded)". Walking in from the city
loads the overlay, and a save made inside then CONTINUEs cleanly. That is a
limit of the tool, not of the port.

All runs below are Black, on the core built from main 21cf05779, with the
boosted scouting party. None found a port defect:

- **Seasons.** Settled below ("Seasons, from the code"): the season
  changes at a map load, never on CONTINUE. Seasons on the port are correct.
- **Skyarrow Bridge (249) and Castelia (28)** are rail maps: the probe's
  field_ready stays 0 on them, and walk_to does not apply. Holding a key
  works. From the gate 251, holding UP (B to run) crosses the bridge to 252
  in about 3240 frames; the deck's y rises to 31 and falls again. From
  252, holding LEFT follows Castelia's street; UP at a door enters a normal
  building map.
- **The Musical Theater** works: the Prop Case, the dress-up screen (drag
  by touch), "Is this look OK?", and the dressing room.
- **Late-game areas**, each entered by CONTINUE at a door or exit, then
  walked: Black City, Nimbasa and its gym's rollercoaster, Driftveil and
  the drawbridge, Mistralton and its gym, Icirrus and its gym's ice,
  Opelucid, the Pokémon League, Desert Resort, Relic Castle with the "so
  much sand" gate, Cold Storage, Chargestone Cave, Twist Mountain,
  Dragonspiral Tower, and N's Castle with its "Those in accord with Fate"
  text. All render and take input. The story events there (Reshiram,
  the credits) need the chain's flags and were not reached.

White parity: the White save of bw-script's milestone 10, moved by
`set-location`, crosses Skyarrow Bridge from gate 251 (hold UP) and follows
Castelia's street from gate 252 (hold LEFT). It rides the Nimbasa gym's
coaster after entering through the door, and it shows N's Castle's "Those in
accord with Fate" line. Every run returned rc 0. On CONTINUE, a save that
has the C-Gear asks "Launch C-Gear communications?", and that prompt takes
the A presses before the walk starts.

### Seasons, from the code (Black; White matches)

- `sub_0202E888` computes the season from the clock. It calls
  `sub_0203F4E4`, which copies out the RTC date, and returns
  `(month - 1) & 3`: 0 spring (January, May, September), 1 summer, 2 autumn,
  3 winter.
- The current season is a byte at GAMEDATA+0x1C4: getter `sub_02012984`,
  setter `sub_0201298C`, called through `sub_0202E8CC`.
  - GAMEDATA's constructor (`sub_020124F4`) seeds it from the clock.
  - Loading a save then restores the saved value, so CONTINUE always keeps
    the season the save was made in.
- `sub_0202E848(gamedata, &cur, &new)` decides whether the season changes:
  - If the lock byte GAMEDATA+0x1CC is set (`sub_02012B70`; `ov20` sets it
    to 1 and clears it again), nothing changes.
  - Otherwise it reports a change when the stored season differs from the
    clock's.
- `ov20_0218403C` runs on a map change. If a change is reported and the
  destination zone takes seasons (`sub_020140D8` on the zone header), it
  records the old and new values.
  - Overlay 20 then commits the new value (`sub_0202E8CC`) and plays the
    season card, stepping through every season in between (`sub_0202E8A4`
    returns `(s + 1) % 4`).
  - This matches Bulbapedia's description: seasons change on leaving a
    building or crossing a loading zone.

The earlier identical frames were therefore correct: those runs CONTINUEd
and stood still. Here, the player starts in an autumn save inside the house
in Nuvema (zone 390, March 2009 clock) and walks out:

| `--rtc` | card | Nuvema afterwards |
| --- | --- | --- |
| January 2011 | "Winter", then "Spring" | spring green |
| February | "Winter", "Spring", "Summer" | summer teal |
| March | none (still autumn) | autumn |
| April | "Winter" | winter grey |

Frames: `/tmp/bw2/seasons_exit.png` (cards and towns),
`/tmp/bw2/sx_jan_splash.png` (January's two cards). Icirrus in April, on
leaving its Pokémon Center (zone 115), has snow on the ground, snow-laden
trees and falling snow; in March it shows autumn's orange trees
(`/tmp/bw2/icirrus_seasons.png`). The port's RTC model (`pc_rtc.c`, the
`--rtc` seconds) needed no change.

How to check a season: CONTINUE with `--rtc` set to the month you want, from
a save made indoors, then walk out.

## Link play: Black and White stations over np_host_net (2026-10-09)

Two np_headless stations, Black's A and White's B, run in lockstep
(`--lockstep`, `--net-id`). They meet in the Union Room, trade, and battle.
The wireless model is the one D/P/Pt already use and needed no change:
`games/platinum/pc/src/pc_wm.c`, the ARM7 WMSP half behind PXI tag 10. The
BW core builds it with `PC_GAME_DP`, through `games/diamond/pc/mk/host.mk`.
Black and White link TWL-SDK 5's ARM9 WM library (`WMi_StartMP` at
0x0208FA28, `ndsrec_arm9_022.s`). That library sends the same requests and
decodes the same callback records the model writes. No shared wireless code
was touched.

What runs, in order:

- **The C-Gear** beacons as a WM parent after "Launch C-Gear
  communications?" YES: ggid 0x1380, channels 1/7/13, as `PC_WM_TRACE=1`
  shows.
- **The Union Room.** The counter in Striaton's Pokémon Center (zone 8, the
  attendant at (4,4) up the tier, y=3) needs flag 0x73 (scr 0855 script 2).
  After the first-visit title, "DS Wireless Communications will be
  launched" and the save, both stations are in zone 422.
  - Each station sees the other's avatar.
  - Both chat logs read "LINKA: I've entered the Union Room." and "LINKB:
    I've entered the Union Room."
  - B's avatar stands at (13,10). A walks to (13,11), presses A, and gets
    "Talking to LINKB..." while B shows "Hello!". A's menu then offers
    Greet, Battle, Trade, Draw, Spin and Cancel.
- **The trade** is all touch.
  - Each side offers up to three Pokémon, and each sees the other's offer
    on the top screen.
  - Two confirmations follow, then each picks the partner's Pokémon and
    taps TRADE twice.
  - The animation runs on both stations ("LINKB sent over ..."), the game
    saves both, and the parties hold the swapped Pokémon.
- **The battle**: Battle > Battles for two > Single Battle, No
  Restrictions > Confirm; B accepts.
  - Both are moved to the battle room (zone 150, the referee's "Please take
    your designated position and start the battle.").
  - The 1v1 positions are (3,5) and (9,5). They come from overlay 19's u16
    table at 0x02180794: {3,5, 9,5}, then the 2v2 {3,4, 9,6, 3,6, 9,4}. That
    is BW's counterpart of Platinum's `sub_020590C4` grid
    (`comm_player_manager.c`).
  - Standing there opens team selection. It shows the opponent's party, with
    ENTER and CONFIRM.
  - The battle runs with moves on both stations ("Emboar used Earthquake!" /
    "The foe's Emboar used Earthquake!").
  - It ends on the shared result screen (WIN for the winner on both) or on
    "The match was forfeited.", and both stations return to the battle room.

Tests: `tests/link/run_link_tests.py --game black:white` runs `bw_trade` and
`bw_battle` (about two minutes each), and `linkpair.py` takes `black` and
`white` stations.
- Black/White have no lab, so `linkpair.mint_bw` builds each station's save
  from the e2e chain's milestone 10 (`build/e2e/<game>/10-.../end.sav`, or
  `NP_BW_LINK_BASE_<GAME>`):
  1. `np_save5` renames the save (LINKA / LINKB), sets new IDs and places
     the player outside the Pokémon Center door.
  2. The game walks in and saves there (`schedules/bw-pc-save.sched`), so the
     save holds the Center's own objects.
  3. `np_save5` then moves the player in front of the Union Room counter.
- `bw_trade` checks the swap with `np_save5`: A's party slot 1 against B's
  slot 4, relative to the minted parties.
- `bw_battle` checks that both stations enter the battle, leave it, and
  return to zone 150. Turn 1 trades a move; on turn 2 A forfeits. A forfeit
  is used because its timing does not depend on the chain's levels.
- The schedules are timed against milestone 10's parties (two Tepig turns).
  A chain change that alters those needs the battle taps retimed.

Frames (A | B pairs), in `/tmp/bw2/link/report/`:
1. `1-union-room.png`: both in the room.
2. `2-trade-boosted.png`: the trade animation and "sent over".
3. `3-trade-regression.png`: the regression trade, Patrat ↔ Tepig.
4. `4-battle-ko-win.png`: a KO battle with the WIN screen on both.
5. `5-battle-regression.png`: the regression battle, Ember / critical
   Tackle / forfeit.
6. `6-battle-room-team-select.png`: the battle room and team selection.

### The post-game trade: trade evolution and Landorus (2026-10-10)

`run_link_tests.py --game black:white bw_trade_landorus` trades between the chain's own players after the credits.

- **The stations.** Black's and White's milestone-85 end saves, each with its own roamer caught (`linkpair.BW_CHAIN`
  `landorus`). They keep the chain's names and IDs, because the shrine checks the OT.
  - Black's party slot 4 becomes a lv 30 Boldore (np_save5 set-mon, as the e2e boosts' party-set).
  - At Striaton's PC, Black puts Sawk in BOX 1 and takes out its Tornadus, which 85 caught into BOX 1 because the
    party was full.
  - The schedules were recorded by `linkbot.py` from `scenarios/bw-trade-landorus.json`.
- **Trade evolution.** Black offers the Boldore and White offers BOX 1's Thundurus. On White's station the Boldore
  evolves by the trade: "What? Boldore is evolving!", then "Congratulations! Your Boldore evolved into Gigalith!".
  White's save holds the Gigalith with the Boldore's PID.
- **The saves.** The game saves both stations after the animation, and the case checks both with np_save5:
  - Black: Thundurus with White's TID, and Tornadus with its own.
  - White: the Gigalith, and no Thundurus or Boldore left.
- **Handover to e2e.** A passing case leaves both saves as `build/e2e/<game>/link-bw_trade_landorus/end.sav`.
- **Landorus.**
  - tests/e2e/black/88 CONTINUEs Black's save. The Union Room's own return is what CONTINUE takes (flag 0x966 and
    var 0x4041 = 1, scr 0855). It runs only in Striaton's Pokemon Center: set-location to Route 14 left the player
    held there. So 88 shows the party (both roamers) and saves in the Center.
  - black/89 moves to Route 14 below the Abundant Shrine and walks in. The shrine's level script shows the children
    (Tornadus caught, 0x40CE 1), and their trigger plays their scene. The shrine's bg (scr 0752 script 2) counts
    Tornadus with the player's OT and Thundurus, and calls Landorus (lv 70), which is caught with a Master Ball.
- **White's Landorus.** `bw_trade_landorus_white` swaps the sides (`linkpair.BW_CHAIN` `landorus-white`, both 85
  saves as they are): White takes Thundurus out of BOX 1, Black trades its BOX 1 Tornadus for White's party slot 4
  (Tepig). White's save then holds Tornadus with Black's TID and its own Thundurus; white/88 and white/89 continue it
  as black/88-89 do, and Landorus is caught on White.
- **Not covered here.** Karrablast and Shelmet need both in the chain's saves; neither is.

## The late game on the core: N's Castle's VRAM hand-over (2026-10-09)

The story chain's scouts (tests/e2e/black/22-33) ran the late game on synthesized saves. N's Castle's throne room
(zone 278, scr 0556 script 3) and the ending after Ghetsis stopped the core on armrec's VRAM overlap trap (banks D
and I both sub OBJ, VRAMCNT 83 8B 80 84 83 81 82 80 82). The SDK's bank setters store the new bank before moving the
old one to LCDC, and the recompiled SDK's per-store VRAMCNT hook sees that moment; armrec now models overlapping
banks as the console does (reads OR, writes to both; tests/e2e/DEFECTS.md has the details). No new overlay-230
caller appeared in the throne room, the capture, N's battle, Ghetsis's battle or the farewell.

Rails, for the e2e probe: the Icirrus Gym's ramp and spin tiles (zone 114, Cmd195) and Victory Road's outdoor zone
214 are rails like Skyarrow Bridge's: the player's MMDL has status bit 0x2000 and sits 0x20 fx off the tile centre,
so field_ready stays 0 there (the `rail` bot, and B/W `slide`'s held press for the gym's ramps). A rail map's tables
are in RAM while it is loaded (Dragonspiral 211: the player's rail work at MMDL +0x94 leads to a header with the
point and line arrays: points 112 bytes, u32 lines[4], keys[4] (1 U, 2 R, 3 D, 4 L), fx32 pos at +0x30; lines 72
bytes, start and end point, key, position, camera, length).

The story chain's last milestone is one per version: tests/e2e/black/32-n-castle-reshiram-ghetsis-credits and
tests/e2e/white/32-n-castle-zekrom-ghetsis-credits run from the throne room's entrance to the game's own save after
the credits. scr 0556 is one uninterrupted chain from talking to the legendary (script 8) through the capture, N's
battle, Ghetsis's battle and N's farewell to Cmd156 (the ending); 0x40B6 = 5 is set inside it and never seen in the
field, so the former split (32 ending at 0x40B6 = 5, 33 for Ghetsis) cannot be played, and 33-ghetsis-ending is
folded into 32. On the fixed core the ending runs to the game's save (0x40B6 5, 0x40A2 3 from scr 0866 @0x0457,
flag 0x133) and OS_ResetSystem, about 17k frames after Ghetsis's win; CONTINUE from that save starts in the
player's bedroom (zone 391).

Victory Road (214), routed for milestone 28 (2026-10-10): the player's rail state is u16 line at MMDL +0x98 and s16
side, front at +0x9C (the rail work at +0x94 holds at +4 a pointer to the current line's record, so the line array starts 72 x line before it and
the points end where it starts; names RE_LINE_n / RE_POINT_n). A point's pos is fx32 world units (/0x10000 = tiles,
the probe's x/z). 214 has 76 points and 65 lines in seven terraces (y 0, 5, 10, 15, 20, 25, 30) that share no line:
caves join them upward, cliff slides downward. A rail map's warps and triggers are (line, front, side, width) in
zone_event's x, y, z fields; a slide is a down-hold past side +2 at a slope (line 15 front 2, line 44 front 3,
lines 54-57 front 0-3); side +2 elsewhere is a fence or a cliff. Trainers whose sight stops the player on the rails
are fought by `rail` with `on_battle = "fight"`.

## Poké Transfer (design note 2026-10-09; done 2026-10-10)

**Status: done in the core.** A Black lab sends the child to a second station with a Platinum or HeartGold card. The
six are caught and moved, and both saves verify after the games' own writes; see "Built" below. The shell runs the
second station with `--poke-transfer`. Not covered:
- Diamond, Pearl and SoulSilver cards (not run). D/P use the child's `mb_data_main` layout, Platinum `mb_data_pt` and
  HG/SS `mb_data_gs`; SoulSilver shares HeartGold's.
- White's lab. The child program is byte-identical to Black's, but no White lab schedule has been recorded.
- An app run. The app case `n2_poke_transfer` (tests/mac/feature_matrix.py) needs an app built with the poketransfer
  guest; the stations there are free-running, so it checks the download and boot, not the minigame.
- Paths not tested: the timer running out with some but not all caught, held items and eggs (msg 313 #32 asks
  for none held), boxes other than box 1, and a second round ("use Poké Transfer again?" YES). The timer running
  out with none caught was seen: the child asks "again?", and NO ends with nothing written.
- The firmware's RSA check of the download (the hash check against the recompiled image stands in for it).
- The Relocator (`dl_rom/child2_r_eng.srl`) is out of scope.

The design note follows as written before the work. It was read from the Black ROM (White matches by name; offsets
below are Black's) with `tests/e2e/tools/bw_script.py`, `tools/ndsrec/nds.py` and the recompiler's generated
assembly.

**The lab and its gate.** The Poké Transfer Lab is zone 381, entered from Route 15 (zone 378 warp 2 at
(608,425)). Its counter scientist (zone_event 381 object 0, scr 0878 script 1, text msg 313) checks only:
- six free PC box slots: Cmd1F0 5, 0 then Cmd121 var 0x8021, 5, and "unless var 0x8021 >= 6" leads to msg 313 #2,
  "make room for six Pokémon";
- wireless on: Cmd13B, else std 2005 (msg 158 #22 "Wireless communications are turned OFF");
- a save: std 2003.

  It then runs Cmd1C0 and reads the outcome with Cmd1C1 into var 0x8024. Outcome 5 means "I put the Pokémon you
  caught in your PC Box" (msg 313 #15); 0 and 1 count as successes. Cmd1C0's handler, ov21_021CC570, starts the
  process whose data is at 0x021F6D78 in overlay 107 (`sub_02014840(..., 0x6B, 0x021F6D78)`).

  No Pokédex, badge or Hall of Fame check appears in the lab's scripts. Access is reaching Route 15, through the
  Black Gate (zone 379, from Black City) or the Bridge Gate (zone 380, from the Marvelous Bridge, zone 263). Route
  15, Route 16 and the Marvelous Bridge each exist twice in the zone table (378/313, 383/314, 263/303).
  [INFERENCE] These are story-dependent variants, and Route 15 opens only after the Hall of Fame. Not traced to the
  variable that selects them. The chain's saves stop at milestone 27 (8 badges), so testing the lab needs a later
  chain save or `np_save5 set-location 378 608 0 426`.

**The parent: overlay 107.**
- Location: RAM 0x021EE740, 0x8760 bytes, 202 functions (`ndsrec_ov107_000.s`). Strings: `mb_parent_sys.c`, `mbp.c`.
- It names the child files `/dl_rom/child_r_eng.srl`, `/dl_rom/child2_r_eng.srl` and their icons
  (`/dl_rom/icon_{b,w}.{char,plt}`).
- Its only overlay loads are 14 and 21 (`sub_02034AC4` with 0xE and 0x15). **Overlay 230 is not on this path.**
  Its known callers (startup, `ov10_0216EB98` after a battle, `ov20_021841C0` in the field) are the three this
  plan already substitutes, and no transfer code calls it.

**The Download Play child: `dl_rom/child_r_eng.srl`** (file 478, 1,369,088 bytes in the ROM, byte-identical in
Black and White; White's overlay 107 sits at 0x021EE760, the same size).
- Header: an NTR SRL, title `SYACHI_MB`, game code NTRJ, unit code 2 (DSi-enhanced). The ARM9i (7,816 bytes) and
  ARM7i (291,064 bytes) modules are ignored in NTR mode, which is how a Download Play child runs.
- ARM9 static: RAM 0x02004000, entry 0x02004850. It is BLZ-compressed (0x888A8 bytes) and expands to 1,050,464 bytes
  with three autoloads. BSS runs to 0x0213F2A0. SDK version word 0x0503757C, the TWL-SDK 5 family Black and White
  are built with.
- ARM7: 167,812 bytes at 0x02380000.
- No overlays and no FAT. The image's used size is 765,952 bytes, followed by the `ac` block of a Download Play
  RSA signature.
- Its sources, from the strings:
  - `mb_child_sys.c`, `mb_comm_sys.c`;
  - `mb_sel_poke.c` (choosing the six);
  - `mb_cap_{obj,poke,down,ball,effect,demo}.c` (the capture minigame);
  - `mb_data_main.c`, `mb_data_pt.c`, `mb_data_gs.c` (the Gen 4 save layouts: D/P in main, then Platinum and
    HG/SS);
  - the GF library (`gfl_use.c`, `net_*.c`, `wih.c`).
- It reads the **inserted Gen 4 card**:
  - an archive `child_rom` with `child_rom:/poketool/icongra/poke_icon.narc`, `pl_poke_icon.narc` and
    `child_rom:/a/0/2/0`;
  - the backup library (`[SDK+NINTENDO:BACKUP]`);
  - a list of game codes, `ADAEAPAPCPUEIPKEIPGP` (Diamond, Pearl, Platinum, HeartGold, SoulSilver).
- Seven 4 KiB blocks at 0x02067000-0x0206F000 are near 8 bits/byte of entropy: [INFERENCE] compressed embedded
  graphics, since the child has no filesystem. Not checked.
- `child2_r_eng.srl` (file 477) has the same shape, but `mb_movie_sys.c` takes the place of the selection and
  capture sources. [INFERENCE] It is the movie-legendary "Relocator", opened by a Wonder Card. Out of scope here.

**What our port would need.**
1. **The child as a guest.** Run `child_r_eng.srl` through ndsrec the way Black and White's static is: TWL-SDK 5 in
   NTR mode, one static module of about 1 MiB, no overlays, much smaller than B/W's 35,941 functions. Register it
   as its own core guest, with the host layer the B/W core already uses. The SRL bytes come from the player's B/W
   ROM at run time, so nothing new ships.
2. **A second station.** The child runs on the other DS. In our terms that is a second instance, an np_headless
   `--lockstep` pair or a second shell window, as the link scenarios already pair stations. The child station
   boots from the SRL directly. The firmware's download and RSA check are the console's, not the game's.
3. **Multiboot in `pc_wm.c`.** This is the main unknown.
   - The parent's MB library sends the image to a child over WM. Today the model knows only an MB-flag beacon
     (`WM_ATTR_FLAG_MB`, `childMaxSize`); it has no MB download protocol.
   - Either `pc_wm.c` answers the download as a firmware child would (request, block acks, done), or the host has
     to satisfy the parent's MB state machine some other way, after reading what it waits for.
   - [INFERENCE] Once booted, the child rejoins the parent as an ordinary WM child (GF's `wih.c` / `net_whpipe.c`
     on both sides), using the parent parameters the firmware leaves for a booted child. The model already carries
     that part for the Union Room.
4. **A foreign slot-1 card.** The child station needs the player's Gen 4 ROM and save as its inserted card:
   - card-bus ROM reads served from that ROM image, with its header and game code (`pc_card_rom.c` serves only the
     guest's own ROM today);
   - the 512 KiB backup over SPI from that game's slot file, with D/P's backup model reused.
   - Transfer takes the six out of the Gen 4 boxes, so the child writes the Gen 4 save back. The shell must store
     it to that game's slot, and the test must run on a copy.
5. **The shell.** Pick the Gen 4 slot, show the child station's screens (the capture minigame is touch), and hand
   the two saves back.

**Tests.**
- A link scenario like `hgss_trade`: a B/W station at the lab with six free box slots, and a child station with a
  D/P/Pt/HG/SS save holding six transferable Pokémon (no held items, as msg 313 #32 asks).
- Check the six arrived in the B/W boxes (np_save5) and left the Gen 4 boxes (np_save4).

**Effort.** About 2.5-4 weeks of agent time:
- child guest through ndsrec and its first boot: 3-5 days;
- foreign card ROM and backup: 2-3 days;
- multiboot in `pc_wm.c`: 5-10 days, the widest range;
- the capture minigame, results and save write-back on two stations: 2-4 days;
- shell UX and app evidence: 2-3 days.

**Risks.**
- The MB download protocol is undocumented in our tree; the ARM7 WM firmware side is what we would model.
- The parent may check the child's MAC or GGID, or a timeout, in ways only a run shows.
- The child's ARM9 may do what B/W's needed rules for (VCOUNT, overlapping VRAM, ndsrec rules). Its high-entropy
  blocks are unverified.
- The Gen 4 card read may expect card states the host has never modelled (pulled-out checks, a second card's
  KEY1/KEY2 mode).
- Getting to Route 15 needs a post-game save the chain does not have yet.
- Transfer is destructive on the Gen 4 save: a bug loses Pokémon. Write-back must go to a copy until verified.
- Overlay 230 stays opaque. Nothing found here calls it, but a run would need `PC_TRACE_OVERLAYS=1` to confirm no
  new caller.

### Built: the child station, its Download Play, the transfer and the shell (2026-10-10)

**The child as a guest.** `games/ndsrec` with `VER=poketransfer BW_ROM=<Black or White ROM>` extracts
`dl_rom/child_r_eng.srl` (`ndsrec.py extract`, into build/) and recompiles it like Black and White's static:
- 2,961 functions (2,937 ARM, 24 Thumb); armrec translates every one it is given (2,882) cleanly;
- 90 of 135 primitives matched (B/W's 102, less GX/MTX helpers the child does not link);
- `OS_GetIrqFunction`, absent from the child, comes from `pc/src/pc_pt_child.c` over the child's own IRQ tables.

The game id is `poketransfer` (`NP_GAME_POKETRANSFER`, core and tools). The runtime's ROM and save are the Gen 4
card in the child's slot 1. `pc_card_rom.c` under `PC_MB_CHILD` puts that card's header at HW_CARD_ROM_HEADER, where
a multiboot child finds the inserted card (CARD_Init copies a program's own header there only on a card boot).

**The firmware's Download Play client.** On a console the DS firmware receives the child and boots it. Here that is
`pc/src/pc_pt_dlplay.c`, run by `pc_main.c` before NitroMain:
- It is NitroSDK 4.2's own multiboot child (`libraries/mb`) over its ARM9 WM library, both compiled into the image
  (`PTFW_SRCS`), talking to `pc_wm.c` like any WM library. It scans, takes the first game whose beacon validates,
  connects, requests the file and receives every block.
- The firmware is NITRO code, so it receives the ROM header at the NITRO HW_ROM_HEADER_BUF (0x027FFE00), which is
  what B/W's parent sends; the image's TWL-SDK map has it at 0x02FFFE00, the same byte on a 4 MB DS
  (`pc_pt_fw_mb.h`).
- Nothing boots that was not received. The header, the ARM9 static and the ARM7 static are checked against the
  SHA-1s of the image this build recompiled (`ndsrec.py mbimage-c`); a mismatch stops the run. The firmware's
  check of the image's RSA signature is not modelled: only this build's recompiled code can run, so the hash check
  is what matters.
- Then crt0's steps: the ARM9 static is BLZ-decompressed in place and must hash to the recompiled image; each
  autoload block is copied to its place and its bss cleared; then the static's bss is cleared (it begins on the
  autoload bytes).
- The boot state a child reads: MBParam at HW_WM_BOOT_BUF (MB_TYPE_MULTIBOOT and the parent's BSS description, which
  the child reconnects to) and the parent's user parameter at HW_DOWNLOAD_PARAMETER. The display line goes back to
  0, because the child's OS_Init waits for VCOUNT 0.

Two fixes outside the child station came with it:
- `pc_wm.c`: a parent's MPEND_IND now also carries, in its receive buffer, what each child sent since the previous
  one. The multiboot parent reads its children only there (`mb_parent.c` MBi_CommParentRecvData). Every other
  caller reads the port records, as before.
- `tools/ndsrec/discover.py`: a bounded ARM switch (`cmp rN, #K; addls pc, pc, rN, lsl #2`) may hold returns
  (`ldmia sp!, {..., pc}`) among its branches. Discovery stopped at the first one, so the rest of the table and its
  cases were not code. The child's sub_0200B8C8 reached one at run time. On Black the fix adds 226 instructions and
  no functions.

**What a run shows** (`tests/poketransfer/run_tests.py link`):
- Station A: Black with bw-chain's milestone 34 save, at the lab counter (`pt-parent-black.sched`). It shows the
  Download Play screen by frame 7300.
- Station B: the Platinum card and a save with six Pokémon in box 1. The client connects at frame 7447, and the
  download runs from 7470 to 10422, about 49 seconds, as a real one takes. The image verifies and boots at 10428.
- The child reconnects to A as its own program, identifies the card (state 6 of its main state machine, 0x02013D20,
  sorts the game code: D/P, Pt or HG/SS) and reads the save and the card's icon archive. By frame 11200 it shows
  BOX 1 with the six icons and "Please choose the six Pokémon to transfer". A says "Please select Pokémon in the
  other DS system."
- With a save that has fewer than six boxed Pokémon, the child says "There aren't six Pokémon in the PC Boxes for
  Poké Transfer to catch." It does this with a Platinum card and with a HeartGold card.
- `link` checks the steps up to here and that the card save is unchanged.

**The transfer** (`run_tests.py transfer_platinum`, `transfer_heartgold`, 2026-10-10). The child is played with the
stylus. The six icons are dragged into the frame on the right and YES confirms. Then comes the capture minigame
(mb_cap_*): the ball is pulled back on the bottom screen's slingshot and lands where the reticle shows, about 24
frames after release. Pulling (dx, dy) from (128, 85) lands near (128 - 2.43 dx, 201 - 2.48 dy). The Pokémon hop
between the bushes and hide. A ball landing on a hidden one flushes it out, and one landing on a Pokémon in the open
catches it. With all six caught: "Finish!", "Pokémon CANNOT be returned once they transfer." (tapped), "Transfer
these Pokémon?" YES.
- The child then saves the card ("Saving... Don't turn off the power."). Its backup writes go through the card model
  that already served its reads.
- Black saves the six into its boxes, and the lab says "Poké Transfer will end."
- Both saves verify after the games' own writes. Black's box 1 holds its Pansage and the six, with the same
  personality values they had in the Gen 4 boxes. The card save's boxes are empty.
- It works with a Platinum card (milestone 25's save) and with a HeartGold card (milestone 04's save): Sentret,
  Hoothoot, Pidgey, Rattata, Caterpie and Weedle, with HeartGold's icons.

The two stations run in lockstep, so a recorded touch schedule repeats exactly. `tests/poketransfer/catchbot.py`
records `pt-child-<card>.sched` greedily:
- It plays from np_headless `--fork-at` checkpoints: a second's tries take seconds, not the 13000-frame walk to the
  minigame.
- It records a stretch and finds what moved against the field's background. It tries shots landing where something
  will be, and keeps a shot when the icon bar shows one more Poké Ball. When nothing in the stretch is caught, it
  keeps a flush shot.
- Platinum needed 11 shots (released 13347 to 14455), HeartGold 10 (13663 to 15180), 10-40 minutes each.
- A schedule fits only the saves it was recorded with; the test checks their SHA-1s and names the tool otherwise.

**The shell.** `nativeplat --poke-transfer --game <diamond|pearl|platinum|heartgold|soulsilver> [--slot S]`, or a Gen 4
slot's *Poke Transfer station*, makes the window the second DS:
- The window runs the `poketransfer` core with that game's cartridge and save slot as the card (`app->card_game`:
  whose ROM and slot a session uses). The child's writes go to that slot.
- Local wireless must be on, since the lab is in another window or on another machine. Carts and content packages
  stay the card game's.
- The packages build the guest in when `ndsrec-poketransfer.wasm` exists.

## The Musical show: the ARM7's VBlank count (2026-10-10)

The Musical's show (overlays 132 and 112, after the backstage zone 78) stayed on its closed curtain. Its main
function, ov132_021F879C, steps the show's scripts once for each frame that has passed. It counts those frames by
the change in `OS_GetVBlankCount()`, the shared word HW_VBLANK_COUNT_BUF (0x02FFFC3C on TWL-SDK). On a console, the
ARM7's VBlank interrupt increments that word (NitroSDK os_irqTable.c, OSi_IrqVBlank). No host-side ARM7 code did, so
the count stayed 0 and the show never took a step. OS_Halt (pc_os_lite.c) now increments it on each delivered VBlank,
before the ARM9's handler. The other ports have the same counter at 0x027FFC3C, and it counts there too now.
75-musical passes on Black and White, with the full show (tests/e2e/DEFECTS.md).

How it was found, a method that works for any silent wait:
- run macOS `sample <np_gp pid>` at the hang. The recompiled functions keep their guest names (w2c_black_ov132_...).
- attach with `lldb -p` while np_gp is running frames, not while it is blocked reading stdin. Break on the function:
  w1 is the guest r0.
- `peek` that work area through np_gp's serve protocol.

## The chain and the side systems on the VBlank-count core (2026-10-10)

With the VBlank count running, the game's luck comes out differently on build/core-bwm2: which wild Pokemon a
walk meets, where the roamers go, and the day care's egg rolls. The whole story chain (01-32 to the credits, 34
after them) and every side system in black/systems.txt and white/systems.txt were run again on that core, on both
games, and pass. The estimates and `[run] frames` in the milestones are this core's measurements.

What the new luck needed:
- 26 (Black): Bianca's battle on Route 8 whited out the chained party. `boost.recipe` raises the levels only.
- 64: a wild Emolga on Route 18 that RUN could not leave took 6100 frames, more than talk_to's budget for the
  HM05 ball. The step now has `max = 20000`.
- 67: one fixed delay after the Bag's USE no longer met the bite on Black. The rod is registered on Y and cast
  again until a battle starts.
- 68: 240 legs of 14 steps laid no egg on Black. The same number of legs at 13 steps lays one on both games.
- 70: a lv 15 Lillipup leading lost to Route 16's lv 20 wild Pokemon, and fleeing with it let a Liepard faint it.
  The Lillipup now sits on the bench holding an Exp. Share, and the lead Darmanitan wins the battles. Recipes gain
  `party-item` for B/W (np_save5 set-held). The sheet ends on the party screen, which shows the Herdier.
- 77: the money check is gone. The amount depends on the luck.
- 85: the fixed number of fled battles before the roamer no longer holds. The new `hunt` bot paces the grass and
  flees every battle until the one against the roamer.

Harness changes found on the way:
- auto_battle could spin without a frame on a moves cursor its key table has no way out of (the back button). It
  now presses up and lets frames pass.
- The bots' snaps (auto_battle's first menu, a fish on the hook) go on the contact sheet in step order, labelled by
  their step or by the step's `snap` string, not at the end labelled by frame. A run that ends the game labels its
  last frame `reset`.

The contact sheets were also gone over for black or wrongly labelled shots:
- A `shot` on the step that walks into a trainer battle caught the black or white transition. 22 of them became
  `snap = "<name>-battle"` on the auto_battle after it.
- Shots named after a person who had already left became `after-...`.
- 06's Route 2 shot moves off the map's black fade-in, to the step after it.
- 66 and 34 take Looker's shot during his scene, and 66 the Nuvema scene's first line.
- 68 shows the Day-Care Man's egg offer.
- 73 shows the seventh battle and the clerk's line.
- 75 shows the curtain open, the performers, and the results.
- 32, 80-85 and 87 take the "caught" shot on the Pokedex line.
