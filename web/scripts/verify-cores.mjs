import { createHash } from "node:crypto";
import { updateCoreManifest } from "./core-manifest.mjs";
import { nativeGames } from "../shared/native-games.mjs";
// Test the prepared artifacts with LOCAL cartridges, never included in the site.
import { execFileSync } from "node:child_process";
import { readFileSync, existsSync } from "node:fs";
import { resolve } from "node:path";
const web = resolve(import.meta.dirname, ".."),
  root = resolve(web, ".."),
  path = resolve(web, "public/cores/manifest.json");
const manifest = JSON.parse(readFileSync(path, "utf8"));
const selected = process.argv.slice(2).filter((arg) => arg !== "--force");
const runtimeSha256 = createHash("sha256")
  .update(readFileSync(resolve(web, "src/runtime/host.ts")))
  .update(readFileSync(resolve(web, "src/runtime/abi.ts")))
  .digest("hex");
let failed = false;
for (const game of selected) {
  if (!manifest.games[game]) {
    console.error(`${game}: missing prepared core`);
    failed = true;
  }
}
for (const [game, core] of Object.entries(manifest.games)) {
  if (selected.length && !selected.includes(game)) continue;
  const bytes = readFileSync(resolve(web, "public/cores", core.file));
  if (createHash("sha256").update(bytes).digest("hex") !== core.sha256)
    throw Error(`${game}: artifact hash mismatch`);
  if (
    !process.argv.includes("--force") &&
    core.verification === "passed" &&
    core.browserVerification === "passed" &&
    core.runtimeSha256 === runtimeSha256
  ) {
    console.log(
      `${game}: reusing matching core/runtime and browser qualification`,
    );
    continue;
  }
  const frames = core.recipe === "asyncify-o3-v2" ? 12000 : 600;
  const rom = resolve(root, nativeGames[game].rom);
  if (!existsSync(rom)) {
    core.verification = "unverified";
    console.error(`${game}: missing local verification cartridge`);
    failed = true;
    continue;
  }
  try {
    execFileSync(
      process.execPath,
      [
        "--liftoff-only",
        "--import",
        "tsx",
        resolve(web, "scripts/smoke.ts"),
        game,
        String(frames),
      ],
      {
        cwd: web,
        timeout: frames > 600 ? 600000 : 60000,
        stdio: ["ignore", "pipe", "pipe"],
      },
    );
    core.verification = "passed";
    core.runtimeSha256 = runtimeSha256;
    core.note = `${frames.toLocaleString("en-US")}-frame browser-runtime smoke passed`;
    console.log(`${game}: passed`);
  } catch (e) {
    failed = true;
    core.verification = "blocked";
    core.note =
      e.code === "ETIMEDOUT"
        ? `Core did not finish the ${frames}-frame check within its time budget.`
        : "Core failed the browser-runtime boot check.";
    console.log(`${game}: ${core.note}`);
  }
}
updateCoreManifest(path, (latest) => {
  for (const [game, core] of Object.entries(manifest.games)) {
    if (selected.length && !selected.includes(game)) continue;
    if (latest.games[game]?.sha256 !== core.sha256)
      throw Error(`${game}: core changed during verification`);
    Object.assign(latest.games[game], {
      verification: core.verification,
      note: core.note,
      runtimeSha256: core.runtimeSha256,
    });
  }
});
if (failed) process.exitCode = 1;
