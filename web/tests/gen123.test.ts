import { test } from "node:test";
import assert from "node:assert/strict";
import { games, generation, isGen4, isNative } from "../src/catalog";
import { saveData } from "../src/storage";
import {
  gen123Games,
  MAX_SAVE_BYTES,
  validateSave,
} from "../shared/save-format.mjs";
const envelope = (game = "red", save = 'return {version = "red",}') =>
  new TextEncoder().encode(
    JSON.stringify({ format: "pokeweb-gen123-v1", game, save }),
  );
test("generation catalog and save validator agree on all supported games", () => {
  assert.equal(games.length, 18);
  assert.deepEqual(
    games
      .filter((g) => !isNative(g.id) || g.id === "emerald")
      .map((g) => g.id)
      .sort(),
    [...gen123Games].sort(),
  );
  assert.deepEqual(
    [1, 2, 3, 4, 5].map(
      (n) => games.filter((g) => generation(g.id) === n).length,
    ),
    [3, 3, 5, 5, 2],
  );
  for (const g of games) assert.match(g.hash, /^[a-f0-9]{40}$/);
});
test("engine saves remain separate from DS SRAM and reject wrong-game imports", () => {
  const bytes = envelope();
  assert.deepEqual(new Uint8Array(saveData(bytes.buffer, "red")), bytes);
  assert.throws(() => saveData(bytes.buffer, "blue"));
  assert.throws(() => saveData(bytes.buffer, "platinum"));
  assert.throws(() => saveData(new ArrayBuffer(524288), "red"));
  assert.throws(() => validateSave(new Uint8Array(MAX_SAVE_BYTES + 1), "red"));
  assert.throws(() =>
    validateSave(envelope("red", 'os.execute("bad")'), "red"),
  );
  assert.throws(() => validateSave(new Uint8Array([255, 254]), "red"));
  assert.equal(
    saveData(new ArrayBuffer(524410), "platinum").byteLength,
    524288,
  );
});

test("native save formats cover DS and GBA while preserving legacy Emerald saves", () => {
  for (const game of ["ruby", "sapphire", "emerald"] as const) {
    assert.equal(saveData(new ArrayBuffer(131072), game).byteLength, 131072);
    assert.throws(() => saveData(new ArrayBuffer(65536), game));
  }
  for (const game of ["heartgold", "soulsilver", "black", "white"] as const)
    assert.equal(saveData(new ArrayBuffer(524288), game).byteLength, 524288);
  assert.deepEqual(
    new Uint8Array(saveData(envelope("emerald").buffer, "emerald")),
    envelope("emerald"),
  );
});
