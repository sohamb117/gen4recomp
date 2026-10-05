import { readFileSync, readdirSync, copyFileSync, mkdirSync } from "node:fs";
import { resolve } from "node:path";
import { createHash } from "node:crypto";
const required = [
  "game.love",
  "love.js",
  "love.wasm",
  "player.html",
  "player.js",
  "cache.js",
  "GEN123-LICENSE.txt",
  "LOVE-LICENSE.txt",
];
/** Stage only manifest-verified, explicitly named runtime outputs. No cartridge inputs. */
export function stageGen123(source, destination) {
  const manifest = JSON.parse(
    readFileSync(resolve(source, "manifest.json"), "utf8"),
  );
  if (manifest.version !== 1 || !/^[a-f0-9]{40}$/.test(manifest.source))
    throw Error("Invalid Gen 1–3 manifest");
  if (
    JSON.stringify(Object.keys(manifest.files).sort()) !==
    JSON.stringify([...required].sort())
  )
    throw Error("Unexpected Gen 1–3 output");
  mkdirSync(destination, { recursive: true });
  for (const name of required) {
    const data = readFileSync(resolve(source, name)),
      expected = manifest.files[name];
    if (
      data.length !== expected.bytes ||
      createHash("sha256").update(data).digest("hex") !== expected.sha256
    )
      throw Error(`Gen 1–3 integrity mismatch: ${name}`);
    if (resolve(source) !== resolve(destination))
      copyFileSync(resolve(source, name), resolve(destination, name));
  }
  if (resolve(source) !== resolve(destination))
    copyFileSync(
      resolve(source, "manifest.json"),
      resolve(destination, "manifest.json"),
    );
  for (const name of readdirSync(destination))
    if (
      !required.includes(name.replace(/\.(gz|br)$/, "")) &&
      name !== "manifest.json" &&
      name !== "manifest.json.gz" &&
      name !== "manifest.json.br"
    )
      throw Error(`Unexpected engine asset: ${name}`);
}
