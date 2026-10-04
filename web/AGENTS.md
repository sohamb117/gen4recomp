# Web development boundaries

- Work only in `web/`, `design/`, `shell-wired/`, and generated `../build/`
  for this UI workstream. Original shell, core, feature, game, and tool sources
  are read-only. Explicitly copy a source tree before changing it.
- Keep game execution in `src/runtime/`; UI components do not read WASM memory.
- The source of truth for the guest layout is `../core/include/np_guest_abi.h`.
  Update ABI tests together with adapter changes.
- Imported ROMs stay client-side. Local saves work without login; the user
  authorized username/password accounts and one Postgres cloud save per account.
  Keep account controls inside Save manager and network work off the frame loop.
  The user authorized public GCP hosting
  of Diamond and Platinum; only `scripts/stage-gcp.mjs` includes those allowlisted
  cartridges as encrypted `.npc` packages; never ship raw `.nds` files. Decrypt
  only on first download, cache the result, and keep crypto out of the gameplay
  path. The portable release remains ROM-free. Never add cartridge images,
  save images, or generated WASM to version control. Only manifest-listed
  prepared cores ship.
- Use `npm test` and `npm run build` for runtime/storage changes. Run
  `npm run verify:cores` after preparing new game binaries. A passed boot
  check does not imply full gameplay qualification.
- Palette and typography live in `src/theme.css`. Keep the morisoba-inspired
  double borders, terminal labels, restrained colors, and readable controls.
- Preserve honest capability states: unavailable native-only features must
  not be represented by working-looking controls in the web frontend.
