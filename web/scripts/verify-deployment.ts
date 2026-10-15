import assert from "node:assert/strict";
import { loadCartridges, fetchCartridge } from "../src/cartridges";
import { GuestHost } from "../src/runtime/host";
import { games, isNative, type CoreManifest } from "../src/catalog";
import { readFileSync } from "node:fs";

const base = new URL(process.argv[2] || "http://127.0.0.1:8088/").href;
const index = await fetch(base);
assert.equal(index.status, 200);
assert.match(index.headers.get("content-type") || "", /text\/html/);
assert.match(
  index.headers.get("content-security-policy") || "",
  /wasm-unsafe-eval/,
);
assert.match(await index.text(), /assets\/index-/);
if (process.env.NP_WITH_SAVE_API === "1") {
  const health = await fetch(new URL("api/health", base));
  assert.equal(health.status, 200, "Cloud-save database must be ready");
  assert.deepEqual(await health.json(), { ok: true });
  const anonymous = await fetch(new URL("api/account", base));
  assert.equal(
    anonymous.status,
    401,
    "Accounts require sign-in; the public site does not",
  );
  assert.match(anonymous.headers.get("cache-control") || "", /no-store/);
}
const catalog = await loadCartridges(base);
const localCores = JSON.parse(
  readFileSync(
    new URL("../public/cores/manifest.json", import.meta.url),
    "utf8",
  ),
) as CoreManifest;
const expectedNative = Object.entries(localCores.games)
  .filter(([, c]) => c?.verification === "passed")
  .map(([id]) => id);
const expectedGames = games.filter(
  (g) => !isNative(g.id) || g.id === "emerald" || expectedNative.includes(g.id),
);
assert.deepEqual(
  Object.keys(catalog).sort(),
  expectedGames.map((g) => g.id).sort(),
);
// Previously published plaintext URLs must no longer be served (including gzip).
for (const file of [
  "diamond-e29bc6ebe431d7a6.nds",
  "platinum-fbce4c4def0c7797.nds",
  "red.gb",
  "yellow.gbc",
  "emerald.gba",
  "test.pws",
]) {
  for (const suffix of ["", ".gz"]) {
    const response = await fetch(new URL(`cartridges/${file}${suffix}`, base), {
      method: "HEAD",
    });
    assert.equal(
      response.status,
      404,
      `Plaintext cartridge still available: ${file}${suffix}`,
    );
  }
}
const manifest = (await fetch(new URL("cores/manifest.json", base)).then((r) =>
  r.json(),
)) as CoreManifest;
assert.deepEqual(Object.keys(manifest.games).sort(), expectedNative.sort());
for (const game of expectedNative)
  assert.deepEqual(
    manifest.games[game as keyof typeof manifest.games],
    localCores.games[game as keyof typeof localCores.games],
  );
const engine = await fetch(new URL("engines/gen123/manifest.json", base)).then(
  (r) => r.json(),
);
const expectedEngine = JSON.parse(
  readFileSync(
    new URL("../public/engines/gen123/manifest.json", import.meta.url),
    "utf8",
  ),
);
assert.deepEqual(
  engine,
  expectedEngine,
  "Deployed engine must match the prepared release",
);
for (const [file, expected] of Object.entries(engine.files) as [
  string,
  { bytes: number; sha256: string },
][]) {
  const response = await fetch(new URL(`engines/gen123/${file}`, base));
  assert.equal(response.status, 200);
  assert.doesNotMatch(response.headers.get("cache-control") || "", /immutable/);
  const bytes = await response.arrayBuffer();
  assert.equal(bytes.byteLength, expected.bytes);
  assert.equal(
    Buffer.from(await crypto.subtle.digest("SHA-256", bytes)).toString("hex"),
    expected.sha256,
  );
}
console.log(
  "Gen 1–3: all eight deployed engine/source/license files match the prepared release",
);
for (const { id: game, hash: sha1 } of expectedGames) {
  const data = await fetchCartridge(base, game, catalog[game]!, () => {});
  console.log(
    `${game}: downloaded encrypted package, decrypted and verified ${data.byteLength} cartridge bytes`,
  );
  assert.equal(
    Buffer.from(await crypto.subtle.digest("SHA-1", data)).toString("hex"),
    sha1,
  );
  if (!isNative(game) || !manifest.games[game]) continue;
  const core = manifest.games[game]!;
  const response = await fetch(new URL(`cores/${core.file}`, base));
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") || "", /application\/wasm/);
  assert.match(response.headers.get("cache-control") || "", /immutable/);
  const wasm = await response.arrayBuffer();
  assert.equal(wasm.byteLength, core.bytes);
  const hash = Buffer.from(
    await crypto.subtle.digest("SHA-256", wasm),
  ).toString("hex");
  assert.equal(hash, core.sha256);
  const host = new GuestHost(
    new Uint8Array(data),
    undefined,
    () => {},
    () => {},
  );
  await host.load(wasm);
  let lit = false;
  for (let i = 0; i < 600; i++) {
    const frame = host.runFrame();
    assert.equal(frame.width, 256);
    assert.equal(frame.height, 192);
    lit ||= frame.top.some((byte, j) => j % 4 !== 3 && byte !== 0);
  }
  assert.ok(lit, `${game} rendered no picture`);
  console.log(
    `${game}: deployed core integrity and 600-frame runtime check passed`,
  );
}
