// Read-only cartridge inputs; interactive D/P smoke beyond the early boot gate.
import { readFileSync, writeFileSync } from "node:fs";
import assert from "node:assert/strict";
import { resolve } from "node:path";
import { GuestHost } from "../src/runtime/host";
const root = resolve(import.meta.dirname, "../..");
const game = process.argv[2] || "pearl";
assert.ok(["diamond", "pearl"].includes(game));
const manifest = JSON.parse(
  readFileSync(resolve(root, "web/public/cores/manifest.json"), "utf8"),
);
const host = new GuestHost(
  new Uint8Array(
    readFileSync(
      resolve(root, `games/diamond/build/${game}.us/poke${game}.us.nds`),
    ),
  ),
  undefined,
  () => {},
  (text) => {
    if (/fatal|assert|error/i.test(text)) console.log(text.trim());
  },
);
await host.load(
  new Uint8Array(
    readFileSync(resolve(root, "web/public/cores", manifest.games[game].file)),
  ),
);
let lit = false;
const started = performance.now();
for (let i = 0; i < 12000; i++) {
  host.input.keys = i === 1800 ? 8 : i > 1860 && i % 60 < 4 ? 1 : 0;
  if (i % 600 === 0) console.log(`${game}: entering frame ${i}`);
  const f = host.runFrame();
  assert.equal(f.width, 256);
  assert.equal(f.height, 192);
  if ([1799, 3599, 5999, 11999].includes(i)) {
    const rgb = Buffer.alloc(f.width * f.height * 6);
    [f.top, f.bottom].forEach((pixels, screen) => {
      for (let p = 0; p < pixels.length / 4; p++) {
        for (let c = 0; c < 3; c++)
          rgb[(screen * f.width * f.height + p) * 3 + c] = pixels[p * 4 + c];
      }
    });
    lit ||= rgb.some((byte) => byte !== 0);
    writeFileSync(
      resolve(root, `build/web-cores/${game}-journey-${i + 1}.ppm`),
      Buffer.concat([
        Buffer.from(`P6\n${f.width} ${f.height * 2}\n255\n`),
        rgb,
      ]),
    );
    console.log(`${game}: ${i + 1} frames, pixels captured`);
  }
}
assert.ok(lit, "no rendered image");
const report = {
  game,
  frames: 12000,
  seconds: (performance.now() - started) / 1000,
  core: manifest.games[game].file,
  note: "Boot/title/intro input smoke; not full-game qualification",
};
writeFileSync(
  resolve(root, `build/web-cores/${game}-journey.json`),
  JSON.stringify(report, null, 2) + "\n",
);
console.log(report);
