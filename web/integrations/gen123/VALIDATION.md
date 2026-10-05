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
- TypeScript/Vite production build and manifest-based engine staging pass.

These are smoke and save-plumbing checks, not full-game playthroughs. Upstream
Gen 2 is still under construction and Gen 3 is beta; the selector says so.
No Gen 1–3 cartridges are included in the web bundle. No production database
migration, cloud deployment, remote push, or changes to upstream checkouts were
performed as part of this integration.
