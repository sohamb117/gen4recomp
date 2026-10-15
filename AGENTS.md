# pokeweb development

- Active release work belongs on web-ready in this isolated worktree. Never
  edit, check out, commit to, reset, or push nativeplat main. Read-only source
  inspection and merges from main into web-ready are authorized.
- This worktree shares nativeplat Git metadata. origin is gen4recomp; publish
  only web-ready there. The independent pokeweb main remains a source input.
- The primary application is `web/`; follow `web/AGENTS.md`. Keep web UI/runtime,
  save API, native UI copies, and upstream translation sources compartmentalized.
- Build game cores in disposable copies. Record any web-specific decomp patches
  in `web/DECOMP_CHANGES.md`; do not silently edit upstream game/core sources.
- Never commit credentials, cartridge images, save images, generated WASM,
  node_modules, or build outputs. Use the existing ignore rules.
- The live site is pokeweb.morisoba.moe. Reuse documented GCP/Neon configuration
  for an authorized deployment; repository naming does not rename cloud resources.
