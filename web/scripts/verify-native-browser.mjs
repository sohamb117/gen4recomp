// Real UI + worker qualification using local cartridges and read-only save fixtures.
import { chromium } from "playwright";
import { readFileSync, writeFileSync, mkdirSync, existsSync } from "node:fs";
import { resolve } from "node:path";
import assert from "node:assert/strict";
import { nativeGames } from "../shared/native-games.mjs";
const root = resolve(import.meta.dirname, "../..");
const game = process.argv[2] || "platinum",
  spec = nativeGames[game];
const fixture = process.env.NP_TEST_SAVE;
if (!spec || !fixture || !existsSync(fixture))
  throw Error("Pass a native game and NP_TEST_SAVE fixture");
const manifestPath = resolve(root, "web/public/cores/manifest.json");
const originalCore = JSON.parse(readFileSync(manifestPath, "utf8")).games[game];
const out = resolve(root, "build/browser-native");
mkdirSync(out, { recursive: true });
const browser = await chromium.launch({
  headless: true,
  executablePath:
    process.env.BROWSER_EXECUTABLE ||
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
});
try {
  const page = await browser.newPage({
      viewport: { width: 1280, height: 1000 },
    }),
    errors = [];
  page.on("console", (m) => {
    if (m.type() === "error") console.log("BROWSER", m.text());
  });
  setInterval(async () => {
    try {
      console.log(
        "STATE",
        await page.evaluate(() => ({
          notice: document.querySelector("[role=status]")?.textContent,
          frames: window.nativeFrames,
          error: window.nativeError,
          frame: window.lastNativeFrame,
          body: window.nativeFrames
            ? undefined
            : document.body.innerText.slice(0, 1800),
        })),
      );
    } catch {}
  }, 10000).unref();
  page.on("pageerror", (e) => errors.push(e.message));
  await page.addInitScript(() => {
    window.nativeFrames = 0;
    window.glRequests = 0;
    window.testDrive = true;
    window.lastNativeFrame = null;
    const context = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function (type, ...args) {
      if (/webgl/i.test(type)) {
        window.glRequests++;
        return null;
      }
      return context.call(this, type, ...args);
    };
    const WorkerBase = window.Worker;
    window.Worker = class extends WorkerBase {
      constructor(...args) {
        super(...args);
        this.addEventListener("message", ({ data }) => {
          if (data.type === "frame") {
            window.nativeFrames++;
            const f = data.frame;
            window.lastNativeFrame = {
              number: f.number,
              status: f.status,
              width: f.width,
              height: f.height,
              bottomBytes: f.bottom.length,
            };
          }
          if (data.type === "error") window.nativeError = data.message;
        });
      }
      postMessage(data, ...args) {
        if (
          data.type === "frame" &&
          window.testDrive &&
          !window.lastNativeFrame?.status?.fieldReady
        )
          data.input.keys = window.nativeFrames % 60 < 4 ? 1 : 0;
        return super.postMessage(data, ...args);
      }
    };
  });
  await page.goto(process.env.NP_BROWSER_URL || "http://127.0.0.1:4185");
  await page
    .locator("input[type=file]")
    .nth(0)
    .setInputFiles(resolve(root, spec.rom));
  await page.waitForFunction(
    (name) => document.body.textContent.includes(name + " imported."),
    spec.name,
    { timeout: 60000 },
  );
  await page.locator("input[type=file]").nth(1).setInputFiles(fixture);
  await page.waitForFunction(() =>
    document.body.textContent.includes("Save imported into a new slot."),
  );
  await page
    .getByRole("button", { name: `Continue ${spec.name}`, exact: true })
    .click();
  await page.waitForFunction(
    () => window.lastNativeFrame?.status.fieldReady || window.nativeError,
    null,
    { timeout: 180000 },
  );
  assert.equal(await page.evaluate(() => window.nativeError), undefined);
  await page.evaluate(() => {
    window.testDrive = false;
  });
  await page.keyboard.down("ArrowRight");
  await page.waitForTimeout(500);
  await page.keyboard.up("ArrowRight");
  await page.getByRole("button", { name: "Quick save", exact: true }).click();
  await page.waitForFunction(
    () =>
      window.lastNativeFrame?.status.quickSaveResult === 1 ||
      window.nativeError,
    null,
    { timeout: 30000 },
  );
  const saved = await page.evaluate(async () => {
    const db = await new Promise((r, j) => {
      const q = indexedDB.open("nativeplat-wired", 1);
      q.onsuccess = () => r(q.result);
      q.onerror = () => j(q.error);
    });
    return await new Promise((r, j) => {
      const q = db.transaction("slots").objectStore("slots").getAll();
      q.onsuccess = () =>
        r(
          q.result.map((s) => ({
            game: s.game,
            bytes: s.data?.byteLength,
            backup: s.backup?.byteLength,
            updated: s.updated,
          })),
        );
      q.onerror = () => j(q.error);
    });
  });
  assert.ok(
    saved.some(
      (s) =>
        s.game === game &&
        s.bytes === spec.saveBytes &&
        s.backup === spec.saveBytes,
    ),
    "quick save updates IndexedDB and keeps backup",
  );
  await page.screenshot({ path: resolve(out, `${game}-field.png`) });
  const report = await page.evaluate(() => {
    const canvas = document.querySelector(
      'canvas[aria-label="Top game screen"],canvas[aria-label="Game screen"]',
    );
    const p = canvas
        .getContext("2d")
        .getImageData(0, 0, canvas.width, canvas.height).data,
      colors = new Set();
    for (let i = 0; i < p.length; i += 4)
      colors.add(`${p[i]},${p[i + 1]},${p[i + 2]}`);
    return {
      frames: window.nativeFrames,
      glRequests: window.glRequests,
      frame: window.lastNativeFrame,
      colors: colors.size,
    };
  });
  assert.equal(report.glRequests, 0);
  assert.ok(report.colors > 8, "field image must contain rendered content");
  if (spec.generation === 3) {
    assert.equal(report.frame.bottomBytes, 0);
    assert.equal(await page.locator(".touch-screen").isVisible(), false);
  }
  await page.getByRole("button", { name: "Close game", exact: true }).click();
  await page.waitForFunction(
    () => document.querySelector(".live-label")?.textContent !== "RUNNING",
  );
  await page.evaluate(() => {
    window.lastNativeFrame = null;
    window.testDrive = true;
    window.nativeError = undefined;
  });
  await page
    .getByRole("button", { name: `Continue ${spec.name}`, exact: true })
    .click();
  await page.waitForFunction(
    () => window.lastNativeFrame?.status.fieldReady || window.nativeError,
    null,
    { timeout: 180000 },
  );
  assert.equal(await page.evaluate(() => window.nativeError), undefined);
  assert.deepEqual(errors, []);
  Object.assign(report, {
    game,
    saved,
    reloadedField: true,
    coreSha256: originalCore.sha256,
  });
  writeFileSync(
    resolve(out, `${game}.json`),
    JSON.stringify(report, null, 2) + "\n",
  );
  console.log(JSON.stringify(report));
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
  assert.equal(
    manifest.games[game].sha256,
    originalCore.sha256,
    "Core changed during browser qualification",
  );
  manifest.games[game].browserVerification = "passed";
  manifest.games[game].browserNote =
    "WebGL-blocked field, input, raw quick save with backup, and reload passed";
  writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + "\n");
} finally {
  await browser.close();
}
