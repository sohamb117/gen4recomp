# RSE (Ruby/Sapphire/Emerald) handoff

Branch `rse` (worktree `../nativeplat-rse`), rebased on main db34ff98b.
Not yet integrated into `main`: the gate has not been run to completion.

## State

- Emerald runs headless: copyright, intro, title, Birch, naming, the truck,
  Littleroot (map 9) and the house (map 256); quick save through the game's
  own save (`-o F:quicksave_seq=1`, result 1), CONTINUE from that save,
  snapshot round trips (`--state-test`, 3/3 ok, 15.6 MB), audio (m4a + PSG).
  ~0.9 ms/frame. Evidence sheets (outside git): `build/evidence/rse/`.
- Design and the build: `games/gba-common/README.md`.
- Ruby/Sapphire: `games/ruby/pc/src/ruby_port.c` written, not built. pokeruby
  cloned at `.cache/gba/pokeruby` (5784633). Its ROM/ELF build was queued:
  `NP_HEAVY_SLOTS=1 tools/heavy.sh tools/rom_build.sh ruby` (log
  `build/rse/rom_ruby.log`), then `... sapphire`.
- Shell: GBA cards, ROM SHA-1s, `.gba` import, single-screen layout,
  editor refusal; build `build/rse/shell` was started, not yet checked.
- Decomps are not vendored (disk): pinned clones in `.cache/gba`.

## Next steps

1. Gate: `NP_HEAVY_SLOTS=1 tests/dp/regress.sh` (was queued behind other
   agents' heavy slots, log `build/rse/gate/regress.log`) and
   `tests/gameplay/run.sh --game platinum`; then `git rebase main`,
   `git -C ../nativeplat merge --ff-only rse` (or `git fetch . rse:main`).
2. Ruby/Sapphire: after the ROM builds, `games/gba-common/tools/gbabuild.py
   ruby` / `sapphire`, check `bridge_report.txt` (M/A records), build a core
   with `-DNP_GUEST_WASM_ruby=...`, boot with the Emerald-style schedule
   (`build/evidence/rse/ng3.sched` adapted).
3. Add a tests/rse hash regression (runs are deterministic).
4. Not done: compressed/reverse DirectSound samples are skipped (logged once);
   OBJ mosaic approximate; no link cable.

## Commands

```sh
games/gba-common/tools/gbabuild.py emerald -j3
cmake -S core -B build/rse/native -G Ninja -DNP_BUILD_TESTS=OFF \
  -DNP_GUEST_WASM_emerald=$PWD/games/emerald/build/pc-wasm/pokeemerald.wasm \
  -DNP_GUEST_POSTPROCESS_emerald=$PWD/tools/wasm2c_postprocess.py
cmake --build build/rse/native -j3
build/rse/native/np_headless emerald .cache/gba/pokeemerald/pokeemerald.gba \
  --frames 11000 --schedule build/evidence/rse/ng3.sched --dump out --dump-every 150
```
