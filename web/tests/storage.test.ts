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

test("cartridge blobs roundtrip exact bytes and old ArrayBuffer records remain readable", async () => {
  const data = Uint8Array.from([1, 2, 3, 4, 255]).buffer;
  await storage.importRom({
    game: "platinum",
    name: "fixture.nds",
    data,
    imported: 1,
  });
  assert.deepEqual((await storage.cartridge("platinum"))?.data, data);
  const db = await new Promise<IDBDatabase>((resolve, reject) => {
    const r = indexedDB.open("nativeplat-wired", 1);
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  await new Promise<void>((resolve, reject) => {
    const tx = db.transaction("roms", "readwrite");
    tx.objectStore("roms").put({
      game: "diamond",
      name: "legacy.nds",
      data,
      imported: 1,
    });
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
  assert.deepEqual((await storage.cartridge("diamond"))?.data, data);
});
