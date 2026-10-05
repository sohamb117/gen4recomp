# Local validation — 2026-10-04

- Browser extraction/boot and input smoke checks completed for Red, Blue, Yellow,
  Gold, Silver, Crystal, FireRed, LeafGreen and Emerald using checksum-matching
  test cartridges. Crystal's optional Gold artwork probes required the Lua-side
  missing-image guard documented in DECOMP_CHANGES.md.
- Red: entered the field through the actual pokeweb UI, wrote a 2,111-byte `.pws`
  envelope to IndexedDB, closed the process, and relaunched that save.
- FireRed: verified cached extraction reuse and advanced into the new-game flow.
- Production nginx/CSP: Red first boot 7,302 ms; cached boot 434 ms in the local
  headless Chromium check. These are loader-to-ready timings, not gameplay FPS
  or guarantees for other devices/games. The cache is entirely client-side.
- 26 web tests pass, including DS runtime/storage regression checks and new
  game/format validation. Five API tests pass against an ephemeral local
  Postgres instance, including DS/engine replacement, one row per account,
  checksum validation, cross-game rejection, and migration preserving a save.
- Normal browser audio activation (without an autoplay override), mute,
  pause/resume, and close-game flushing passed in the UI.
- TypeScript/Vite production build and the complete GCP staging bundle pass.

These are smoke and save-plumbing checks, not full-game playthroughs. Upstream
Gen 2 is still under construction and Gen 3 is beta; the selector says so.
No Gen 1–3 cartridges are included in the web bundle. No production database
migration, cloud deployment, remote push, or changes to upstream checkouts were
performed as part of this integration.

## Authorized GCP release — 2026-10-04

The subsequent deployment request expanded public hosting to all twelve games.
Revision `nativeplat-00007-vc2` deployed the nine Gen 1–3 encrypted packages and
expanded the existing Neon schema. Both Google and custom-domain checks verified
all twelve decrypted ROM fingerprints, all eight Gen 1–3 engine/source/license
files, and 600 frames per DS core. The live API verified generated save fixtures
for every game against Neon, replacement back to DS, and one row per account;
all temporary accounts were removed.

Fresh Chromium UI sessions at https://pokeweb.morisoba.moe used the actual hosted
cartridges with no local-file injection. First/cached loader-to-ready timings:
Red 8,652/1,267 ms, Crystal 18,197/1,501 ms, Emerald 83,992/4,752 ms. Each cached
launch made no cartridge request and repeated no extraction. Input, pause/resume,
and closing passed. These timings are not gameplay FPS benchmarks. Gen 2 remains
unfinished upstream and Gen 3 remains beta.

LibreWolf's console confirmed that its disabled WebGL setting caused LÖVE's
native-looking OpenGL error. The browser wrapper now checks WebGL once before
engine download/extraction and returns an actionable error to the parent UI.
It does not change browser privacy settings or add work to the frame loop.

Revision `nativeplat-00008-dn7` ships the graphics check. A fresh browser with
WebGL deliberately disabled returned the clear error without a native dialog or
heavy engine download; Close game restored the UI. Enabled-WebGL Red startup
passed locally and through the live domain (4,129/884 ms first/cached). This
checks startup and cache behavior; removing the WebGL dependency remains separate.
