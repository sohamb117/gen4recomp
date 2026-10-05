// Replay upstream's first-save and Continue schedules through the browser host.
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { execFileSync } from "node:child_process";
import { GuestHost } from "../src/runtime/host";
const root = resolve(import.meta.dirname, "../..");
const game = process.argv[2] || "diamond";
assert.ok(game === "diamond" || game === "pearl");
const manifest = JSON.parse(
  readFileSync(resolve(root, "web/public/cores/manifest.json"), "utf8"),
);
const romPath = resolve(
  root,
  `games/diamond/build/${game}.us/poke${game}.us.nds`,
);
const rom = new Uint8Array(readFileSync(romPath));
const wasm = new Uint8Array(
  readFileSync(resolve(root, "web/public/cores", manifest.games[game].file)),
);
const key: Record<string, number> = {
  a: 1,
  b: 2,
  select: 4,
  start: 8,
  right: 16,
  left: 32,
  up: 64,
  down: 128,
  r: 256,
  l: 512,
  x: 1024,
  y: 2048,
  none: 0,
};
async function replay(schedule: string, save?: Uint8Array) {
  const text = readFileSync(resolve(root, "tests/dp", schedule), "utf8");
  const frames = Number(text.match(/# frames:\s*(\d+)/)?.[1]);
  assert.ok(frames > 0);
  let previous = 0;
  const steps = text
    .split("\n")
    .flatMap((line) => line.split("#")[0].split(";"))
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      const [at, kind, ...values] = line.split(":");
      const frame = at.startsWith("+") ? previous + Number(at) : Number(at);
      previous = frame;
      const v = values.map(Number),
        touch = kind === "tap";
      const x = touch ? v.shift()! : 0,
        y = touch ? v.shift()! : 0;
      const keys = touch
        ? 0
        : kind.split("+").reduce((value, name) => {
            assert.ok(name in key);
            return value | key[name];
          }, 0);
      return {
        frame,
        touch,
        x,
        y,
        keys,
        duration: v[0] || 6,
        every: v.length >= 3 ? v[1] : 0,
        count: v.length >= 3 ? v[2] : 1,
      };
    });
  let stored: Uint8Array | undefined,
    writes = 0;
  const host = new GuestHost(
    rom,
    save,
    (bytes) => {
      stored = new Uint8Array(bytes);
      writes++;
    },
    (text) => {
      if (/fatal|assert|error/i.test(text)) console.log(text.trim());
    },
  );
  await host.load(wasm);
  let field = false;
  for (let i = 0; i < frames; i++) {
    host.input = { keys: 0, touch: false, x: 0, y: 0 };
    for (const step of steps) {
      let offset = i - step.frame;
      if (offset < 0) continue;
      if (step.every) {
        if (Math.floor(offset / step.every) >= step.count) continue;
        offset %= step.every;
      }
      if (offset >= step.duration) continue;
      if (step.touch)
        Object.assign(host.input, { touch: true, x: step.x, y: step.y });
      else host.input.keys |= step.keys;
    }
    const frame = host.runFrame();
    field ||= frame.status.fieldReady;
    if ((i + 1) % 1000 === 0)
      console.log(`${game} ${schedule}: ${i + 1}/${frames}`);
  }
  assert.ok(field, "Must reach the field");
  assert.ok(
    writes > 0 && stored?.length === 524288,
    "Game must persist a full save",
  );
  return stored!;
}
const target = resolve(root, `build/web-cores/${game}-first-save.sav`);
function inspect(bytes: Uint8Array) {
  writeFileSync(target, bytes);
  const dump = JSON.parse(
    execFileSync(
      process.env.NP_SAVE4_TOOL ||
        resolve(root, "build/merge-features/np_save4"),
      ["dump", romPath, target],
      { encoding: "utf8" },
    ),
  );
  assert.equal(dump.trainer.name, "NATIVE");
  const block = dump.blocks.find((b: { name: string }) => b.name === "general");
  return Math.max(
    ...block.valid.map((valid: boolean, i: number) =>
      valid ? block.save_counter[i] * 2 ** 32 + block.block_counter[i] : -1,
    ),
  );
}
const first = await replay(`${game}_first_save.sched`);
const before = inspect(first);
assert.ok(before >= 0, "First save checksum must be valid");
const continued = await replay("continue.sched", first);
const after = inspect(continued);
assert.ok(after > before, "Continue must persist a newer valid save");
writeFileSync(
  resolve(root, `build/web-cores/${game}-first-save.json`),
  JSON.stringify(
    {
      game,
      core: manifest.games[game].file,
      trainer: "NATIVE",
      before,
      after,
      passed: true,
    },
    null,
    2,
  ) + "\n",
);
console.log(
  `${game}: new game, first save, Continue, and re-save passed with valid checksums.`,
);
