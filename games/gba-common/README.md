# GBA games: Ruby, Sapphire, Emerald

pret's pokeruby and pokeemerald compiled as C to wasm32, run by the same
runtime as the DS games (`core/`, contract `core/include/np_guest_abi.h`),
with the cartridge's data read from the player's own ROM.

## Layout

- `tools/`: the build.
  - `gbabuild.py emerald|ruby|sapphire [-j N]`: the whole guest build.
  - `elfsyms.py`: the decomp ELF's symbol table (every function and object,
    statics by FILE group) as JSON.
  - `srcfix.py`: agbcc's struct layout for clang (every struct/union aligned
    to 4 and padded to a multiple of 4; bit-field-only structs packed) and
    inline ARM asm removed (barriers, register bindings and file-scope asm
    dropped; other asm becomes a trap, `naked` removed).
  - `gbabridge.py`: rewrites one TU's LLVM IR against the ELF (see below).
  - `gen_dispatch.py`: the code-address table behind `gba_dispatch()`.
- `pc/src`, `pc/include`: the GBA machine, compiled into every GBA module.
  - `gba_main.c`: entry, ROM copy to 0x08000000, frame boundary, soft reset.
  - `gba_io.c`: I/O registers, DMA, timers, interrupts (each decomp's crt0.s
    IntrMain: `gba_crt0_emerald`, `gba_crt0_ruby`), the scanline clock, the
    bridge's volatile hooks.
  - `gba_ppu.c`: scanline renderer (text/affine/bitmap BGs, sprites,
    windows, blending, mosaic).
  - `gba_bios.c`: the BIOS calls (libagbsyscall) in C.
  - `gba_m4a.c`: m4a_1.s (MP2K sequencer, channel allocation, DirectSound
    mixer) in C; m4a.c is the decomp's own.
  - `gba_apu.c`: PSG channels and DirectSound output at 32768 Hz.
  - `gba_flash.c`, `gba_rtc.c`: the agb_flash and siirtc APIs over a flash
    image the host stores and the host clock.
  - `gba_link_stubs.c`: multiboot/GameCube boot (no partner).
- `games/emerald/pc/src`, `games/ruby/pc/src`: per-cartridge constants.
- `games/ruby/pc/patches/<file>.c.patch`: fixes to pokeruby TUs that cannot
  build as they are (asm-only functions given C), applied by gbabuild.py to a
  copy. `pc/include/gba_prelude.h` makes pokeruby's K&R calls match their
  callee's arity, which wasm requires.

## Inputs (outside git)

`.cache/gba/pokeemerald` (pret/pokeemerald 731ad5b) and
`.cache/gba/pokeruby` (pret/pokeruby 5784633), each with its matching ROM
built (`tools/rom_build.sh emerald|ruby|sapphire`, which installs
`.cache/gba/agbcc`): the build reads the ELF for addresses and the built
graphics for INCBIN sizes. The ROM itself is the player's; the module holds
none of its bytes. Expected SHA-1s: Emerald f3ae0881…, Ruby f28b6ffc…,
Sapphire 3ccbbd45… (shell/src/romdb.c).

```sh
tools/rom_build.sh emerald
games/gba-common/tools/gbabuild.py emerald -j4      # games/emerald/build/pc-wasm/pokeemerald.wasm
cmake -S core -B build/core-gba -G Ninja \
  -DNP_GUEST_WASM_emerald=$PWD/games/emerald/build/pc-wasm/pokeemerald.wasm \
  -DNP_GUEST_POSTPROCESS_emerald=$PWD/tools/wasm2c_postprocess.py
cmake --build build/core-gba -j4
build/core-gba/np_headless emerald pokeemerald.gba --frames 3000 --dump out --dump-every 60
```

## How the decomp runs

Each decomp TU goes through `clang -E`, the decomp's `preproc` (charmap
strings, INCBIN), `srcfix.py`, `clang -S -emit-llvm` with no optimisation
passes, `gbabridge.py`, then `clang -O2 -c`. The bridge:

1. drops every global the ELF places (statics by the TU's FILE group,
   function-scope statics `f.x` by `x.N`, the rest by name), so a use of
   one becomes its cartridge address: ROM tables are read from the ROM and
   RAM variables live at their EWRAM/IWRAM addresses, as ROM data points at
   them. A size different from the ELF's is reported (`bridge_report.txt`,
   `M` records).
2. turns every non-call use of a function into its GBA code address (the
   ELF's, Thumb bit included; port functions get a synthetic 0x0Fxxxxxx
   one) and gives each function a word wrapper registered under that
   address, so function pointers in ROM data and in C compare and call
   alike.
3. sends indirect calls through `gba_dispatch` (a binary search over the
   generated table), recording a call-site id for error reports.
4. replaces volatile loads/stores with `gba_vload*/gba_vstore*`: I/O side
   effects, and polls (eight identical reads of one location) move the
   clock, which is how busy-waits such as WaitForVBlank's end.
5. starts every decomp function with `gba_tick()`: 96 calls are a
   scanline of CPU time, so interrupts arrive while the game computes
   (DoMapLoadLoop waits for the VBlank handler's DMA queue).

Time is in scanlines: the PPU draws line by line, HBlank DMA/IRQs and
VCount matches happen per line, and at line 160 the frame goes to the host
(`np_host_vblank`) before the VBlank interrupt runs. Interrupt handlers run
with time stopped.

## Licences

The machine is new code for this project (GPL-3.0-or-later). The BIOS
ArcTan/affine algorithms follow GBATEK and mGBA's documented BIOS behaviour;
the decomps are pret's (their own licence terms apply to their sources,
which are not vendored here).
