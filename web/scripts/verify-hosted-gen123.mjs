// Exercise the public UI with hosted cartridges and a fresh browser profile.
import assert from "node:assert/strict";
import { mkdirSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { chromium } from "playwright";

const base = process.argv[2] || "https://pokeweb.morisoba.moe/";
const out = resolve(
  import.meta.dirname,
  "../../build/hosted-gen123-verification",
);
mkdirSync(out, { recursive: true });
const browser = await chromium.launch({
  headless: true,
  ...(process.env.BROWSER_EXECUTABLE
    ? { executablePath: process.env.BROWSER_EXECUTABLE }
    : {}),
});
const results = [];
try {
  // Model LibreWolf's disabled WebGL without changing the user's browser.
  const blocked = await browser.newContext();
  const blockedPage = await blocked.newPage();
  const dialogs = [],
    heavyRequests = [];
  blockedPage.on("dialog", (d) => {
    dialogs.push(d.message());
    void d.dismiss();
  });
  blockedPage.on("request", (r) => {
    if (/\/(game\.love|love\.wasm)$/.test(r.url())) heavyRequests.push(r.url());
  });
  await blockedPage.addInitScript(() => {
    const original = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function (type, ...args) {
      if (
        type === "webgl" ||
        type === "experimental-webgl" ||
        type === "webgl2"
      )
        return null;
      return original.call(this, type, ...args);
    };
  });
  await blockedPage.goto(base);
  await blockedPage.getByLabel("Generation", { exact: true }).selectOption("1");
  await blockedPage
    .getByRole("button", { name: "Play Red", exact: true })
    .click();
  await blockedPage.waitForFunction(
    () => document.querySelector(".live-label")?.textContent === "ERROR",
  );
  assert.match(
    await blockedPage.getByRole("status").textContent(),
    /WebGL is blocked or unavailable/,
  );
  assert.deepEqual(dialogs, []);
  assert.deepEqual(heavyRequests, []);
  await blockedPage
    .getByRole("button", { name: "Close game", exact: true })
    .click();
  await blockedPage.waitForFunction(
    () => document.querySelector(".live-label")?.textContent === "STANDBY",
  );
  await blocked.close();
  console.log(
    "Blocked WebGL: clear error, no native alert or engine download, close recovers UI",
  );
  const context = await browser.newContext();
  const page = await context.newPage();
  const errors = [],
    packages = [];
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("response", (r) => {
    if (r.url().endsWith(".npc"))
      packages.push({ url: r.url(), status: r.status() });
  });
  await page.addInitScript(() => {
    window.engineEvents = [];
    window.addEventListener("message", (e) => {
      if (
        e.origin === location.origin &&
        e.source ===
          document.querySelector("iframe.engine-screen")?.contentWindow &&
        typeof e.data?.channel === "string"
      )
        window.engineEvents.push(e.data);
    });
  });
  await page.goto(base);
  for (const [game, name, generation] of [
    ["red", "Red", "1"],
    ["crystal", "Crystal", "2"],
    ["emerald", "Emerald", "3"],
  ].filter(
    ([game]) =>
      !process.env.GEN123_TEST_GAMES ||
      process.env.GEN123_TEST_GAMES.split(",").includes(game),
  )) {
    await page
      .getByLabel("Generation", { exact: true })
      .selectOption(generation);
    await page
      .locator("button.cartridge")
      .filter({ has: page.getByRole("heading", { name, exact: true }) })
      .click();
    for (let run = 0; run < 2; run++) {
      await page.evaluate(() => {
        window.engineEvents = [];
      });
      const before = packages.length,
        start = Date.now();
      await page
        .getByRole("button", { name: `Play ${name}`, exact: true })
        .click();
      await page.waitForFunction(
        () =>
          ["RUNNING", "ERROR"].includes(
            document.querySelector(".live-label")?.textContent,
          ),
        null,
        { timeout: 300000 },
      );
      assert.equal(await page.locator(".live-label").textContent(), "RUNNING");
      const bootMs = Date.now() - start;
      await page.waitForTimeout(10000);
      for (let press = 0; press < 5; press++) {
        await page.keyboard.press("KeyZ", { delay: 100 });
        await page.waitForTimeout(700);
      }
      const events = await page.evaluate(() => window.engineEvents);
      assert.ok(
        events.some((e) => e.type === "ready"),
        "Engine must report ready",
      );
      assert.equal(
        events.find((e) => e.type === "error"),
        undefined,
      );
      assert.deepEqual(errors, []);
      const extracted = events.some((e) => e.type === "progress");
      assert.equal(
        packages.length - before,
        run === 0 ? 1 : 0,
        "Only first launch downloads cartridge",
      );
      if (run === 1)
        assert.equal(extracted, false, "Cached launch must skip extraction");
      assert.ok(packages.every((p) => p.status === 200));
      await page.screenshot({
        path: resolve(out, `${game}-${run}.png`),
        fullPage: true,
      });
      await page.getByRole("button", { name: "Pause", exact: true }).click();
      assert.equal(await page.locator(".live-label").textContent(), "PAUSED");
      await page
        .getByRole("button", { name: "Resume", exact: true })
        .last()
        .click();
      await page
        .getByRole("button", { name: "Close game", exact: true })
        .click();
      await page.waitForFunction(
        () => document.querySelector(".live-label")?.textContent === "STANDBY",
      );
      const result = {
        game,
        run,
        bootMs,
        extracted,
        hostedDownloads: packages.length - before,
      };
      results.push(result);
      console.log(JSON.stringify(result));
    }
  }
  writeFileSync(
    resolve(out, "results.json"),
    JSON.stringify({ base, results, errors }, null, 2) + "\n",
  );
} finally {
  await browser.close();
}
