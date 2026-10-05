# RSE (Ruby/Sapphire/Emerald) handoff

Paused by re-prioritisation (E2E D/P/Pt, then B/W, then HGSS, then RSE).
Nothing is integrated into `main`. The branch `rse` (worktree
`../nativeplat-rse`, seeded with `tools/worktree_seed.sh`) holds only this note.

## State

- Not started: vendoring, `tools/rom_build.sh emerald|ruby|sapphire`, the GBA
  layer, the wasm build, runtime/headless/shell changes.
- Available inputs (outside git, in the main checkout's `.cache`, linked into
  every worktree):
  - `.cache/gba/pokeemerald`: pret/pokeemerald at `731ad5b`, a matching build
    (`pokeemerald.gba` SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7, plus
    `pokeemerald.elf` and `pokeemerald.map`), built in the
    `nativeplat-romtools:1` container (tools are linux/amd64 binaries).
  - `.cache/gba/agbcc`: pret/agbcc, built.

## Findings, with evidence (pokeemerald.elf, `llvm-objdump -t`)

- The ELF symbol table is complete enough to place every game symbol: 893
  `df` FILE entries (object basenames such as `main.o`), and local and global
  `O`/`F` symbols with sizes. This includes statics such as
  `082f1d48 l O .rodata 00000018 sSpriteTemplate_StampShadow` and RAM such as
  `03002710 g O iwram 00000038 gIntrTable`. Use the ELF to map names to
  addresses, not the `.map` file (the `.map` file has no statics).
- Thumb function symbols are even in the ELF (`08000738 l F .text VBlankIntr`).
  ROM data holds them with bit 0 set: `gIntrTableTemplate` at 0x082e9548 is
  0x8000845 VCountIntr, 0x8000879 SerialIntr, 0x800ba29 (timer 3),
  0x8000815 HBlankIntr, 0x8000739 VBlankIntr, then IntrDummy. A C reference to
  a function must therefore become `addr|1`, and lookups must mask bit 0.
- agbcc pads every struct and union to a multiple of 4 bytes and aligns it to
  4: `sMauvilleGymSwitchCoords` (4 x `struct UCoords8 {u8 x, y;}`) has size
  0x10, and `sRotatingGate_ArmPositions*` (8 x `Coords8`) has size 0x20. clang
  has to match, for example by rewriting every non-packed `struct/union ... {`
  in the preprocessed source to `__attribute__((aligned(4)))`. A bridge check
  should compare each IR global's size against the ELF size.
- Compile flags from pokeemerald's Makefile: `old_agbcc` for `libc.c` and
  `m4a.c`; `agb_flash*.c` and `siirtc.c` take their own CFLAGS. The asm in
  `src/` is `crt0.s`, `m4a_1.s`, `libgcnmultiboot.s` and `rom_header.s`; the
  data asm is `data/*.s`. Use `-funsigned-char` (ARM char).

## Design decided so far (not yet implemented)

- No change to the guest contract for memory: the ROM is copied to
  0x08000000 through `np_host_rom_read`. The flash at 0x0E000000 sits above
  `NP_GUEST_C_BASE`, so the port does not map it. Instead it provides a C
  model of the agb_flash API (IdentifyFlash, ReadFlash,
  ProgramFlashSectorAndVerify, EraseFlashSector, ...) over a buffer that
  `np_frame_desc.save_image`/`save_size` publish (Emerald 128 KiB, R/S
  64 KiB). siirtc is replaced by a model built on `np_host_rtc_now`.
- Contract additions: `NP_GAME_RUBY/SAPPHIRE/EMERALD` in `np_core.h`. A
  single-screen frame is `screen[1] == 0` (NULL in `np_frame`) at 240x160, so
  the geometry check in `core/runtime/np_imports.c:83` must allow that size.
  `NP_KEY_*` bits 0..9 already match GBA KEYINPUT.
