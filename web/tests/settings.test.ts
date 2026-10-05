import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { ABI, OPT } from "../src/runtime/abi";
import {
  gameOptions,
  initialSettings,
  normalizeSettings,
} from "../src/runtime/settings";
import { mapTouch } from "../src/runtime/touch";
import { GuestHost } from "../src/runtime/host";

test("preference migration preserves defaults and bounds malformed records", () => {
  assert.deepEqual(normalizeSettings(null), initialSettings);
  assert.deepEqual(normalizeSettings({ volume: 0.2, instantText: true }), {
    ...initialSettings,
    volume: 0.2,
    instantText: true,
  });
  const s = normalizeSettings({
    volume: NaN,
    renderScale: 99,
    cameraTilt: -9999,
    cameraZoom: 0,
    musicVolume: "0",
    speed: 300,
  });
  assert.equal(s.volume, 0.65);
  assert.equal(s.renderScale, 4);
  assert.equal(s.cameraTilt, -720);
  assert.equal(s.cameraZoom, 64);
  assert.equal(s.musicVolume, 100);
  assert.equal(s.speed, 1);
});

test("web option indexes match the read-only native ABI header", () => {
  const header = readFileSync(
    new URL("../../core/include/np_guest_abi.h", import.meta.url),
    "utf8",
  );
  const names = [
    "BGM_VOLUME",
    "SE_VOLUME",
    "RENDER_SCALE",
    "WIDESCREEN",
    "CAMERA_ZOOM",
    "CAMERA_TILT",
    "QUICKSAVE_SEQ",
    "RULES",
    "TEXT_INSTANT",
  ];
  Object.values(OPT).forEach((index, i) =>
    assert.match(header, new RegExp(`NP_OPT_${names[i]}\\s*=\\s*${index}\\b`)),
  );
});

test("native guest receives live recomp settings and returns quick-save status", async () => {
  const host = new GuestHost(
    new Uint8Array(0x4000),
    undefined,
    () => {},
    () => {},
  );
  await host.load(
    new Uint8Array(
      readFileSync(
        new URL("../../build/web-tests/browser.wasm", import.meta.url),
      ),
    ),
  );
  host.runFrame();
  host.options = gameOptions(
    {
      ...initialSettings,
      musicVolume: 50,
      effectsVolume: 0,
      renderScale: 2,
      widescreen: true,
      cameraZoom: 512,
      cameraTilt: -160,
      instantText: true,
      fixBugs: true,
    },
    "platinum",
    7,
  );
  const frame = host.runFrame();
  assert.equal(frame.width, 512);
  assert.equal(frame.height, 384);
  assert.equal(frame.status.fieldReady, true);
  assert.equal(frame.status.quickSaveSequence, 7);
  assert.equal(frame.status.quickSaveResult, 1);
  const v = host.view(),
    d = host.descriptor;
  // Mock reports the options it actually consumed, rather than merely reading
  // back the host's input buffer. Slot 5 preserves the signed tilt bit pattern.
  assert.deepEqual(
    Array.from({ length: 8 }, (_, i) =>
      v.getUint32(d + ABI.status + (8 + i) * 4, true),
    ),
    [128, 0, 2, 1, 512, -160 >>> 0, 7, 1],
  );
  assert.equal(v.getUint32(d + ABI.options + OPT.instantText * 4, true), 1);
  host.options = gameOptions(initialSettings, "platinum", 7);
  assert.equal(host.runFrame().width, 256);
});

test("Diamond/Pearl receive their implemented recomp hooks", () => {
  const s = {
    ...initialSettings,
    renderScale: 2,
    widescreen: true,
    instantText: true,
    fixBugs: true,
    cameraTilt: 80,
    musicVolume: 0,
  };
  for (const game of ["diamond", "pearl"] as const) {
    const o = gameOptions(s, game, 4);
    assert.equal(o[OPT.renderScale], 2);
    assert.equal(o[OPT.widescreen], 1);
    assert.equal(o[OPT.instantText], 1);
    assert.equal(o[OPT.rules], 1);
    assert.equal(o[OPT.quickSaveSequence], 4);
    assert.equal(o[OPT.cameraTilt], 80);
    assert.equal(o[OPT.musicVolume], 0);
  }
});

test("stylus stays on the centred DS screen at every HD scale and rejects side gutters", () => {
  for (const scale of [1, 2, 3, 4]) {
    assert.deepEqual(mapTouch(384, 192, 768, 384, 384 * scale, 192 * scale), {
      x: 128,
      y: 96,
      inside: true,
    });
    assert.deepEqual(mapTouch(128, 0, 768, 384, 384 * scale, 192 * scale), {
      x: 0,
      y: 0,
      inside: true,
    });
    assert.equal(
      mapTouch(127, 100, 768, 384, 384 * scale, 192 * scale).inside,
      false,
    );
    assert.equal(
      mapTouch(640, 100, 768, 384, 384 * scale, 192 * scale).inside,
      false,
    );
  }
  assert.deepEqual(mapTouch(256, 256, 512, 512, 256, 192), {
    x: 128,
    y: 96,
    inside: true,
  });
  assert.equal(mapTouch(100, 10, 512, 512, 256, 192).inside, false);
});
