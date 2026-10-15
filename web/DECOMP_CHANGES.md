# Web-only decomp changes

Original `games/`, `core/`, `features/`, `shell/`, and `tools/` sources remain
untouched. Rebuilds and experiments use disposable copies under `build/web-source-*/`.
Permanent web-specific patches, if needed, must live under `web/` and be applied
to an isolated copy by the web build pipeline.

## 2026-10-04 — Diamond/Pearl sync and Pearl rebuild

- Rebuilt Pearl using the current shared Diamond/Pearl translation and Platinum
  host code. This incorporates the existing upstream NVRAM boot, register access,
  and GX FIFO fixes; those fixes were not authored or modified by this workstream.
- Remapped absolute paths in copied compiler `.d` files into the snapshot so
  `make` builds copied targets only. This is build bookkeeping, not game logic.
- Pearl now passes the first 600 browser-runtime frames, where the old artifact
  stalled before frame one. Longer input qualification exposed a shared D/P heap
  corruption at frame 3573 in the intro text menu. Both current Diamond and rebuilt
  Pearl reproduce it; boot success alone is not full-game qualification.

### Temporary diagnostics — not for deployment

Copied file: `build/web-source/games/diamond/arm9/lib/libnns/src/NNS_FND_expheap.c`
(original relative path: `games/diamond/arm9/lib/libnns/src/NNS_FND_expheap.c`).

- Added `<stdio.h>` and `<stdlib.h>` in the copy.
- Added logging plus `abort()` after `GetRegionOfMBlock` computes its region if
  its start is below DS main RAM (`0x02000000`). Logs region pointer, block pointer,
  start/end and block attributes.
- Added the same diagnostic before `RecycleRegion` processes an invalid region,
  logging its input region, heap and start/end.
- Purpose: identify the first invalid free rather than let the following allocation
  spin forever. These guards are diagnostic assumptions, not a gameplay fix.
- The diagnostic module is isolated as `build/web-guest/pearl-diagnostic.wasm`;
  it must not be included in the final web release.

Validation so far: debugger identified a self-linked free block at `0x00383fe0`
inside `AllocFromHead`, called while the intro creates a text list. A standalone
probe of the original allocator's create/allocate/free routines passes repeated
round trips, so a blanket allocator replacement is not justified.

## Platinum-only release / Pearl paused

At the user's request, Pearl investigation is paused. No decomp changes are
needed for Platinum's recomp options: the web adapter exposes the existing ABI.
The public release includes the clean Platinum core and the existing hosted
Diamond core only. Neither the rebuilt Pearl core nor its diagnostic variant
is included. The copied diagnostics remain under ignored `build/` for later
investigation; there is no permanent decomp patch in this release.

## 2026-10-04 — merged-main release rebuild

- Builds use `web/scripts/build-cores-copy.py`, with fresh source copies under
  `build/web-source-*/`; original translation trees are read-only inputs.
- Platinum, Diamond, and Pearl are rebuilt from merge commit `6de15fc30` (main
  `352d36b23`). Upstream translation, timer, field, save, and battle fixes are
  inherited unchanged; no game/decomp logic patch is introduced by this release.
- Copied compiler dependency paths are remapped into the snapshot, including
  paths inherited from other worktrees. This changes only build bookkeeping.
- Each raw WASM gets a source-commit/hash record under `build/web-guest/`;
  browser preparation checks it and carries the commit into the core manifest.
- The earlier diagnostic allocator guards are absent from these fresh copies.
- Existing web recomp controls now send the shared option ABI to Diamond/Pearl,
  whose field/text/rules implementations are present in merged main. The user
  resumed Pearl qualification for this release; its old diagnostic core stays excluded.

## 2026-10-04 — Gen1Recomp browser adapter

Source: `bryanthaboi/gen1recomp` at `340e2567ec93d8ac74659d7091ebbec8925d7295`.
All substitutions run while writing a disposable archive; the source checkout,
original Gen 4 trees, and upstream branches are unchanged.

- `web/integrations/gen123/main.lua` replaces the native launcher with a new
  pokeweb bootstrap, input/pause/audio bridge, extraction, and save callbacks.
  Each iframe owns one game filesystem; `GameVersion.cachePrefix` becomes empty
  so generated assets resolve without native PhysFS/FFI mounts.
