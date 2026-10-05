// Experimental local renderer qualification; WebGL is deliberately unavailable.
import { chromium } from "playwright";
import { readFileSync, mkdirSync, writeFileSync } from "node:fs";
import { resolve, basename } from "node:path";
import assert from "node:assert/strict";
const root = resolve(import.meta.dirname, "../.."),
  engine = resolve(root, "build/gen123-canvas"),
  out = resolve(root, "build/canvas-verification");
mkdirSync(out, { recursive: true });
const browser = await chromium.launch({
  headless: true,
  executablePath:
    process.env.BROWSER_EXECUTABLE ||
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
});
const game = process.argv[2] || "Red";
try {
  const page = await browser.newPage({
    viewport: { width: 1280, height: 1000 },
  });
  page.on("console", (m) => {
    if (
      m.type() === "error" ||
      m.text().includes("Error:") ||
      m.text().includes("CANVAS_STATE:")
    )
      console.log(m.type(), m.text());
  });
  page.on("pageerror", (e) => console.log("PAGEERROR", e.message));
  page.on("dialog", async (d) => {
    console.log("DIALOG", d.message());
    await d.dismiss();
  });
  await page.route("**/engines/gen123/*", async (route) => {
    const name = basename(new URL(route.request().url()).pathname);
    await route.fulfill({
      body: readFileSync(resolve(engine, name)),
      contentType: name.endsWith(".js")
        ? "application/javascript"
        : name.endsWith(".html")
          ? "text/html"
          : name.endsWith(".json")
            ? "application/json"
            : name.endsWith(".wasm")
              ? "application/wasm"
              : "application/octet-stream",
    });
  });
  await page.addInitScript(() => {
    window.glRequests = 0;
    const original = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function (type, ...args) {
      if (type.includes("webgl")) {
        window.glRequests++;
        return null;
      }
      return original.call(this, type, ...args);
    };
  });
  await page.goto("http://127.0.0.1:4177");
  await page
    .getByLabel("Generation", { exact: true })
    .selectOption(
      ["Gold", "Silver", "Crystal"].includes(game)
        ? "2"
        : ["Emerald", "FireRed", "LeafGreen"].includes(game)
          ? "3"
          : "1",
    );
  await page
    .locator("button.cartridge")
    .filter({ has: page.getByRole("heading", { name: game, exact: true }) })
    .click();
  await page.getByRole("button", { name: `Play ${game}`, exact: true }).click();
  await page.waitForFunction(
    () =>
      ["RUNNING", "ERROR"].includes(
        document.querySelector(".live-label")?.textContent,
      ),
    null,
    { timeout: 300000 },
  );
  const status = await page.locator(".live-label").textContent();
  if (status === "ERROR")
    console.log("NOTICE", await page.getByRole("status").textContent());
  assert.equal(status, "RUNNING");
  const frame = page.frames().find((f) => f.url().includes("/player.html"));
  await page.waitForTimeout(5000);
  for (let i = 0; i < 8; i++) {
    await page.keyboard.press("KeyZ", { delay: 100 });
    await page.waitForTimeout(1000);
  }
  if (process.env.GEN123_FIELD === "1") {
    await page
      .getByRole("button", { name: "Fast-forward 2×", exact: true })
      .click();
    for (let i = 0; i < 160; i++) {
      await page.keyboard.press("KeyZ", { delay: 80 });
      await page.waitForTimeout(180);
      if (i % 20 === 0) await page.keyboard.press("Enter", { delay: 100 });
    }
    for (let i = 0; i < 5; i++) {
      await page.keyboard.press("KeyX", { delay: 100 });
      await page.waitForTimeout(250);
    }
    await page.keyboard.press("ArrowDown", { delay: 600 });
    await page.waitForTimeout(1500);
    await page.getByRole("button", { name: "Quick save", exact: true }).click();
    await page.waitForTimeout(1500);
    const saves = await page.evaluate(
      () =>
        new Promise((resolve) => {
          const r = indexedDB.open("nativeplat-wired");
          r.onsuccess = () => {
            const t = r.result
              .transaction("slots")
              .objectStore("slots")
              .getAll();
            t.onsuccess = () =>
              resolve(
                t.result.map((s) => ({
                  game: s.game,
                  bytes: s.data?.byteLength,
                })),
              );
          };
        }),
    );
    console.log("SAVES", saves);
    assert.ok(saves.some((s) => s.game === "red" && s.bytes > 0));
  }
  await page.locator("iframe").scrollIntoViewIfNeeded();
  await page.screenshot({ path: resolve(out, game + ".png"), fullPage: true });
  const metrics = await frame.evaluate(() => {
    const r = window.canvasRenderer,
      c = document.querySelector("canvas"),
      ctx = c.getContext("2d");
    const p = ctx.getImageData(0, 0, c.width, c.height).data,
      colors = new Set();
    for (let i = 0; i < p.length; i += 4)
      colors.add(`${p[i]},${p[i + 1]},${p[i + 2]},${p[i + 3]}`);
    return {
      glRequests: window.glRequests,
      frames: r?.frames,
      renderMs: r?.frameMs,
      colors: colors.size,
      dimensions: [c.width, c.height],
      state: r?.state,
      commands: r?.lastCommands.slice(-20),
    };
  });
  console.log(
    JSON.stringify({ ...metrics, renderMs: metrics.renderMs?.slice(-5) }),
  );
  writeFileSync(resolve(out, game + ".json"), JSON.stringify(metrics, null, 2));
  assert.equal(metrics.glRequests, 0);
  assert.ok(metrics.frames > 100);
  assert.ok(metrics.colors > 1, "Must render a nonblank game picture");
} finally {
  await browser.close();
}
