// Local-only deployment smoke. Deletes only the random accounts it creates.
import assert from "node:assert/strict";
import { randomBytes, createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { execFileSync } from "node:child_process";
import { GuestHost } from "../src/runtime/host";
import { gameOptions, initialSettings } from "../src/runtime/settings";
const base = "http://127.0.0.1:8088";
const root = resolve(import.meta.dirname, "../..");
const ids: string[] = [];
const hash = (data: Uint8Array) =>
  createHash("sha256").update(data).digest("hex");
async function api(
  path: string,
  method = "GET",
  body?: unknown,
  cookie?: string,
) {
  const r = await fetch(`${base}/api/${path}`, {
    method,
    headers: {
      Origin: base,
      "Content-Type": "application/json",
      ...(cookie ? { Cookie: cookie } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  return {
    status: r.status,
    body: await r.json(),
    cookie: r.headers.get("set-cookie")?.split(";")[0],
  };
}
async function account() {
  const username = "check_" + randomBytes(6).toString("hex"),
    password = randomBytes(24).toString("hex");
  const r = await api("auth/register", "POST", { username, password });
  assert.equal(r.status, 200);
  assert.match(r.body.user.id, /^[0-9a-f-]{36}$/);
  ids.push(r.body.user.id);
  return { ...r, username, password };
}
try {
  assert.equal((await fetch(base)).status, 200);
  assert.equal((await api("health")).status, 200);
  assert.equal((await api("account")).status, 401);
  const a = await account(),
    b = await account();
  const data = process.env.NP_TEST_SAVE
    ? readFileSync(process.env.NP_TEST_SAVE)
    : randomBytes(524288);
  assert.equal(data.length, 524288);
  const payload = {
    accountId: a.body.user.id,
    game: "platinum",
    name: "Validation fixture",
    revision: 0,
    sha256: hash(data),
    data: data.toString("base64"),
  };
  assert.equal((await api("save", "PUT", payload, b.cookie)).status, 403);
  assert.equal(
    (await api("save", "PUT", payload, a.cookie)).body.save.revision,
    1,
  );
  assert.equal((await api("save", "GET", undefined, b.cookie)).status, 404);
  await api("auth/logout", "POST", {}, a.cookie);
  assert.equal((await api("save", "GET", undefined, a.cookie)).status, 401);
  const login = await api("auth/login", "POST", {
    username: a.username,
    password: a.password,
  });
  const loaded = await api("save", "GET", undefined, login.cookie);
  assert.equal(loaded.status, 200);
  const restored = Buffer.from(loaded.body.data, "base64");
  assert.deepEqual(restored, data);
  if (process.env.NP_TEST_SAVE) {
    const manifest = JSON.parse(
      readFileSync(resolve(root, "web/public/cores/manifest.json"), "utf8"),
    );
    let gameSave: Uint8Array | undefined;
    const host = new GuestHost(
      new Uint8Array(
        readFileSync(
          resolve(root, "games/platinum/build/rom/pokeplatinum.us.nds"),
        ),
      ),
      new Uint8Array(restored),
      (bytes) => {
        gameSave = new Uint8Array(bytes);
      },
      () => {},
    );
    await host.load(
      new Uint8Array(
        readFileSync(
          resolve(root, "web/public/cores", manifest.games.platinum.file),
        ),
      ),
    );
    let frame = host.runFrame();
    for (let i = 0; i < 5000 && !frame.status.fieldReady; i++) {
      host.input.keys = i % 60 < 4 ? 1 : 0;
      frame = host.runFrame();
    }
    assert.ok(
      frame.status.fieldReady,
      "Postgres round-trip save must reach the saved field",
    );
    host.input.keys = 0;
    host.options = gameOptions(initialSettings, "platinum", 1);
    for (let i = 0; i < 100 && frame.status.quickSaveSequence !== 1; i++)
      frame = host.runFrame();
    assert.equal(frame.status.quickSaveResult, 1);
    assert.ok(gameSave);
    const next = Buffer.from(gameSave);
    assert.equal(
      (
        await api(
          "save",
          "PUT",
          {
            ...payload,
            revision: 1,
            sha256: hash(next),
            data: next.toString("base64"),
          },
          login.cookie,
        )
      ).body.save.revision,
      2,
    );
    assert.deepEqual(
      Buffer.from(
        (await api("save", "GET", undefined, login.cookie)).body.data,
        "base64",
      ),
      next,
    );
    console.log(
      "Platinum loaded the Postgres round-trip save, reached the field, and persisted a new quick save.",
    );
  }
  console.log(
    "Local packaged app: registration, login, logout, isolation, one-save revisions and byte-for-byte restore passed.",
  );
} finally {
  if (ids.length)
    execFileSync(
      "docker",
      [
        "compose",
        "--env-file",
        resolve(root, "build/pokeweb-local.env"),
        "-f",
        resolve(root, "web/deploy/compose.local.yml"),
        "exec",
        "-T",
        "db",
        "psql",
        "-U",
        "pokeweb",
        "-d",
        "pokeweb",
        "-v",
        "ON_ERROR_STOP=1",
        "-c",
        `DELETE FROM accounts WHERE id IN (${ids.map((id) => `'${id}'`).join(",")})`,
      ],
      { stdio: "pipe" },
    );
}