- The browser's stock Lua 5.1 needs the LuaBitOp-compatible `bit` interface and
  a string-aware `load` adapter. The former uses MIT-licensed lua-bit-numberlua;
  saves still go through upstream's data-only `SaveSerializer`, never `load`.
- Missing filesystem reads and image loads are checked before entering the C API, avoiding this
  love.js build's unsafe exception unwinding across Lua coroutines.
- `src/core/Music.lua`: do not call `setLooping` on synthesized queueable sources;
  their chip synthesizer already handles looping. LÖVE disallows that operation.
- Six GBA extraction files replace loop-tail `goto` with `repeat`/`break`:
  `map_tree.lua`, `extract_scripts.lua`, `encounters_extract.lua`,
  `extract_map_events.lua`, `door_anim_extract.lua`, `battle_anim_extract.lua`.
  Existing terminating breaks remain outside the added repeat blocks.
  The checked substitutions live in `integrations/gen123/patches.py`.
- Extraction uses the upstream sequential fallback in the thread-free browser
  build. All generations yield between work batches. During Gen 3 extraction only,
  `yield.lua` supplies coroutine-aware protected calls so progress can yield
  across the extraction tasks; native `pcall` is restored before gameplay.
- The proprietary launcher files, labels, launcher assets, and original main
  are excluded. The archive includes the corresponding modified Lua source,
  original license, required visible credit, and dependency licenses.
- love.js generated JavaScript exposes its virtual filesystem and disables its
  automatic shared IDBFS mount. pokeweb's IndexedDB owns save-slot persistence;
  engine state cannot silently leak between slots or games.
- `src/core/game3/bg_affine.lua`: replace `ffi.new("float", x)` with a
  LÖVE float32 pack/unpack round trip. This preserves single-precision rounding
  rather than silently promoting the original calculations to double precision.
- The generated love.js bridge exposes audio resume/state alongside FS access.
  User controls can resume a browser-suspended AudioContext; audio work remains
  inside LÖVE and mute/volume changes do not touch cartridge extraction.
- Import time slicing resets its budget after resumption, excluding time spent
  yielding. Counting the wait caused one-item-per-frame imports on slow frames.

## 2026-10-05 — experimental Canvas 2D backend (not deployed)

`web/scripts/prepare-gen123-canvas.py` copies the already prepared ROM-free LÖVE
archive into ignored `build/gen123-canvas`. In that copy only, `conf.lua` disables
the native window and graphics modules; `main.lua` installs the new browser
`love.graphics` adapter before the existing bootstrap and adds temporary state
diagnostics. Original game logic and all source checkouts remain unchanged.

`web/integrations/gen123-canvas` contains the separate rendering implementation.
Its GBA CPU shader equivalents derive from the pinned GPL Gen1Recomp
`src/core/game3/gba_ppu.lua`; the package retains upstream licenses/attribution.
These changes do not alter the production engine, staging manifest, or ROM data.

The reusable, game-free foundation is published separately as
https://github.com/sohamb117/love-canvas (initial commit `7484544`). Game-specific
CPU PPU/palette effects stay here. Optional GLSL effects are translated from
copied, pinned shader sources at build time; translation is not visual parity
validation. `GLSL-TRANSPILER-NOTICE.txt` accompanies generated shader helpers.

## 2026-10-15 — native WASM release from web-ready

Merged nativeplat main `7dc5e22a41` into the isolated `web-ready` worktree,
alongside pokeweb `903ecd50`. Nativeplat main and all source worktrees remain
read-only. Builds clone the source and GBA/HG/BW input assets into ignored
`build/web-source-*`; copied depfiles are remapped to prevent make writing to
source worktrees. Browser preparation patches only generated WAT/WASM.

The web host implements the new reset/reset_carry imports, preserves the backup
chip and carry across fresh instances, and accepts single-screen 240x160 GBA
frames. No decomp patch is introduced by this integration. Candidate games
ship only after browser qualification; legacy Emerald engine saves retain their
original runtime and are not converted or overwritten.

The snapshot builder also copies ignored generated HG SDK headers from the ROM
build and uses space-free local Black/White ROM filenames for make. These are
build-input adaptations, not decomp edits. Absolute depfile paths are remapped;
relative prerequisites retain their original meaning.
