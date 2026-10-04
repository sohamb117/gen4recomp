import { createHash } from "node:crypto";
import {
  unlinkSync,
  readFileSync,
  writeFileSync,
  readdirSync,
  statSync,
  copyFileSync,
} from "node:fs";
import { resolve, relative } from "node:path";
import { gzipSync, brotliCompressSync, constants } from "node:zlib";
import { execFileSync } from "node:child_process";
const root = resolve(import.meta.dirname, "../.."),
  dist = resolve(root, "web/dist");
const manifest = JSON.parse(
  readFileSync(resolve(dist, "cores/manifest.json"), "utf8"),
);
if (!Object.keys(manifest.games).length)
  throw Error("No game cores in release");
for (const [game, core] of Object.entries(manifest.games)) {
  const data = readFileSync(resolve(dist, "cores", core.file));
  if (createHash("sha256").update(data).digest("hex") !== core.sha256)
    throw Error(`${game} integrity mismatch`);
}
// Vite copies public/ verbatim; exclude stale cores from this release only.
const allowed = new Set(Object.values(manifest.games).map((core) => core.file));
for (const name of readdirSync(resolve(dist, "cores"))) {
  if (
    name !== "manifest.json" &&
    !allowed.has(name.replace(/\.(br|gz)$/, ""))
  ) {
    const path = resolve(dist, "cores", name);
    if (statSync(path).isFile()) unlinkSync(path);
  }
}
copyFileSync(
  resolve(root, "games/platinum/pc/LICENSE"),
  resolve(dist, "LICENSE.txt"),
);
copyFileSync(
  resolve(root, "web/THIRD-PARTY.txt"),
  resolve(dist, "THIRD-PARTY.txt"),
);
const sums = [];
function walk(dir) {
  for (const name of readdirSync(dir)) {
    const file = resolve(dir, name);
    if (statSync(file).isDirectory()) {
      walk(file);
      continue;
    }
    const rel = relative(dist, file);
    if (/\.(nds|sav|dsv|pgt|pcd)$/i.test(name))
      throw Error(`Private cartridge/save in distribution: ${rel}`);
    if (/\.(wasm|js|css|html|json|svg|txt)$/.test(name)) {
      const bytes = readFileSync(file);
      sums.push(`${createHash("sha256").update(bytes).digest("hex")}  ${rel}`);
      if (bytes.length > 1024) {
        writeFileSync(file + ".gz", gzipSync(bytes, { level: 9 }));
        writeFileSync(
          file + ".br",
          brotliCompressSync(bytes, {
            params: { [constants.BROTLI_PARAM_QUALITY]: 6 },
          }),
        );
      }
    }
  }
}
walk(dist);
writeFileSync(resolve(dist, "SHA256SUMS"), sums.sort().join("\n") + "\n");
const archive = resolve(root, "build/nativeplat-web.tar.gz");
execFileSync("tar", ["-czf", archive, "-C", dist, "."]);
console.log(`Static deployment: ${dist}\nArchive: ${archive}`);
