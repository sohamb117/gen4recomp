// Exercise the real copied cores without modifying cartridge/save inputs.
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { GuestHost } from "../src/runtime/host";
import { gameOptions, initialSettings } from "../src/runtime/settings";
import { OPT } from "../src/runtime/abi";
const root = resolve(import.meta.dirname, "../..");
const manifest = JSON.parse(
  readFileSync(resolve(root, "web/public/cores/manifest.json"), "utf8"),
);
const report: Record<string, unknown> = {};
const selected = process.argv.slice(2);
const games = selected.length ? selected : ["platinum"];
for (const game of games) {
  assert.ok(game === "platinum" || game === "diamond" || game === "pearl");
  const rom =
    game === "platinum"
      ? "games/platinum/build/rom/pokeplatinum.us.nds"
      : `games/diamond/build/${game}.us/poke${game}.us.nds`;
  let stored = 0;
  const host = new GuestHost(
    new Uint8Array(readFileSync(resolve(root, rom))),
    process.env.NP_TEST_SAVE
      ? new Uint8Array(readFileSync(process.env.NP_TEST_SAVE))
      : undefined,
    (data) => {
      assert.equal(data.length, 512 * 1024);
      stored++;
    },
    (text) => {
      if (/fatal|assert/i.test(text)) console.log(text);
    },
  );
  await host.load(
    new Uint8Array(
      readFileSync(
        resolve(root, "web/public/cores", manifest.games[game].file),
      ),
    ),
  );
  host.runFrame();
  const sizes = [];
  for (const widescreen of [false, true])
    for (const renderScale of [1, 2, 3, 4]) {
      host.options = gameOptions(
        { ...initialSettings, renderScale, widescreen },
        game,
      );
      const frame = host.runFrame();
      assert.equal(frame.width, (widescreen ? 342 : 256) * renderScale);
      assert.equal(frame.height, 192 * renderScale);
      assert.equal(frame.top.length, frame.width * frame.height * 4);
      sizes.push(`${frame.width}x${frame.height}`);
    }
  host.options = gameOptions(initialSettings, game);
  assert.equal(host.runFrame().width, 256);
  report[game] = { geometry: sizes, restoredOriginal: true };
  host.options[OPT.quickSaveSequence] = 1;
  let frame = host.runFrame();
  for (let i = 0; i < 100 && frame.status.quickSaveSequence !== 1; i++)
    frame = host.runFrame();
  assert.equal(frame.status.quickSaveSequence, 1);
  assert.equal(
    frame.status.quickSaveResult,
    2,
    "title screen must refuse quick save",
  );
  assert.equal(stored, 0, "refused request must not write a save");
  Object.assign(report[game] as object, { refusedSave: true });
  if (process.env.NP_TEST_SAVE) {
    for (let i = 0; i < 5000 && !frame.status.fieldReady; i++) {
      // Dismiss title, choose Continue and reach the saved field.
      host.input.keys = i % 60 < 4 ? 1 : 0;
      frame = host.runFrame();
    }
    assert.equal(
      frame.status.fieldReady,
      true,
      "save fixture must reach the field",
    );
    host.input.keys = 0;
    host.options = gameOptions(
      {
        ...initialSettings,
        cameraZoom: 512,
        cameraTilt: -160,
        instantText: true,
        fixBugs: true,
        musicVolume: 50,
        effectsVolume: 75,
      },
      game,
      2,
    );
    for (let i = 0; i < 100 && frame.status.quickSaveSequence !== 2; i++)
      frame = host.runFrame();
    assert.equal(frame.status.quickSaveResult, 1);
    assert.equal(frame.status.quickSaveSequence, 2);
    assert.ok(
      stored > 0,
      "successful quick save reaches the persistence callback",
    );
    Object.assign(report[game] as object, {
      fieldOptions: true,
      quickSave: true,
      writes: stored,
      map: frame.status.mapId,
    });
  }
  console.log(game, report[game]);
}
writeFileSync(
  resolve(root, "build/web-cores/options-verification.json"),
  JSON.stringify(report, null, 2) + "\n",
);
console.log(report);
