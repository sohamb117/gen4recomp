import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { gzipSync } from "node:zlib";
import { unpackCartridge, type HostedCartridge } from "../src/cartridges";
import { identifyRom } from "../src/catalog";

const root = resolve(import.meta.dirname, "../..");
const site = resolve(root, "build/gcp-nativeplat/dist");
const manifest = JSON.parse(readFileSync(resolve(site, "cartridges/manifest.json"), "utf8"));
const results = [];
for (const game of ["diamond", "platinum"]) {
  const entry = manifest.games[game] as HostedCartridge;
  const encrypted = new Uint8Array(readFileSync(resolve(site, "cartridges", entry.file))).buffer;
  const plain = await unpackCartridge(encrypted, entry);
  const compressed = new Uint8Array(gzipSync(new Uint8Array(plain), { level: 9 })).buffer;
  const baseline = [], packaged = [];
  // Alternate old gzip+hash loading and encrypted loading; exclude network and disk.
  for (let i = 0; i < 5; i++) {
    let start = performance.now();
    const decoded = await new Response(new Blob([compressed]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
    await crypto.subtle.digest("SHA-256", decoded);
    await identifyRom(decoded);
    baseline.push(performance.now() - start);
    start = performance.now();
    await identifyRom(await unpackCartridge(encrypted, entry));
    packaged.push(performance.now() - start);
  }
  const median = (values: number[]) => values.sort((a, b) => a - b)[2];
  const result = { game, gzipBytes: compressed.byteLength, encryptedBytes: entry.packedBytes,
    baselineMedianMs: Math.round(median(baseline)), encryptedMedianMs: Math.round(median(packaged)),
    samples: 5, environment: `Node ${process.version}; local WebCrypto/DecompressionStream, network excluded` };
  results.push(result);
  console.log(JSON.stringify(result));
}
writeFileSync(resolve(root, "build/gcp-package-benchmark.json"), JSON.stringify(results, null, 2) + "\n");
