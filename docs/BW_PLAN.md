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
- The new game plays to the first save (`tests/dp/first_save.sh`): the
  stored save holds the trainer NATIVE, CONTINUE loads it and saves again.
  No run-time dispatch miss in the 12,600 + 3,400 frames (a miss aborts and
  names the address). Audio is identical to the decompilation core's through
  frame 12,300, and so are the frames up to 9,001.
- One divergence: from the bedroom on (frame 10,001), the player's avatar
  (a 16x23 px billboard at the screen centre, 314 pixels) is not drawn;
  everything else on both screens is identical. Not yet diagnosed.

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
