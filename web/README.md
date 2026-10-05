# nativeplat / wired — web

React + TypeScript + Vite frontend, a dedicated Web Worker runtime, and a
copy-only WASM packaging pipeline. All existing source trees are read-only
inputs. Native UI work is the sibling `shell-wired/` copy.

## Develop

Node 22.12+ (or Node 24), npm; the repo's pinned wasi-sdk + WABT for preparing
cores/tests. Seed the worktree with the repository's ignored ROM/SDK inputs first.
`npm run sync:cores` rebuilds Platinum, Diamond, and Pearl in isolated copies, prepares
optimized browser modules, and runs boot and option checks.

```sh
cd web
npm ci
npm run sync:cores
npm run dev
```

Open http://127.0.0.1:5173, import a matching cartridge, then start a journey.
ROM identification matches the native shell's SHA-1 registry. Imported ROMs stay
in IndexedDB. Local saves need no account; the Neon backend syncs one selected save per username/password account on the live site. The GCP release additionally
offers encrypted Diamond, Pearl, and Platinum packages, downloaded, decrypted and
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

The merged-main release rebuilds all three hosted cores from clean snapshots.
`npm run verify:first-save -- diamond` replays upstream's new-game and Continue
schedules through the browser host and validates the resulting save checksums.
Set `NP_SAVE4_TOOL` to a built `np_save4` executable (the local default is
`build/merge-features/np_save4`).
Older diagnostic modules remain excluded from public hosting.
See [DECOMP_CHANGES.md](DECOMP_CHANGES.md).
Boot checks do not imply full-game correctness. The CLI verification commands
use Node's `--liftoff-only` baseline compiler mode: Node 22 can spend minutes draining background
TurboFan compilation after D/P checks finish, or when loading a second instance.
This affects the test harness only; browser builds retain their full optimization
and default engine behavior. These checks qualify behavior, not browser FPS.

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
Content-Encoding, not as opaque downloads. Check the generated manifest for artifact sizes and your static provider's
individual-file size limit.

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

**Preferences → Recomp options** now exposes the current guest controls. Diamond, Pearl, and Platinum offer live 1×–4× 3D resolution, widescreen, and music/effects volume, field camera distance (25%–400%) and tilt (±45°),
instant text and documented cartridge bug fixes. Quick save is in the
toolbar and on F1, using the game's own field/save rules, with success reported
only after browser persistence. Requests outside the field are reported as refused.
Camera and audio have original-value defaults; HD is opt-in. Controls follow the
running game even if another cartridge is selected. The shared D/P option hooks now come from main's field, text, and rules implementations. Preferences from older web versions migrate
with original gameplay defaults for the new settings.

Screen aspect ratio follows actual guest frames, and stylus input maps to the
centred 256×192 DS area at every resolution, excluding widescreen side gutters.
New controls do not add per-frame React updates or cryptography.

For subsequent translation updates, `npm run sync:cores` builds Platinum and
Diamond and Pearl under `build/web-source-*/`, then prepares and validates browser copies.
It never builds inside the original translation trees. Raw modules and source
commit/hash records live in `build/web-guest/`. Prepared manifests include that
provenance. Set `NP_WASM_OPT` to a native Binaryen 123 binary for faster packaging.
With a local field-save fixture, `NP_TEST_SAVE=/absolute/path/to/save.sav npm run
verify:options -- platinum` (or `diamond` / `pearl`) exercises field settings and successful
quick save without modifying the input save. Each fixture must match its game.
Results go to `../build/web-cores/options-verification.json`.

Pearl release work has resumed on the merged-main source snapshot. It uses the
same copy-only `build:cores` pipeline and must pass its own 12,000-frame check,
first-save/Continue replay, and options checks. The older `build:pearl` and
`verify-journey.ts` helpers are retained only for historical diagnostics.

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

## Optional cloud saves

Account controls appear only in Save manager. Play and local save import/export
remain available without login. Create an account with a username and password;
there is no email, verification flow, or password recovery service.

Each account has one cloud save total, across all games. For an empty account,
the next in-game save connects that local slot. Choose **Save to cloud** or
**Replace cloud save** beside an existing local slot to connect it instead.
Subsequent writes from that slot sync automatically after IndexedDB commits.
**Load cloud save** creates a fresh local slot and connects it, preserving local
slots. Close a running game before loading a cloud save. Other local slots keep
working independently; replace the cloud save explicitly to switch slots.

Uploads run outside the frame loop. Network failures leave the local save intact;
reconnection retries pending writes, and reopening the site resumes a linked
local save if the remote revision is unchanged. Cross-device conflicts require
loading the remote save or explicitly replacing it. Cloud uploads never include
cartridges, save backups, or browser preferences.

`server/` is an isolated Node/Postgres API with parameterized queries, salted
scrypt passwords, opaque hashed sessions in secure HttpOnly cookies, same-origin
write checks, bounded request sizes, and database-backed sign-in throttling.
A primary key on account ID enforces one save. Save images are exactly 512 KiB,
nonblank, assigned a supported game, and SHA-256 checked on upload/download.
This validates storage integrity, not every internal cartridge save checksum.

For the complete local app and persistent Postgres, run:

```sh
bash web/scripts/local-up.sh  # from the repository root
```

Open http://127.0.0.1:8088. Account controls appear only inside Save manager.
The database uses a persistent Docker volume; stopping containers preserves it.
See [server/README.md](server/README.md) for development, tests, and lifecycle
commands. The live GCP service uses Neon through a Secret Manager-injected
`DATABASE_URL`; CockroachDB is not yet qualified.
