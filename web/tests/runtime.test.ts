import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { GuestHost } from "../src/runtime/host";
const wasm = new Uint8Array(
  readFileSync(
    process.env.NP_TEST_WASM ||
      new URL("../../build/web-tests/browser.wasm", import.meta.url),
  ),
);
function rom() {
  return Uint8Array.from(
    { length: 0x4000 },
    (_, i) => (i * 7 + (i >> 8) + 3) & 255,
  );
}
test("real ABI: fiber shadow stacks, frame/audio, input, guest save + dirty flush, reboot", async () => {
  const saves: Uint8Array[] = [];
  const host = new GuestHost(
    rom(),
    undefined,
    (s) => saves.push(s.slice()),
    () => {},
  );
  await host.load(wasm);
  let f = host.runFrame();
  assert.equal(f.width, 256);
  assert.equal(f.height, 192);
  assert.equal(host.fibers.size, 4);
  const px = host.view().getUint32(host.descriptor + 8, true);
  host.u32(px + 100 * 4, 0x123456);
  host.u32(px + 101 * 4, 0xff0000);
  host.u32(px + 102 * 4, 0x00ff00);
  host.u32(px + 103 * 4, 0x0000ff);
  const colors = host.frame().top;
  assert.deepEqual(
    [...colors.slice(400, 416)],
    [18, 52, 86, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255],
  );
  host.input = { keys: 1, touch: true, x: 83, y: 92 };
  for (let i = 0; i < 9; i++) f = host.runFrame();
  const v = host.view(),
    d = host.descriptor,
    top = v.getUint32(d + 8, true),
    bottom = v.getUint32(d + 12, true);
  assert.equal(v.getUint32(top, true), 1);
  assert.equal(v.getUint32(top + 4, true), 0x10000 | (83 << 8) | 92);
  const status = v.getUint32(bottom, true);
  for (const bit of [1, 8, 64, 128])
    assert.ok(status & bit, `missing status ${bit}`);
  assert.equal(saves.length, 2);
  assert.equal(saves[0].length, 256);
  assert.equal(new TextDecoder().decode(saves[1].slice(0, 4)), "NPSV");
  assert.ok(f.audio.some((x) => x !== 0));
  const reloaded = new GuestHost(
    rom(),
    saves[1],
    () => {},
    () => {},
  );
  await reloaded.load(wasm);
  reloaded.runFrame();
  const rb = reloaded.view().getUint32(reloaded.descriptor + 12, true);
  assert.ok(reloaded.view().getUint32(rb, true) & 16, "loaded persistent save");
  host.options[2] = 2;
  f = host.runFrame();
  assert.equal(f.width, 512);
  assert.equal(f.height, 384);
  host.input.keys = 512 | 256;
  assert.throws(() => host.runFrame(), /trap requested/);
});
test("memory and ABI validation reject invalid pointers", async () => {
  const host = new GuestHost(
    rom(),
    undefined,
    () => {},
    () => {},
  );
  await host.load(wasm);
  host.runFrame();
  assert.throws(() => host.bytes(0xfffffff0, 32), /bounds/);
  assert.throws(() => host.validateDescriptor(0), /ABI/);
});
test("2x batches preserve input, all audio, final pixels, and every save", async () => {
  const saves: Uint8Array[][] = [[], []];
  const hosts = saves.map(
    (list) =>
      new GuestHost(
        rom(),
        undefined,
        (save) => list.push(save.slice()),
        () => {},
      ),
  );
  await Promise.all(hosts.map((host) => host.load(wasm)));
  for (let batch = 0; batch < 5; batch++) {
    for (const host of hosts)
      host.input = { keys: batch & 1, touch: true, x: batch * 8, y: 92 };
    const first = hosts[0].runFrame();
    const second = hosts[0].runFrame();
    const combined = hosts[1].runFrame(2);
    assert.equal(combined.number, second.number);
    assert.ok(
      Buffer.from(combined.top).equals(Buffer.from(second.top)),
      "top pixels match",
    );
    // Mock guest pixel 3 reports OS entropy, which differs between instances.
    combined.bottom.fill(0, 12, 16);
    second.bottom.fill(0, 12, 16);
    assert.ok(
      Buffer.from(combined.bottom).equals(Buffer.from(second.bottom)),
      "bottom pixels match",
    );
    const audio = new Float32Array(first.audio.length + second.audio.length);
    audio.set(first.audio);
    audio.set(second.audio, first.audio.length);
    assert.deepEqual(combined.audio, audio);
  }
  assert.equal(saves[1].length, 2);
  assert.deepEqual(saves[1], saves[0]);
  assert.throws(() => hosts[1].runFrame(0), /Invalid frame batch/);
});
test("prepared division matches cartridge zero-divisor and overflow behavior", async () => {
  // The WAT postprocessor is separately checked by the generated fixture test below.
  const { execFileSync } = await import("node:child_process");
  const { writeFileSync } = await import("node:fs");
  const { resolve } = await import("node:path");
  const root = resolve(import.meta.dirname, "../.."),
    wat = resolve(root, "build/web-tests/div.wat");
  writeFileSync(
    wat,
    `(module (memory (export "memory") 1) (global $__stack_pointer (mut i32) (i32.const 4096)) (func $malloc (param i32) (result i32) i32.const 32) (func $free (param i32))\n` +
      ["i32", "i64"]
        .flatMap((t) =>
          ["div_s", "div_u", "rem_s", "rem_u"].map(
            (op) =>
              `(func (export "${t}.${op}") (param ${t} ${t}) (result ${t})\nlocal.get 0\nlocal.get 1\n${t}.${op}\n)\n`,
          ),
        )
        .join("") +
      ")",
  );
  execFileSync("python3", [resolve(root, "web/scripts/prepare-wat.py"), wat]);
  execFileSync(resolve(root, ".cache/toolchains/wabt/bin/wat2wasm"), [
    wat,
    "-o",
    wat + ".wasm",
  ]);
  const { instance } = await WebAssembly.instantiate(
    readFileSync(wat + ".wasm"),
  );
  const e = instance.exports as Record<string, (a: any, b: any) => any>;
  assert.equal(e["i32.div_s"](-7, 0), -7);
  assert.equal(e["i32.rem_s"](-7, 0), 0);
  assert.equal(e["i32.div_s"](-2147483648, -1), -2147483648);
  assert.equal(e["i64.rem_u"](7n, 0n), 7n);
  assert.equal(e["i64.div_s"](-(2n ** 63n), -1n), -(2n ** 63n));
  assert.equal(e["i32.div_s"](-7, 2), -3);
});
