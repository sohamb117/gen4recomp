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
| discovery | 35,993 functions (28,831 Thumb), 1,290,366 instructions, 30 undecodable words | 35,990 functions |

For scale, Diamond is 26,518 functions and 1,045,668 instructions, all of
which armrec translates; Black/White are about 24% larger.

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

**Signatures.** `tools/ndsrec/sigs.py` learns the 131 host primitives from
Diamond (NitroSDK 3.2) and places them in Diamond 131/131 and in
Platinum's ROM 112/131 (0 wrong), using byte patterns, normalised
instruction sequences that survive toolchain differences, callers and
literal pools. TWL-SDK 5.3 code differs more: expect the byte and sequence
stages to place fewer functions, and the caller/call-structure stages and
hand-checked anchors to carry more. Where a primitive cannot be placed, the
fallback is to leave it recompiled: most of the host's overrides exist for
speed or for hardware the machine model also emulates at the register
level, and only a few (thread context switch, IRQ entry, halt, cache and
protection-unit control, card ROM reads) are mandatory.

## Steps, now that the ROMs are here

1. Front end on both ROMs (done): `ndsrec.py info -v <rom>` (table above).
2. Emit and translate, to find what armrec does not yet accept in TWL-SDK
   code:
   `python3 tools/ndsrec/ndsrec.py emit roms/<Black>.nds --out build/ndsrec/black`
   then `armrec.py --scan --decomp-state /dev/null` over `arm9/**/*.s`
   (as in games/ndsrec/pc/mk/ndsrec.mk). Fix constructs until 100%.
3. Primitive map: `sigs.py match roms/<Black>.nds build/ndsrec/sigdb.json
   --out build/ndsrec/black.syms --debug`; place the misses from their
   callers or by hand-reading the recompiled SDK (crt0's literal pool
   already gives NitroMain), and record any rule learned in sigs.py.
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
