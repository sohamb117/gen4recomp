# Pokémon Black / White: plan

There is no usable Black/White decompilation (squiddonaut/pokeblack is a
fraction of a percent C, Black only, and unlicensed, so it is neither a
source nor something to vendor). Black/White therefore go the way this
repository proved on Diamond: pure static recompilation of the cartridge
image (`tools/ndsrec` + armrec), linked with the shared machine model
(games/platinum/pc, tools/armrec/armrec_rt.c). This file says what is
already done, what is different about a TWL-SDK 5.3 hybrid cartridge, and
the exact steps.

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
