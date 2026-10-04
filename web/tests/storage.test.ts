import "fake-indexeddb/auto";
import { test } from "node:test";
import assert from "node:assert/strict";
import { storage, saveData } from "../src/storage";
import { identifyRom } from "../src/catalog";
test("save commits preserve one backup and keep games/slots separate", async () => {
  await storage.putSlot({
    id: "a",
    game: "platinum",
    name: "First",
    updated: 0,
  });
  await storage.putSlot({
    id: "b",
    game: "diamond",
    name: "Second",
    updated: 0,
  });
  const a = new Uint8Array(524288);
  a[0] = 1;
  await storage.save("a", a.buffer);
  const b = a.slice();
  b[0] = 2;
  await storage.save("a", b.buffer);
  const slots = await storage.slots(),
    s = slots.find((x) => x.id === "a")!;
  assert.equal(new Uint8Array(s.data!)[0], 2);
  assert.equal(new Uint8Array(s.backup!)[0], 1);
  assert.equal(slots.find((x) => x.id === "b")!.data, undefined);
  await assert.rejects(storage.save("missing", a.buffer), /committed/);
});
test("raw and DSV save sizes are checked before importing", () => {
  assert.equal(saveData(new ArrayBuffer(524288)).byteLength, 524288);
  assert.equal(saveData(new ArrayBuffer(524410)).byteLength, 524288);
  assert.throws(() => saveData(new ArrayBuffer(4)), /512 KiB/);
});
test("unrecognized ROMs are rejected with their fingerprint", async () => {
  await assert.rejects(identifyRom(new ArrayBuffer(100)), /cartridge dump/);
  await assert.rejects(
    identifyRom(new ArrayBuffer(512)),
    /SHA-1: [a-f0-9]{40}/,
  );
});
