import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { GuestHost } from "../src/runtime/host";
const root = resolve(import.meta.dirname, "../..");
const core = resolve(process.argv[2]);
const frames = Number(process.argv[3] || 12000);
if (!Number.isInteger(frames) || frames < 1000)
  throw Error("Use at least 1000 measured frames");
const host = new GuestHost(
  new Uint8Array(
    readFileSync(resolve(root, "games/platinum/build/rom/pokeplatinum.us.nds")),
  ),
  undefined,
  () => {},
  () => {},
);
await host.load(new Uint8Array(readFileSync(core)));
for (let i = 0; i < 1200; i++) host.runFrame();
const times: number[] = [];
const start = performance.now();
for (let i = 0; i < frames; i++) {
  const t = performance.now();
  host.runFrame();
  times.push(performance.now() - t);
  if ((i + 1) % 2000 === 0) console.log(`${i + 1}/${frames} frames`);
}
const seconds = (performance.now() - start) / 1000;
times.sort((a, b) => a - b);
const result = {
  core,
  workload:
    "Platinum title loop, no input; includes screen conversion, excludes browser canvas",
  warmupFrames: 1200,
  frames,
  seconds,
  fps: frames / seconds,
  medianMs: times[Math.floor(frames * 0.5)],
  p95Ms: times[Math.floor(frames * 0.95)],
  p99Ms: times[Math.floor(frames * 0.99)],
};
console.log(JSON.stringify(result, null, 2));
if (process.argv[4])
  writeFileSync(
    resolve(process.argv[4]),
    JSON.stringify(result, null, 2) + "\n",
  );
