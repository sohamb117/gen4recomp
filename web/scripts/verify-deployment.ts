import assert from "node:assert/strict";
import { loadCartridges, fetchCartridge } from "../src/cartridges";
import { GuestHost } from "../src/runtime/host";
import type { GameId, CoreManifest } from "../src/catalog";

const base = new URL(process.argv[2] || "http://127.0.0.1:8088/").href;
const index = await fetch(base);
assert.equal(index.status, 200);
assert.match(index.headers.get("content-type") || "", /text\/html/);
assert.match(index.headers.get("content-security-policy") || "", /wasm-unsafe-eval/);
assert.match(await index.text(), /assets\/index-/);
const catalog = await loadCartridges(base);
assert.deepEqual(Object.keys(catalog).sort(), ["diamond", "platinum"]);
// Previously published plaintext URLs must no longer be served (including gzip).
for (const file of ["diamond-e29bc6ebe431d7a6.nds", "platinum-fbce4c4def0c7797.nds"]) {
  for (const suffix of ["", ".gz"]) {
    const response = await fetch(new URL(`cartridges/${file}${suffix}`, base), { method: "HEAD" });
    assert.equal(response.status, 404, `Plaintext cartridge still available: ${file}${suffix}`);
  }
}
const manifest = await fetch(new URL("cores/manifest.json", base)).then((r) => r.json()) as CoreManifest;
assert.deepEqual(Object.keys(manifest.games).sort(), ["diamond", "platinum"]);
for (const game of ["diamond", "platinum"] as GameId[]) {
  const data = await fetchCartridge(base, game, catalog[game]!, () => {});
  console.log(`${game}: downloaded encrypted package, decrypted and verified ${data.byteLength} cartridge bytes`);
  const core = manifest.games[game]!;
  const response = await fetch(new URL(`cores/${core.file}`, base));
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") || "", /application\/wasm/);
  assert.match(response.headers.get("cache-control") || "", /immutable/);
  const wasm = await response.arrayBuffer();
  assert.equal(wasm.byteLength, core.bytes);
  const hash = Buffer.from(await crypto.subtle.digest("SHA-256", wasm)).toString("hex");
  assert.equal(hash, core.sha256);
  const host = new GuestHost(new Uint8Array(data), undefined, () => {}, () => {});
  await host.load(wasm);
  let lit = false;
  for (let i = 0; i < 600; i++) {
    const frame = host.runFrame();
    assert.equal(frame.width, 256);
    assert.equal(frame.height, 192);
    lit ||= frame.top.some((byte, j) => j % 4 !== 3 && byte !== 0);
  }
  assert.ok(lit, `${game} rendered no picture`);
  console.log(`${game}: deployed core integrity and 600-frame runtime check passed`);
}
