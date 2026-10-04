import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { GuestHost } from "../src/runtime/host.ts";
const game = process.argv[2] || "platinum",
  frames = Number(process.argv[3] || 600),
  batch = Number(process.argv[4] || 1);
if (![1, 2].includes(batch)) throw Error("Batch must be 1 or 2");
const root = resolve(import.meta.dirname, "../..");
const manifest = JSON.parse(
  readFileSync(resolve(root, "web/public/cores/manifest.json"), "utf8"),
);
const rom =
  game === "platinum"
    ? "games/platinum/build/rom/pokeplatinum.us.nds"
    : `games/diamond/build/${game}.us/poke${game}.us.nds`;
const host = new GuestHost(
  new Uint8Array(readFileSync(resolve(root, rom))),
  undefined,
  () => {},
  (text) => {
    if (/fatal|error|assert/i.test(text)) console.log(text.trim());
  },
);
await host.load(
  new Uint8Array(
    readFileSync(resolve(root, "web/public/cores", manifest.games[game].file)),
  ),
);
const started = performance.now();
for (let i = 0; i < frames; i += batch) {
  const count = Math.min(batch, frames - i);
  const frame = host.runFrame(count);
  if (i % 100 === 0)
    console.log(
      game,
      i,
      frame.width,
      frame.height,
      frame.audio.length,
      `fibers=${host.fibers.size}`,
    );
  if (i + count === frames) {
    const header = Buffer.from(`P6\n${frame.width} ${frame.height * 2}\n255\n`),
      rgb = Buffer.alloc(frame.width * frame.height * 6);
    [frame.top, frame.bottom].forEach((p, s) => {
      for (let j = 0; j < p.length / 4; j++) {
        rgb[((s * p.length) / 4 + j) * 3] = p[j * 4];
        rgb[((s * p.length) / 4 + j) * 3 + 1] = p[j * 4 + 1];
        rgb[((s * p.length) / 4 + j) * 3 + 2] = p[j * 4 + 2];
      }
    });
    writeFileSync(
      resolve(root, `build/web-cores/${game}-smoke.ppm`),
      Buffer.concat([header, rgb]),
    );
  }
}
console.log(
  `${game}: ${frames} frames (batch ${batch}) in ${((performance.now() - started) / 1000).toFixed(2)}s`,
);
