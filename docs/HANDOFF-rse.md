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

1. Link trade test (tests/link style): `tools/gba/gen3_warp.py` turns a
   house save into one that CONTINUEs into Oldale's Pokemon Center 2F
   (`--map 2.3 --pos 7,4` in R/S, the trade attendant above; Emerald's
   direct-corner attendant is at (10,2)) with a party of two and
   FLAG_SYS_POKEDEX_GET (`--sb1-bit 0x1320.1`; Emerald `0x137C.1`). Two
   instances: `np_headless ... --net 47210 --net-id 0x111111 --net-wait 20
   -e PC_GBA_LINK_WAIT=1` and the same with 47211 / 0x222222 (the lower id
   is the parent). Schedule so far (both): 400/700 start, 900/1000 a,
   1150 up, A every 80 frames from 1200 to 3100: the link comes up at
   ~2433, the parent's "Start link with 2 players" is confirmed, both warp
   into the Trade Center at 3165 (map 6425) and see each other. Chairs are
   the triggers at (4,5) and (7,5); players arrive at the door (5,8)/(6,8):
   next try up 1 tile, then left (parent) / right (child), then up 2; then
   the trade screens. The child unplugs at the end of a run because the
   parent stops first (give the parent a few more frames).
2. Package: `tools/package_macos.sh --test` and the Windows zig build with
   all six cores; run `tests/mac/feature_matrix.py --game sapphire|emerald`
   on the packaged app (Ruby ran on build/rse/shell's bundle: all 12 GBA
   cases ok); zips to ~/Downloads/nativeplat/ named with main's sha.
3. OBJ mosaic approximate.

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
