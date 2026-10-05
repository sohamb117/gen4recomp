# Gen 1–3 browser runtime

Based on the Pokemon Gen 1 Recompilation Project by BOIS CLUB GAMES, LLC
(https://github.com/bryanthaboi/gen1recomp).

`npm run prepare:gen123` in `web/` fetches pinned sources into ignored `build/`
folders, checks every checkout, and produces the ROM-free browser engine in
`public/engines/gen123`. `npm run build` includes it; release staging checks every
asset against its manifest. No upstream branch is edited or pushed.

The nine cartridge IDs are Red, Blue, Yellow, Gold, Silver, Crystal, FireRed,
LeafGreen and Emerald. Ruby/Sapphire are not supported by this upstream engine.
Gen 2 remains upstream work in progress; Gen 3 remains beta. A title-screen smoke
test is not a full playthrough qualification.

The parent React UI owns controls, settings, slots and cloud sync. One iframe
owns one LÖVE/Lua process, with no shared save filesystem. ROMs stay in browser
IndexedDB and are passed to the process only on boot. Extracted assets are cached
as blobs, keyed by source/patch and cartridge hashes. First import is expensive;
cached launches skip extraction. No extraction, hashing or cloud requests occur
in the gameplay update loop.

Save exports are `.pws` JSON envelopes containing the engine's data-only Lua
save table and preferences. They are not raw cartridge `.sav` files. The engine
parses them with its restricted `SaveSerializer` before loading; never execute a
save with `load`. Existing DS saves retain their 512 KiB format. Cloud accounts
still have exactly one save total, replaceable across all supported games.

The packed `game.love` is a standard ZIP containing the exact modified Lua source,
assets and licenses. The excluded proprietary launcher is not included.
`patches.py` records checked copied-source substitutions. See
[DECOMP_CHANGES.md](../../DECOMP_CHANGES.md) for their rationale.

Dependencies:

- Gen1Recomp `340e2567ec93d8ac74659d7091ebbec8925d7295` (GPLv3 + attribution)
- [love.js](https://github.com/Davidobot/love.js/tree/c4f04e185033a7c9fbefa9be3bec88c41a90421b)
  compatibility runtime (MIT). Its source repository documents rebuilding LÖVE
  and its Emscripten dependencies; the generated JS adds FS access and disables
  shared automatic IDBFS save persistence.
- [lua-bit-numberlua](https://github.com/davidm/lua-bit-numberlua/tree/30b6828b298b46411b548202d40524caba06a99c)
  (MIT), with its original copyright notice inside the archive.

Test inputs can be built locally from pret/pokered, pret/pokeyellow, pret/pokegold,
pret/pokecrystal and pret/pokefirered. Never commit or include them in a web build.
The GCP cartridge allowlist remains Diamond/Pearl/Platinum; Gen 1–3 currently use
local cartridge import.

For reproducible browser smoke tests, start the web preview and run:

```sh
BROWSER_EXECUTABLE='/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' \
  npm run verify:gen123 -- red /absolute/path/to/pokered.gbc
```

`GEN123_TEST_URL` can point at an nginx preview to exercise production CSP.
The test verifies a first boot, controls, and a second boot that skips extraction;
it records screenshots and timings in ignored `build/gen123-verification/`.
Omit `BROWSER_EXECUTABLE` to use Playwright's installed Chromium on other systems.
