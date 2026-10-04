# nativeplat / wired — web

React + TypeScript + Vite frontend, a dedicated Web Worker runtime, and a
copy-only WASM packaging pipeline. All existing source trees are read-only
inputs. Native UI work is the sibling `shell-wired/` copy.

## Develop

Node 22.12+ (or Node 24), npm; the repo's pinned wasi-sdk + WABT for preparing
cores/tests. The original game builds must already exist. `npm run build:pearl` rebuilds
Pearl in a disposable copy for future diagnosis; this work is currently paused.

```sh
cd web
npm ci
npm run prepare:cores
npm run verify:cores
npm run dev
```

Open http://127.0.0.1:5173, import a matching cartridge, then start a journey.
ROM identification matches the native shell's SHA-1 registry. Imported ROMs and saves
are stored in IndexedDB and are never uploaded. The GCP release additionally
offers encrypted Diamond and Platinum packages, downloaded, decrypted and
verified on first play before caching in IndexedDB. This is download obfuscation;
the client receives the key. Gameplay and subsequent launches use the existing
decrypted cartridge cache. Existing desktop saves can
be imported as raw 512 KiB `.sav` or DeSmuME `.dsv`. Each import creates a new
slot; committing a game save preserves the previous image as a backup.

No generated core is committed. `prepare:cores` reads these files, creates
browser variants under `public/cores`, and writes a versioned SHA-256 manifest:

- `../games/diamond/build/pc-wasm/pokediamond.wasm`
- `../games/diamond/build/pc-wasm/pokepearl.wasm`
- `../games/platinum/build/pc-wasm/pokeplatinum.wasm`

Override with `NP_GUEST_WASM_diamond`, `NP_GUEST_WASM_pearl`, or
`NP_GUEST_WASM_platinum`; `WABT_BIN` can select a different tool location.
Optional positional game IDs restrict the manifest to those games, e.g.
`npm run prepare:cores -- platinum diamond`. Old content-addressed files may
remain on disk, but the manifest determines which cores players can launch.

## Runtime boundary

`src/runtime/host.ts` implements the existing `np_guest_abi.h` v2 contract.
The copy of each module is disassembled by WABT, patched to give integer
`div`/`rem` the cartridge's ARM semantics (same behavior as the native
`wasm2c_postprocess.py`), and instrumented by Binaryen Asyncify.

The host allocates a 512 KiB continuation buffer per fiber using the guest's
allocator. It saves/restores `__stack_pointer` independently of Asyncify's
continuation stack. `vblank` yields to the worker; `fiber_switch` transfers
between guest fibers. Frames are validated and converted from `0x00RRGGBB`
to RGBA; audio leaves the worker as stereo PCM and Web Audio resamples it.
No SharedArrayBuffer, COOP/COEP, browser extension, or JSPI support is needed.
The main thread bounds work to one outstanding frame request. A 45-second
load timeout and 15-second frame timeout keep failed cores recoverable.

`worker.ts` handles loading, integrity checks, and transfers; `useSession.ts`
handles presentation, input, audio, pause/close, and serialized persistence.
`storage.ts` owns IndexedDB. The game registry is `catalog.ts`. UI components
never access WASM memory. New game features should extend typed worker
messages and capabilities, rather than putting runtime logic in components.

## Verification

```sh
npm test                  # compiles original mock guest as read-only input
npm run verify:cores      # 12,000 frames for optimized cores; 600 for legacy cores
npm run verify:options    # real-core resolution, widescreen and save-refusal checks
npm run smoke -- platinum 1500
npm run build             # TypeScript and production bundle
```

Tests cover live fiber scheduling/shadow-stack integrity, ROM boundary reads,
input, render-size changes, RGB conversion, audio, save-store + dirty flush,
save reload, ARM division edge cases, rejected ROMs, and transactional backup
preservation. The mock guest and original game code are never edited.

Current release keeps the existing hosted Diamond artifact. Platinum passes a
12,000-frame browser-runtime check and real-core resolution/quick-save checks.
Pearl work is paused: an isolated rebuild passed early boot but a longer intro
input test exposed a shared Diamond/Pearl heap hang. Experimental artifacts are
excluded from the hosted release. See [DECOMP_CHANGES.md](DECOMP_CHANGES.md).
Boot checks do not imply full-game correctness.

## Static deployment

```sh
npm run release
# outputs: dist/ and ../build/nativeplat-web.tar.gz
```

Release rebuilds copied cores, records boot-check results, runs tests, builds
the frontend, verifies every core digest, refuses ROM/save files, and emits
SHA256SUMS plus gzip/Brotli sidecars. Host `dist/` over HTTPS (localhost is
fine for development); `.wasm` must be served as `application/wasm`.
The relative Vite base supports a subdirectory deployment without routing
rewrites. Keep `index.html` and `cores/manifest.json` revalidated; cache hashed
JS/CSS/WASM immutably. Serve compressed sidecars with the correct
Content-Encoding, not as opaque downloads. Uncompressed D/P artifacts are
about 62 MiB each, so check a static provider's individual-file size limit.

