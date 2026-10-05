// Test the prepared artifacts with LOCAL cartridges, never included in the site.
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, renameSync, existsSync } from "node:fs";
import { resolve } from "node:path";
const web = resolve(import.meta.dirname, ".."),
  root = resolve(web, ".."),
  path = resolve(web, "public/cores/manifest.json");
const manifest = JSON.parse(readFileSync(path, "utf8"));
const selected = process.argv.slice(2);
let failed = false;
for (const game of selected) {
  if (!manifest.games[game]) {
    console.error(`${game}: missing prepared core`);
    failed = true;
  }
}
for (const [game, core] of Object.entries(manifest.games)) {
  if (selected.length && !selected.includes(game)) continue;
  const frames = core.recipe === "asyncify-o3-v2" ? 12000 : 600;
  const rom = resolve(
    root,
    game === "platinum"
      ? "games/platinum/build/rom/pokeplatinum.us.nds"
      : `games/diamond/build/${game}.us/poke${game}.us.nds`,
  );
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
writeFileSync(path + ".tmp", JSON.stringify(manifest, null, 2) + "\n");
renameSync(path + ".tmp", path);

if (failed) process.exitCode = 1;
