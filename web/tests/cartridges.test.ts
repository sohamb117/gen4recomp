import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { packCartridge } from "../scripts/cartridge-package.mjs";
import { parseCartridges, fetchCartridge, loadCartridges, unpackCartridge } from "../src/cartridges";

const plain = new Uint8Array(4096).map((_, i) => i % 251);
const { payload, entry } = packCartridge("diamond", plain);
const arrayBuffer = (bytes: Uint8Array) => new Uint8Array(bytes).buffer;
const hash = (bytes: Uint8Array) => createHash("sha256").update(bytes).digest("hex");

test("hosted catalog accepts only encrypted packages and valid metadata", () => {
  assert.deepEqual(parseCartridges({ version: 2, games: { diamond: entry } }), { diamond: entry });
  assert.throws(() => parseCartridges({ version: 1, games: { diamond: entry } }), /Invalid/);
  for (const change of [{ file: "../secret.npc" }, { file: "https://example.org/rom.npc" }, { file: "diamond.nds" }, { bytes: -1 }, { bytes: 268435457 }, { packedBytes: 0 }, { sha256: "bad" }, { key: "00" }, { iv: "00" }, { encoding: "none" }])
    assert.throws(() => parseCartridges({ version: 2, games: { diamond: { ...entry, ...change } } }), /Invalid/);
});
test("build packer round-trips through browser AES-GCM and bounded decompression", async () => {
  assert.deepEqual(new Uint8Array(await unpackCartridge(arrayBuffer(payload), entry)), plain);
  const second = packCartridge("diamond", plain);
  assert.notEqual(second.entry.iv, entry.iv);
  assert.notEqual(second.entry.key, entry.key);
  assert.notEqual(second.entry.file, entry.file);
});
test("tampering, wrong keys, expanded-size mismatches, and plaintext corruption fail closed", async () => {
  const changed = new Uint8Array(payload);
  changed[0] ^= 1;
  await assert.rejects(unpackCartridge(arrayBuffer(changed), entry), /integrity/);
  // Updating the untrusted digest cannot bypass the GCM authentication tag.
  await assert.rejects(unpackCartridge(arrayBuffer(changed), { ...entry, packedSha256: hash(changed) }), /decrypted/);
  await assert.rejects(unpackCartridge(arrayBuffer(payload), { ...entry, key: "00".repeat(32) }), /decrypted/);
  await assert.rejects(unpackCartridge(arrayBuffer(payload), { ...entry, bytes: plain.length - 1 }), /exceeded/);
  await assert.rejects(unpackCartridge(arrayBuffer(payload), { ...entry, bytes: plain.length + 1 }), /incomplete/);
  await assert.rejects(unpackCartridge(arrayBuffer(payload), { ...entry, sha256: "00".repeat(32) }), /integrity/);
});
test("missing hosted catalog preserves local-import deployments", async (t) => {
  t.mock.method(globalThis, "fetch", async () => new Response(null, { status: 404 }));
  assert.deepEqual(await loadCartridges("./"), {});
});
test("download rejects truncated, oversized and corrupt packages", async (t) => {
  for (const [size, error] of [[entry.packedBytes - 1, /incomplete/], [entry.packedBytes + 1, /exceeded/], [entry.packedBytes, /integrity/]] as const) {
    const mock = t.mock.method(globalThis, "fetch", async () => new Response(new Uint8Array(size)));
    await assert.rejects(fetchCartridge("./", "diamond", entry, () => {}), error);
    mock.mock.restore();
  }
});
test("a valid encrypted package still has to be a recognized cartridge", async (t) => {
  t.mock.method(globalThis, "fetch", async () => new Response(new Uint8Array(payload)));
  await assert.rejects(fetchCartridge("./", "diamond", entry, () => {}), /Cartridge not recognized/);
});
