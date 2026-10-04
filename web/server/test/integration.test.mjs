import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { randomBytes, createHash } from "node:crypto";
import pg from "pg";
import { createApp, migrate } from "../app.mjs";
const pool = new pg.Pool({
  connectionString:
    process.env.TEST_DATABASE_URL ||
    "postgres://postgres@127.0.0.1:55439/pokeweb_test",
});
const origin = "https://pokeweb.test";
let app, base;
const ids = [];
const pass = "test-password-only-123";
async function boot() {
  app = createApp({
    pool,
    origins: [origin],
    secureCookies: false,
    authLimit: 1000,
  });
  await new Promise((resolve) => app.listen(0, "127.0.0.1", resolve));
  base = `http://127.0.0.1:${app.address().port}`;
}
async function request(path, method = "GET", body, cookie, headers = {}) {
  const response = await fetch(base + "/api/" + path, {
    method,
    headers: {
      Origin: origin,
      ...(body ? { "Content-Type": "application/json" } : {}),
      ...(cookie ? { Cookie: cookie } : {}),
      ...headers,
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  return {
    status: response.status,
    body: await response.json(),
    cookie: response.headers.get("set-cookie")?.split(";")[0],
    headers: response.headers,
  };
}
async function register() {
  const username = "test_" + randomBytes(6).toString("hex");
  const r = await request("auth/register", "POST", {
    username,
    password: pass,
  });
  assert.equal(r.status, 200);
  ids.push(r.body.user.id);
  return { ...r, username };
}
function save(
  accountId,
  revision = 0,
  game = "platinum",
  data = randomBytes(524288),
) {
  return {
    accountId,
    revision,
    game,
    name: "Test journey",
    data: data.toString("base64"),
    sha256: createHash("sha256").update(data).digest("hex"),
  };
}
before(async () => {
  await migrate(pool);
  await boot();
});
after(async () => {
  await new Promise((resolve) => app.close(resolve));
  await pool.query("DELETE FROM accounts WHERE id=ANY($1::uuid[])", [ids]);
  await pool.end();
});
test("account validation, secure password storage, login, logout, session expiry and CSRF", async () => {
  assert.equal((await request("account")).status, 401);
  assert.equal(
    (
      await request("auth/register", "POST", {
        username: "bad';--",
        password: pass,
      })
    ).status,
    400,
  );
  assert.equal(
    (
      await request("auth/register", "POST", {
        username: "okay",
        password: "short",
      })
    ).status,
    400,
  );
  const a = await register();
  assert.match(a.headers.get("set-cookie"), /HttpOnly; SameSite=Strict/);
  assert.equal(a.headers.get("cache-control"), "no-store");
  const stored = (
    await pool.query("SELECT password_hash FROM accounts WHERE id=$1", [
      a.body.user.id,
    ])
  ).rows[0].password_hash;
  assert.notEqual(stored, pass);
  assert.match(stored, /^[a-f0-9]{32}:[a-f0-9]{128}$/);
  assert.equal(
    (
      await request("auth/register", "POST", {
        username: a.username.toUpperCase(),
        password: pass,
      })
    ).status,
    409,
  );
  assert.equal(
    (
      await request("auth/login", "POST", {
        username: a.username,
        password: "bad-password",
      })
    ).status,
    401,
  );
  assert.equal(
    (
      await request(
        "auth/login",
        "POST",
        { username: a.username, password: pass },
        undefined,
        { Origin: "https://evil.test" },
      )
    ).status,
    403,
  );
  const login = await request("auth/login", "POST", {
    username: a.username.toUpperCase(),
    password: pass,
  });
  assert.equal(login.status, 200);
  assert.equal(
    (await request("account", "GET", undefined, login.cookie)).body.user.id,
    a.body.user.id,
  );
  assert.equal(
    (await request("auth/logout", "POST", {}, login.cookie)).status,
    200,
  );
  assert.equal(
    (await request("account", "GET", undefined, login.cookie)).status,
    401,
  );
  await pool.query(
    "UPDATE sessions SET expires_at=now()-interval '1 second' WHERE account_id=$1",
    [a.body.user.id],
  );
  assert.equal(
    (await request("account", "GET", undefined, a.cookie)).status,
    401,
  );
});
test("one save per account, byte-for-byte download, account isolation and validation", async () => {
  const a = await register(),
    b = await register(),
    payload = save(a.body.user.id);
  assert.equal((await request("save", "PUT", payload)).status, 401);
  assert.equal((await request("save", "PUT", payload, b.cookie)).status, 403);
  assert.equal(
    (await request("save", "PUT", { ...payload, game: "bogus" }, a.cookie))
      .status,
    400,
  );
  assert.equal(
    (await request("save", "PUT", { ...payload, revision: -1 }, a.cookie))
      .status,
    400,
  );
  assert.equal(
    (await request("save", "PUT", { ...payload, data: "AAAA" }, a.cookie))
      .status,
    400,
  );
  assert.equal(
    (
      await request(
        "save",
        "PUT",
        { ...payload, sha256: "0".repeat(64) },
        a.cookie,
      )
    ).status,
    400,
  );
  assert.equal(
    (
      await request(
        "save",
        "PUT",
        save(a.body.user.id, 0, "platinum", Buffer.alloc(524288)),
        a.cookie,
      )
    ).status,
    400,
  );
  const first = await request("save", "PUT", payload, a.cookie);
  assert.equal(first.status, 200);
  assert.equal(first.body.save.revision, 1);
  const downloaded = await request("save", "GET", undefined, a.cookie);
  assert.equal(downloaded.body.data, payload.data);
  assert.equal(downloaded.body.sha256, payload.sha256);
  assert.equal((await request("save", "GET", undefined, b.cookie)).status, 404);
  const second = save(a.body.user.id, 1, "diamond");
  assert.equal(
    (await request("save", "PUT", second, a.cookie)).body.save.revision,
    2,
  );
  assert.equal(
    (
      await pool.query("SELECT count(*) FROM cloud_saves WHERE account_id=$1", [
        a.body.user.id,
      ])
    ).rows[0].count,
    "1",
  );
  assert.equal(
    (await request("save", "GET", undefined, a.cookie)).body.game,
    "diamond",
  );
  assert.equal(
    (
      await request(
        "save",
        "PUT",
        { ...payload, data: "a".repeat(720000) },
        a.cookie,
      )
    ).status,
    413,
  );
});
test("concurrent updates have one winner; retries are idempotent; data survives server restart", async () => {
  const a = await register(),
    one = save(a.body.user.id),
    two = save(a.body.user.id);
  const result = await Promise.all([
    request("save", "PUT", one, a.cookie),
    request("save", "PUT", two, a.cookie),
  ]);
  assert.deepEqual(result.map((r) => r.status).sort(), [200, 409]);
  const winner = result[0].status === 200 ? one : two;
  const retry = await request("save", "PUT", winner, a.cookie);
  assert.equal(retry.status, 200);
  assert.equal(retry.body.save.revision, 1);
  const retries = await Promise.all(
    Array.from({ length: 8 }, () => request("save", "PUT", winner, a.cookie)),
  );
  assert.ok(
    retries.every((r) => r.status === 200 && r.body.save.revision === 1),
  );
  await new Promise((resolve) => app.close(resolve));
  await boot();
  assert.equal(
    (await request("save", "GET", undefined, a.cookie)).body.data,
    winner.data,
  );
});
test("production cookies are secure and repeated login attempts are limited in Postgres", async () => {
  const limited = createApp({
    pool,
    origins: [origin],
    secureCookies: true,
    authLimit: 2,
  });
  await new Promise((resolve) => limited.listen(0, "127.0.0.1", resolve));
  try {
    const username = "test_" + randomBytes(6).toString("hex");
    const call = () =>
      fetch(`http://127.0.0.1:${limited.address().port}/api/auth/register`, {
        method: "POST",
        headers: { Origin: origin, "Content-Type": "application/json" },
        body: JSON.stringify({ username, password: pass }),
      });
    // Isolate rate fixture from earlier test attempts on this loopback address.
    await pool.query("DELETE FROM auth_limits");
    const a = await call();
    assert.equal(a.status, 200);
    ids.push((await a.json()).user.id);
    assert.match(a.headers.get("set-cookie"), /^__Host-pokeweb=.*; Secure$/);
    assert.equal((await call()).status, 409);
    const third = await call();
    assert.equal(third.status, 429);
    assert.equal(third.headers.get("retry-after"), "900");
  } finally {
    await new Promise((resolve) => limited.close(resolve));
  }
});
