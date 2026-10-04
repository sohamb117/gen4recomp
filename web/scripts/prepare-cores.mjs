import {
  existsSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
  renameSync,
} from "node:fs";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
const web = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const root = resolve(web, "..");
const wabt =
  process.env.WABT_BIN || resolve(root, ".cache/toolchains/wabt/bin");
const output = resolve(web, "public/cores");
const temp = resolve(root, "build/web-cores");
const cachedOptimizer = resolve(temp, "binaryen-version_123/bin/wasm-opt");
const nativeOptimizer =
  process.env.NP_WASM_OPT ||
  (existsSync(cachedOptimizer) ? cachedOptimizer : undefined);
mkdirSync(output, { recursive: true });
mkdirSync(temp, { recursive: true });
const games = {
  diamond: "games/diamond/build/pc-wasm/pokediamond.wasm",
  pearl: "games/diamond/build/pc-wasm/pokepearl.wasm",
  platinum: "games/platinum/build/pc-wasm/pokeplatinum.wasm",
};
const forceOptimize = process.argv.includes("--optimize");
const noOptimize = process.argv.includes("--no-optimize");
const selected = process.argv
  .slice(2)
  .filter((arg) => !["--optimize", "--no-optimize"].includes(arg));
const manifestPath = resolve(output, "manifest.json");
const previous = existsSync(manifestPath)
  ? JSON.parse(readFileSync(manifestPath, "utf8"))
  : null;
const manifest =
  selected.length && previous
    ? previous
    : { version: 1, abi: 2, runtime: "asyncify", games: {} };
function run(cmd, args) {
  execFileSync(cmd, args, { stdio: "inherit" });
}
for (const [game, path] of Object.entries(games)) {
  const optimize =
    !noOptimize &&
    (forceOptimize ||
      game === "platinum" ||
      previous?.games[game]?.recipe === "asyncify-o3-v2");
  const source = resolve(
    process.env[`NP_GUEST_WASM_${game}`] || resolve(root, path),
  );
  if (selected.length && !selected.includes(game)) continue;
  if (!existsSync(source)) {
    console.log(`${game}: no core built; omitted`);
    continue;
  }
  const sha256 = createHash("sha256")
    .update(readFileSync(source))
    .digest("hex");
  const filename = `${game}-${sha256.slice(0, 12)}-web-${optimize ? "v2-o3" : "v1"}.wasm`;
  const dest = resolve(output, filename);
  console.log(`Preparing ${game} (source stays untouched)…`);
  if (!existsSync(dest)) {
    const wat = resolve(temp, `${game}.wat`),
      patched = resolve(temp, `${game}.wasm`);
    run(resolve(wabt, "wasm2wat"), [source, "-o", wat]);
    run("python3", [resolve(web, "scripts/prepare-wat.py"), wat]);
    run(resolve(wabt, "wat2wasm"), [wat, "--debug-names", "-o", patched]);
    const args = [
      patched,
      ...(optimize ? ["-O3"] : []),
      "--asyncify",
      "--pass-arg=asyncify-imports@np_host.vblank,np_host.fiber_switch",
      ...(optimize ? ["-O3"] : []),
      "--strip-debug",
      "-o",
      `${dest}.tmp`,
    ];
    if (nativeOptimizer) run(resolve(nativeOptimizer), args);
    else
      run(process.execPath, [
        resolve(web, "node_modules/binaryen/bin/wasm-opt"),
        ...args,
      ]);
    run(resolve(wabt, "wasm-validate"), [`${dest}.tmp`]);
    renameSync(`${dest}.tmp`, dest);
  }
  const bytes = readFileSync(dest);
  const prior = previous?.games[game];
  manifest.games[game] = {
    file: filename,
    bytes: bytes.length,
    sourceSha256: sha256,
    recipe: optimize ? "asyncify-o3-v2" : "asyncify-v1",
    sha256: createHash("sha256").update(bytes).digest("hex"),
  };
  if (prior?.sha256 === manifest.games[game].sha256 && prior.verification) {
    manifest.games[game].verification = prior.verification;
    manifest.games[game].note = prior.note;
  }
}
writeFileSync(
  resolve(output, "manifest.json.tmp"),
  JSON.stringify(manifest, null, 2) + "\n",
);
renameSync(
  resolve(output, "manifest.json.tmp"),
  resolve(output, "manifest.json"),
);
console.log("Browser cores prepared. No cartridges or saves were copied.");
