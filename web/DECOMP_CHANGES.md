# Web-only decomp changes

Original `games/`, `core/`, `features/`, `shell/`, and `tools/` sources remain
untouched. Rebuilds and experiments use the copy under `build/web-source/`.
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