- Bridge (generalises `games/platinum/tools/armrec/irbridge.py` by importing
  its IR lexer and call parser, without changing its D/P behaviour). For each
  bridged TU, plus every global in the ELF:
  1. A data symbol with an ELF address gets its definition dropped, and every
     use becomes `inttoptr(addr)`. This covers ROM const data and EWRAM/IWRAM
     variables: ROM tables hold pointers to RAM variables, for example
     `sSpecialVars`, so RAM has to sit at the matching addresses.
  2. A function with an ELF address: every non-call use becomes
     `inttoptr(addr|1)`. Every such function, including layer replacements of
     asm symbols such as `MPlayMain` and the `ply_*` functions, goes into a
     generated sorted dispatch table. ROM data references functions that the
     TU no longer references once the const data is dropped, so all of them
     are registered.
  3. Indirect calls go through `ai$<sig>`, which calls natively for a wasm
     table index, and otherwise looks up the GBA address and either calls it
     natively (same sig) or goes through word-marshalling adapters.
  4. `load/store volatile` becomes `gba_vload/vstore{8,16,32}`. These run the
     I/O side effects (DMA enable, timers, IF write-1-clear, sound regs/FIFO,
     BG2X/Y reload; a VCOUNT read advances one scanline) and are plain memory
     accesses elsewhere.
  5. A per-game drop list for functions the port overrides (for example
     Ruby's busy-wait `WaitForVBlank`).
  The preprocessing step runs the decomp's own `preproc` (built natively)
  after `clang -E`, so `_()` strings and INCBIN give exact sizes. The data
  is then dropped by rule 1, so none of it reaches the module. Check with a
  scan of the module's data segments against the ROM for long identical
  runs.
- Layout: the guest-side layer goes in `games/gba-common/pc/{src,hw,include,mk}`
  plus `games/gba-common/tools` (gbabridge.py, elfsyms.py). It is compiled
  into every GBA wasm module, the same way `games/platinum/pc` serves D/P,
  while `core/` stays host-only. Per game: `games/emerald/pc/Makefile.wasm`
  and an adapter (IntrMain priority order from crt0.s, the address of
  `gIntrTable`, save size). Ruby/Sapphire: `games/ruby/pc` with
  GAME_VERSION.
- Hardware, all in the guest:
  - Scanline PPU derived from `games/platinum/pc/hw/pc_gpu2d.c` (melonDS,
    GPLv3), with the DS 3D, capture, VRAM banks and per-frame closed forms
    removed.
  - APU (4 PSG channels + FIFO A/B) based on mGBA (MPL-2.0, keep its notices,
    in `pc/hw`), output at 32768 Hz (512 cycles/sample).
  - Timers stepped per scanline (1232 cycles; 228 lines per frame); FIFO DMA
    refills on timer overflow.
  - `m4a_1.s` (SoundMainRAM mixer, MPlayMain, ply_*) rewritten in C.
  - SWI HLE in place of libagbsyscall.
  - VBlankIntrWait drives the lines, yields with `np_host_vblank` at line 160
    and then dispatches the VBlank IRQ.
  - Soft reset through a fresh fiber.

## Next steps

1. Vendor (clean tree required):
   `git subtree add --prefix=games/emerald https://github.com/pret/pokeemerald master --squash`
   and the same for `games/ruby` from pret/pokeruby. Before doing this, check
   how games/platinum was vendored (`git log --grep=git-subtree-dir`).
2. `tools/rom_build.sh emerald|ruby|sapphire`: build agbcc in the container,
   then `make` / `make ruby` / `make sapphire`. Keep the .gba/.elf/.map files
   and check the SHA-1s (Emerald above; Ruby and Sapphire from pokeruby's
   README).
3. Slices that can proceed in parallel once 1–2 have landed: bridge +
   elfsyms; Emerald front-end compile (`clang -E` → align rewrite → preproc →
   IR); PPU; APU + m4a C mixer; system/BIOS/flash/RTC; runtime + headless +
   shell (enum, single-screen, romdb SHA-1s, launcher cards, key mapping).
4. Boot Emerald with np_headless, then Ruby/Sapphire, then gameplay scenarios.
   Check that `tests/dp/regress.sh` is unchanged.
