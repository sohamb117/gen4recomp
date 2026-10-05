# pokeweb development

- This is a standalone repository derived from gen4recomp/web-ready. Work here;
  do not modify the original gen4recomp checkouts as part of pokeweb work.
- `origin` is sohamb117/pokeweb. Its `main` is independent of gen4recomp/main.
  Fetch/merge from `upstream` when requested; do not push to upstream unless the
  user explicitly asks to change that repository.
- The primary application is `web/`; follow `web/AGENTS.md`. Keep web UI/runtime,
  save API, native UI copies, and upstream translation sources compartmentalized.
- Build game cores in disposable copies. Record any web-specific decomp patches
  in `web/DECOMP_CHANGES.md`; do not silently edit upstream game/core sources.
- Never commit credentials, cartridge images, save images, generated WASM,
  node_modules, or build outputs. Use the existing ignore rules.
- The live site is pokeweb.morisoba.moe. Reuse documented GCP/Neon configuration
  for an authorized deployment; repository naming does not rename cloud resources.
