# RSE (Ruby/Sapphire/Emerald) handoff

Branch `rse` (worktree `../nativeplat-rse`), integrated into `main` after the
gate (`tests/dp/regress.sh` 11/11, `tests/gameplay/run.sh --game platinum`).

## State

- Emerald, Ruby and Sapphire run headless from boot: copyright, intro,
  title, Birch, naming, the truck, Littleroot (map 9) and the house (map
  256), field_ready=1, with the same input schedule
  (`tests/rse/littleroot.sched`). ~0.9 ms/frame. Evidence sheets (outside
  git): `build/evidence/rse/` (`emerald-*.png`, `ruby-00-boot-to-house.png`,
  `sapphire-00-boot-to-house.png`).
- All three: quick save through the game's own save, CONTINUE,
  `--state-test` round trips and the first battle (Route 101, Torchic;
  Poochyena in R/S, Zigzagoon in Emerald, won) headless:
  `NP_RSE_CORE=build/rse/native tests/rse/first_battle.sh` (18 checks; legs
  chained through the game's own saves, schedules `tests/rse/{rs,e}-*.sched`).
  Shell autotest (`NP_AUTOTEST rom=`) OK on all three.
- Audio: the m4a mixer plays DPCM-compressed and reversed voices
  (pokeruby's cries are DPCM); BGM/SE volume apply per channel owner.
- Instant text (`NP_OPT_TEXT_INSTANT`): `games/{emerald,ruby}/pc/patches/text.c.patch`
  (patched TUs see `core/include`).
- Shell: GBA `.sav` import (128 KiB, mGBA RTC record dropped); Delta GBA
  skins with their own option (`skin_gba`). Feature matrix:
  `docs/FEATURE_PARITY.md`, "Ruby / Sapphire / Emerald".
- Regression: `tests/dp/regress.sh` cases `e-title` (Emerald title at frame
  650), `e-littleroot`, `r-littleroot`, `s-littleroot` (11000 frames); they
  are SKIPped without the decomp ROMs in `.cache/gba`.
- ROMs: `tools/rom_build.sh emerald|ruby|sapphire` (container); Ruby
  f28b6ffc97847e94a6c21a63cacf633ee5c8df1e and Sapphire
  3ccbbd45f8553c36463f13b938e833f652b793e4 match pokeruby's US 1.0 SHA-1s.
- pokeruby specifics (all in the build, none in the decomp checkout):
  - crt0's IntrMain differs from Emerald's (VCount after VBlank, IME 1,
    only Serial/Timer3/HBlank nest): `gba_crt0_ruby` in `gba_io.c`.
  - `games/ruby/pc/patches/*.patch`, applied by gbabuild.py to a copy:
    atk93_tryKO (asm only; pokeemerald's C), the summary screen's move-panel
    tasks sub_80A1334/sub_80A1500 (asm only; C from the asm),
    PokemonUseItemEffects' local declaration (void vs bool8).
  - `gba_prelude.h`: K&R calls with another arity than the definition
    (GetMonData/GetBoxMonData with 2 args, nullsub_11, gpu_sync_bg_hide);
    a mismatched wasm call would hit wasm-ld's trapping stub.
  - Remaining asm-only functions trap if reached: evolution_scene.c's
    unref_sub_8113B50, unref_sub_81143CC, sub_8114E48 (unreferenced).
  - `bridge_report.txt` lists 83 `M` records: agbcc's `.comm` rounds
    1/2-byte uninitialised globals to 4 bytes; harmless (the ELF reserves
    the larger size).
- Shell: GBA cards, ROM SHA-1s, `.gba` import, single-screen layout,
  editor refusal.
- Decomps are not vendored (disk): pinned clones in `.cache/gba`.

## Next steps

1. Packaged app: build the macOS zip and the Windows zig cross-build with
   the three GBA cores (`tools/package_macos.sh --test`,
   `tools/package_windows.sh --test`) and run `tests/mac/feature_matrix.py`
   rows on a GBA game (slots, import/export, quick save/load, snapshots,
   rewind, speed, volume, instant text, a GBA `.deltaskin`).
2. Touch pad: hide X/Y for GBA games in `touchpad.c`/`touchlayout.c`
   (draw, hit test, editor selection).
3. Mods for GBA: the guest reads no `/content`; a GBA package would patch
   ROM data (only data: the code is compiled), e.g. an IPS in the package
   applied before the ROM copy in `gba_main.c`. Mystery Gift / e-Reader:
   decide whether an offline event writer (save editor) is in scope.
4. Link cable: model SIO multi-player (SIOCNT/SIOMLT_SEND/RECV, the serial
   IRQ) in `gba_io.c` over `np_host_net_*`, with `--lockstep` for a
   deterministic two-instance trade test (tests/link style). Gen 3's
   link.c exchanges 8 halfwords per frame per player with handshakes, so a
   one-frame-delayed bus model is the first thing to try.
5. OBJ mosaic approximate.

## Commands

```sh
tools/rom_build.sh ruby                       # then sapphire, emerald
games/gba-common/tools/gbabuild.py ruby -j3   # sapphire, emerald
cmake -S core -B build/rse/native -G Ninja -DNP_BUILD_TESTS=OFF \
  -DNP_GUEST_WASM_ruby=$PWD/games/ruby/build/pc-wasm/pokeruby.wasm \
  -DNP_GUEST_POSTPROCESS=$PWD/tools/wasm2c_postprocess.py
cmake --build build/rse/native -j3
build/rse/native/np_headless ruby .cache/gba/pokeruby/pokeruby.gba \
  --frames 11000 --schedule tests/rse/littleroot.sched --dump out --dump-every 150
NP_RSE_CORE=build/rse/native tests/dp/regress.sh --only r-littleroot
```
