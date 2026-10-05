# Experimental Canvas 2D LÖVE backend

This isolated game integration is **not deployed** or part of the production
engine/staging allowlist. All copied game changes are recorded in
`web/DECOMP_CHANGES.md`; original source checkouts remain untouched.

The reusable foundation has been extracted to the separate public
[sohamb117/love-canvas](https://github.com/sohamb117/love-canvas) repository
(initial commit `7484544`). That MIT-licensed repository includes no game code
or game shaders. It adds a standalone demo, clean integration API, compatibility
table, and pixel-reference/browser tests. This folder is still the independent
prototype, not yet a pinned consumer of the extracted library. It retains
specialized game shader paths under the game's GPL obligations.

The existing LÖVE WASM retains Lua, image decoding, filesystem, audio, and game
logic. Native window/graphics modules are disabled in a copied archive.
`graphics.lua` supplies the used `love.graphics` subset and submits drawing
batches to Canvas 2D. Image bytes move on creation/update, not in every draw.

Implemented foundation: textures, nearest-neighbor quads, render targets,
transforms, clipping, sprite batches, common primitives, blend approximations,
Gen 1/2 palettes, color-key transparency, and CPU equivalents of the three
mandatory GBA PPU shaders. Those equivalents derive from pinned GPL Gen1Recomp
`src/core/game3/gba_ppu.lua`. Optional GLSL effects are translated ahead of time
using pinned glsl-transpiler; browser code never evaluates source strings.
Successful translation is a syntax check, **not proof of shader parity**.

## Prepare and test

Prepare the ordinary engine first, then:

```sh
npm ci --prefix web/integrations/gen123-canvas
python3 web/scripts/prepare-gen123-canvas.py
node web/scripts/verify-gen123-canvas.mjs Red
GEN123_FIELD=1 node web/scripts/verify-gen123-canvas.mjs Red
```

The verifier expects a locally staged pokeweb server at `127.0.0.1:4177`,
including the user's hosted cartridge packages. It intercepts only engine
requests with ignored `build/gen123-canvas` output, blocks WebGL, and records
screenshots and metrics under ignored `build/canvas-verification`. Test files,
cartridges, builds, saves, and dependencies are never committed.

## Qualification so far

- Red: intro/dialogue and field movement visually checked; 3,905 frames with zero
  WebGL requests; an actual 2,116-byte save was created. Reload/cloud-save
  roundtrips are not qualified on this renderer yet.
- Crystal: startup, gender selection, and clock dialogue rendered. Field gameplay
  and saving still need qualification.
- Emerald: startup runs without WebGL using CPU PPU effects. Captured transition
  output does not qualify full visual accuracy, field gameplay, or performance.
- Standalone library: reference pixels and actual Lua demo passed 3,600 frames
  with WebGL blocked. This is library evidence, not game compatibility evidence.

Native vector fonts, meshes, stencil, several API overloads and optional effects
are missing or unqualified. Game rendering samples exclude Lua, serialization,
and browser presentation; do not interpret batch timings as end-to-end FPS.
The existing WebGL production bundle stays in place until compatibility and
sustained performance meet the game's requirements.
