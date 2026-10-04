# nativeplat / wired

Two isolated UI implementations based on morisoba.moe's personal desktop:
near-black backgrounds, fine circuit traces, double-line window borders,
terminal labels, muted monochrome text, sparse sage/blue/pink indicators.
The supplied reference was inspected live in Safari. Assets are newly drawn
with CSS/SVG/native geometry; no site images or game graphics were copied.

## Isolation

- `shell/`, `core/`, `features/`, `games/`, and original `tools/` remain untouched.
- `shell-wired/`: full copy of the native shell, including existing local edits.
- `web/`: independent React + TypeScript + Vite app and browser runtime.
- `build/shell-wired*`, `build/web-cores`, `build/web-tests`: generated outputs.
- The web preparation scripts read original WASM and write browser copies.
- Native wired data and browser IndexedDB are separate from original app data.

The native shell keeps the complete existing feature set. The browser hosts
actual game cores through ABI v2; browser-specific implementations of the
save editor, mods, wireless and other native-only features are explicitly
tracked in `web/README.md`. Game-owned menus are drawn by the untouched game
cores. Changing those requires separate game-source copies, not host theming.

## Run the native app with real cores

From the repository root:

```sh
cmake -S shell-wired -B build/shell-wired-real -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DNP_CORE=real \
  -DNP_GUEST_WASM_platinum="$PWD/games/platinum/build/pc-wasm/pokeplatinum.wasm" \
  -DNP_GUEST_WASM_diamond="$PWD/games/diamond/build/pc-wasm/pokediamond.wasm" \
  -DNP_GUEST_WASM_pearl="$PWD/games/diamond/build/pc-wasm/pokepearl.wasm" \
  -DNP_GUEST_POSTPROCESS="$PWD/tools/wasm2c_postprocess.py"
cmake --build build/shell-wired-real -j8
open build/shell-wired-real/nativeplat.app
# A clean launchable copy from this run also lives at build/wired/nativeplat.app.
```

To work on UI without compiling games, use `-DNP_CORE=stub` in a separate
build directory. Stub behavior is only a test pattern, not gameplay.

## Run / package the web app

```sh
cd web
npm ci
npm run prepare:cores
npm run verify:cores
npm run dev
# or, for a verified static bundle:
npm run release
```

The build writes a content-addressed game manifest and validates imports,
module integrity, ABI behavior, and saves. A failed actual-core boot check
marks that game blocked instead of enabling a broken Play button. The current
Pearl binary stalls; keep building it in the original porting workstream and
rerun core preparation/verification when a new WASM artifact is available.

## Iteration map

| Concern | Native copy | Web |
| --- | --- | --- |
| Colors | `shell-wired/src/theme.h` | `web/src/theme.css` variables |
| Common UI | `shell-wired/src/ui.c` | `web/src/components/Panel.tsx` |
| Launcher | `draw_launcher` in `ui.c` | `web/src/App.tsx`, `catalog.ts` |
| Save editing | `shell-wired/src/editor.c` | Native-only for now |
| Mod manager | `shell-wired/src/mods.c` | Native-only for now |
| Storage | `shell-wired/src/storage.c` | `web/src/storage.ts` |
| Game execution | Existing `core/` ABI | `web/src/runtime/host.ts` |
| Input/audio/pacing | Copied shell modules | `web/src/runtime/useSession.ts` |
| WASM transformation | Original native pipeline | `web/scripts/prepare-cores.mjs` |

Native and web intentionally share the visual specification and the guest
ABI rather than coupling native drawing to DOM components. Keep UI changes
in these new folders; upstream changes can be reviewed and copied explicitly.

## Verified in this iteration

- Original source snapshot: 81,539 files checked; none changed.
- Native stub build: shell unit + rendered autotest passed.
- Native real build: all three existing cores linked; shell unit and Platinum
  first-save/reboot/trainer validation passed (127.6 seconds for gameplay).
- Browser adapter: six tests passed, including the original mock guest's
  multi-fiber workload, RGB channels, save persistence and ARM division.
- Actual browser WASM: Diamond and Platinum passed 600 frames; Platinum
  passed 1,500 frames and was also launched and visually inspected in Safari.
- Pearl's current artifact times out before its first frame; the web manifest
  marks it blocked. No changes were made to its original game code.
- TypeScript and Vite production build passed. Static output: `web/dist/`;
  packaged output: `build/nativeplat-web.tar.gz`. The subsequent public GCP
  deployment is documented in `web/deploy/GCP.md`.

The color regression was in browser `0x00RRGGBB` to RGBA conversion: unmasked
green/blue values saturated in Uint8ClampedArray. Every channel is now masked
to eight bits, with exact red/green/blue/mixed-pixel assertions.
