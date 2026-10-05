// Browser smoke test. Supply game + locally owned/source-built cartridge paths.
import { chromium } from "playwright";
import { readFileSync, mkdirSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { createHash } from "node:crypto";
import assert from "node:assert/strict";
const [game, path] = process.argv.slice(2);
if (!game || !path)
  throw Error("Usage: node scripts/verify-gen123.mjs red /path/to/red.gbc");
const rom = readFileSync(resolve(path));
const sha1 = createHash("sha1").update(rom).digest("hex");
const base = process.env.GEN123_TEST_URL || "http://127.0.0.1:4174/";
const out = resolve(import.meta.dirname, "../../build/gen123-verification");
mkdirSync(out, { recursive: true });
const browser = await chromium.launch({
  headless: true,
  ...(process.env.BROWSER_EXECUTABLE
    ? { executablePath: process.env.BROWSER_EXECUTABLE }
    : {}),
  args: ["--autoplay-policy=no-user-gesture-required"],
});
try {
  const context = await browser.newContext();
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  const results = [];
  for (let run = 0; run < 2; run++) {
    await page.goto(base);
    const start = Date.now();
    await page.evaluate(
      ({ game, bytes, sha1 }) => {
        window.engineEvents = [];
        const frame = document.createElement("iframe");
        frame.src = new URL(
          "engines/gen123/player.html#verify",
          location.href,
        ).href;
        frame.style = "width:720px;height:480px;border:0";
        window.addEventListener("message", (e) => {
          if (
            e.origin !== location.origin ||
            e.source !== frame.contentWindow ||
            e.data?.channel !== "verify"
          )
            return;
          window.engineEvents.push(e.data);
          if (e.data.type === "loaded")
            frame.contentWindow.postMessage(
              {
                channel: "verify",
                type: "start",
                game,
                sha1,
                rom: Uint8Array.from(atob(bytes), (c) => c.charCodeAt(0))
                  .buffer,
              },
              location.origin,
            );
        });
        document.body.replaceChildren(frame);
      },
      { game, sha1, bytes: rom.toString("base64") },
    );
    await page.waitForFunction(
      () =>
        window.engineEvents.some(
          (e) => e.type === "ready" || e.type === "error",
        ),
      null,
      { timeout: 300000 },
    );
    const bootMs = Date.now() - start;
    assert.equal(
      await page.evaluate(
        () => window.engineEvents.find((e) => e.type === "error")?.message,
      ),
      undefined,
    );
    await page.waitForTimeout(12000);
    for (let i = 0; i < 5; i++) {
      for (const keys of [["z"], []]) {
        await page.evaluate(
          (keys) =>
            document
              .querySelector("iframe")
              .contentWindow.postMessage(
                {
                  channel: "verify",
                  type: "command",
                  command: { keys, speed: 1, volume: 0.5 },
                },
                location.origin,
              ),
          keys,
        );
        await page.waitForTimeout(keys.length ? 100 : 1200);
      }
    }
    const events = await page.evaluate(() => window.engineEvents);
    assert.equal(events.find((e) => e.type === "error")?.message, undefined);
    assert.deepEqual(errors, []);
    const extracted = events.some((e) => e.type === "progress");
    if (run === 1)
      assert.equal(extracted, false, "Cached launch repeated extraction");
    await page.screenshot({ path: resolve(out, `${game}-${run}.png`) });
    results.push({ game, sha1, run, bootMs, extracted });
  }
  writeFileSync(
    resolve(out, `${game}.json`),
    JSON.stringify(results, null, 2) + "\n",
  );
  console.log(JSON.stringify(results, null, 2));
} finally {
  await browser.close();
}
