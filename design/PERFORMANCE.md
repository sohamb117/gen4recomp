# Browser performance check — 2026-10-04

Measurements use real Platinum WASM and a local cartridge. Original game,
core, and native-shell source files were not changed.

## Live Safari measurements

Both long runs requested 300 distinct dual-screen canvas draws/second and
completed **12,000 draws**. Counts are whole dual-screen frames, not pixels,
screens, or duplicated draws of the same guest frame. These are live game
runs, not identical deterministic replays, so treat the comparison as observed
end-to-end behavior rather than an isolated causal microbenchmark.

| Prepared core | Canvas FPS, whole-run average | Elapsed | Fresh rAF opportunities/sec |
| --- | ---: | ---: | ---: |
| Original Asyncify recipe (v1) | 42.7 | 280.9 s | 29.7 |
| O3 before and after Asyncify (v2) | 122.9 | 97.6 s | 30.0 |

The original 2-tick-per-draw path initially measured 23.2 canvas draws/sec,
46.5 game ticks/sec, 42.88 ms per worker batch, and 0.17 ms canvas upload.
The single-tick path removes the previous ~60-draw ceiling at 2×.

The optimized build reaches ~120 canvas FPS at 2× in lighter scenes, but
heavier observed scenes fall to ~80 FPS (~12 ms worker time). **It does not
sustain 300 FPS.** Canvas upload is roughly 0.1 ms; core execution remains
more expensive.

An additional sample collected with the diagnostics popup closed measured
119.9 canvas FPS, 3.84 ms mean worker time, 8 ms worker p95, and 0.11 ms canvas
upload. Browser rAF cadence still measured 30/sec. The popup itself is not
the cause of that refresh reading. macOS reported low-power mode disabled;
physical display refresh could not be established in this environment.

`requestAnimationFrame` measures callbacks before repaint and generally
follows the display refresh cadence; it is not physical scanout telemetry.
Do not describe 122.9 canvas draws/sec as 122.9 visible monitor refreshes/sec.
See [MDN's requestAnimationFrame documentation](https://developer.mozilla.org/en-US/docs/Web/API/Window/requestAnimationFrame).

Raw observed browser results are in generated `build/web-cores/safari-baseline.json`
and `safari-optimized.json`. The optional readout is **Preferences → Performance / FPS**.

## Headless core measurements

Each title-loop run used **1,200 warm-up + 12,000 measured frames**. These
include guest execution and pixel conversion, but exclude canvas, browser
messaging, audio playback, and visible presentation. No gameplay input was
sent. These numbers cannot substitute for browser FPS.

| Prepared core | Frames/sec | Median frame | p95 frame |
| --- | ---: | ---: | ---: |
| v1 | 186.37 | 5.39 ms | 8.56 ms |
| v2 | 201.67 | 4.99 ms | 7.85 ms |

Generated reports: `build/web-cores/baseline-benchmark.json` and
`optimized-benchmark.json`. Other compilation/browser work overlapped some
of these runs; they are capacity diagnostics, not controlled lab benchmarks.

## Validation and reproduction

- Ten tests passed, including frame-rate accounting, distinct game/draw/refresh
  counts, batching parity, RGB channels, audio timing, fiber stacks, and saves.
- The same ten tests passed using an O3/Asyncify/O3 mock guest.
- The published optimized Platinum artifact separately passed the 12,000-frame
  `verify:cores` gate. Its SHA-256 is
  `14803892e88e5bb4c86340472c1849ef0e7df09a184c5124fa9447c72ca9f012`.
- TypeScript and production build passed. These checks do not qualify every
  scene, save path, or full-game completion.

Use `npm run benchmark -- public/cores/<file>.wasm 12000 <report.json>` for
core throughput. Use the popup's 12,000-frame / 300-FPS test for real canvas
throughput. The test accelerates the live game and restores the user's speed
setting afterward; hiding the tab or pausing cancels it.