An Nginx configuration and Dockerfile are under `deploy/`:

```sh
docker build -f deploy/Dockerfile -t nativeplat-web .
docker run --rm -p 8080:8080 nativeplat-web
```

Place it behind your HTTPS proxy for public use. The portable release command
only produces local artifacts. For the public GCP deployment and redeployment
command, see [deploy/GCP.md](deploy/GCP.md).

## Capability scope

The copied native shell retains its full existing features. This web frontend
currently offers cartridges, slots, save import/export, stacked screens, handheld controls, LCD
texture, speed, audio volume, instant text, bug-fix opt-in, keyboard/gamepad/
touch input, fullscreen, local RTC, automatic pause, and local persistence.

**Preferences → Recomp options** now exposes the current guest controls. Diamond
and Platinum offer live 1×–4× 3D resolution and widescreen; Platinum additionally
offers music/effects volume, field camera distance (25%–400%) and tilt (±45°),
instant text and documented cartridge bug fixes. Platinum quick save is in the
toolbar and on F1, using the game's own field/save rules, with success reported
only after browser persistence. Requests outside the field are reported as refused.
Camera and audio have original-value defaults; HD is opt-in. Controls follow the
running game even if another cartridge is selected. Unsupported D/P hooks are
not exposed as functional controls. Preferences from older web versions migrate
with original gameplay defaults for the new settings.

Screen aspect ratio follows actual guest frames, and stylus input maps to the
centred 256×192 DS area at every resolution, excluding widescreen side gutters.
New controls do not add per-frame React updates or cryptography.

For subsequent Platinum translation builds, `npm run sync:cores` prepares the
latest Platinum WASM artifact and runs its boot and options checks, without
editing or building inside the original translation trees. Diamond stays on its
existing release artifact. The original trees must be built by their own
workstream before syncing. With a local field-save fixture,
`NP_TEST_SAVE=/absolute/path/to/save.sav npm run verify:options` also exercises
field settings and successful quick save without writing to the source save.
Results go to `../build/web-cores/options-verification.json`.

Pearl's experimental `build:pearl` copies the source trees and remaps dependency
paths before building under `build/`. It is excluded from `sync:cores` and public
staging while its intro hang remains unresolved. Do not publish its diagnostic
core. `verify-journey.ts` is a manual diagnostic for resuming that investigation.

Browser save editing, event gifts, mod/content mounting, GBA migration,
rewind/snapshots, LAN/relay, folder sync, and the native renderer's advanced
visual effects are extension work. The browser reports no network/GBA device
and no WASI content directory; it does not emulate successful transports or
silently present these features as working. In-game menus and assets remain
inside the unmodified game cores and are not reskinned by this host UI.

Player controls live in the game toolbar: mute preserves the selected volume,
and 2× toggles fast-forward. Saves, preferences, and keyboard help open in
closeable dialogs (close button, Escape, or backdrop). Cartridge selection
stays beside the screens without a separate library page.

The frame loop bypasses React state updates, keeps canvas sizes stable, and
uses transferred pixel buffers without copying them again. Each game tick
gets its own canvas draw: normal play targets 59.8261 FPS, and 2× targets
119.6522 FPS with matching audio playback speed. Absolute deadlines recover
timer jitter without accumulating an extra delay on every tick.

Open **Preferences → Performance / FPS** for measured canvas FPS, game ticks,
requestAnimationFrame cadence, fresh-frame refreshes, worker time (mean and
95th percentile), round-trip latency, and canvas upload time. Metrics count
both screens as one frame and never set React state per frame. The popup
refreshes once per second. rAF observations are repaint opportunities, not
proof of physical display scanout.

The popup can run **12,000 frames at a 300 FPS target**. This accelerates the
current game during the test, then restores the selected playback speed.
It continues on window blur while the tab is visible; hiding the tab or
pausing stops it. The observed rate can be lower than the target. The
benchmark retains its complete-run average separately from the live samples.

For an isolated core throughput benchmark (screen conversion included;
canvas, browser messaging, and presentation excluded):

```sh
npm run benchmark -- public/cores/<platinum-file>.wasm 12000 ../build/web-cores/result.json
```

Platinum preparation uses Binaryen 123 `-O3` before and after Asyncify by default.
To prepare a selected core without replacing other manifest entries:

```sh
NP_WASM_OPT=/path/to/wasm-opt npm run prepare:cores -- platinum --optimize
npm run verify:cores -- platinum
```

An installed native Binaryen 123 under `build/web-cores` is preferred when
`NP_WASM_OPT` is unset; the npm-packaged optimizer remains the fallback. Existing optimized recipe
choices survive a subsequent release build. Original source and WASM files
remain read-only; only prepared copies are optimized.
