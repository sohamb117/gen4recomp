// Verify the deployed API; cleanup is limited to the random accounts created here.
import assert from "node:assert/strict";
import { randomBytes, createHash } from "node:crypto";
import pg from "pg";

const origin = new URL(process.argv[2]).origin;
assert.ok(origin.startsWith("https://"), "Production verification requires HTTPS");
assert.ok(process.env.DATABASE_URL_UNPOOLED, "Cleanup requires the deployment database");
const pool = new pg.Pool({ connectionString: process.env.DATABASE_URL_UNPOOLED, max: 1 });
const ids = [];
const usernames = [];
const digest = (data) => createHash("sha256").update(data).digest("hex");
async function api(path, method = "GET", body, cookie, requestOrigin = origin) {
  const response = await fetch(`${origin}/api/${path}`, {
    method,
    headers: { Origin: requestOrigin, "Content-Type": "application/json", ...(cookie ? { Cookie: cookie } : {}) },
    body: body ? JSON.stringify(body) : undefined,
    signal: AbortSignal.timeout(30000),
  });
  assert.match(response.headers.get("cache-control") || "", /no-store/);
  return { status: response.status, body: await response.json(), cookie: response.headers.get("set-cookie")?.split(";")[0], headers: response.headers };
}
async function register() {
  const username = "deploy_" + randomBytes(6).toString("hex");
  const password = randomBytes(24).toString("hex");
  usernames.push(username);
  const result = await api("auth/register", "POST", { username, password });
  assert.equal(result.status, 200);
  ids.push(result.body.user.id);
  assert.match(result.headers.get("set-cookie"), /^__Host-pokeweb=.*HttpOnly; SameSite=Strict;.*Secure$/);
  return { ...result, username, password };
}
try {
  assert.equal((await api("health")).status, 200);
  assert.equal((await api("account")).status, 401);
  const a = await register(), b = await register();
  const data = randomBytes(524288);
  const payload = { accountId: a.body.user.id, game: "platinum", name: "Deployment check", revision: 0, data: data.toString("base64"), sha256: digest(data) };
  assert.equal((await api("save", "PUT", payload, b.cookie)).status, 403);
  assert.equal((await api("save", "PUT", payload, a.cookie, "https://invalid.example")).status, 403);
  assert.equal((await api("save", "PUT", { ...payload, sha256: "0".repeat(64) }, a.cookie)).status, 400);
  assert.equal((await api("save", "PUT", payload, a.cookie)).body.save.revision, 1);
  assert.equal((await api("save", "GET", undefined, b.cookie)).status, 404);
  assert.equal((await api("auth/logout", "POST", {}, a.cookie)).status, 200);
  assert.equal((await api("account", "GET", undefined, a.cookie)).status, 401);
  const login = await api("auth/login", "POST", { username: a.username, password: a.password });
  assert.equal(login.status, 200);
  const restored = await api("save", "GET", undefined, login.cookie);
  assert.equal(restored.status, 200);
  assert.deepEqual(Buffer.from(restored.body.data, "base64"), data);
  const replacement = { ...payload, game: "diamond", revision: 1 };
  assert.equal((await api("save", "PUT", replacement, login.cookie)).body.save.revision, 2);
  assert.equal((await api("save", "PUT", payload, login.cookie)).status, 409);
  const row = (await pool.query("SELECT game,revision,data FROM cloud_saves WHERE account_id=$1", [a.body.user.id])).rows;
  assert.equal(row.length, 1);
  assert.equal(row[0].game, "diamond");
  assert.equal(row[0].revision, 2);
  assert.deepEqual(row[0].data, data);
  console.log("Production API passed: secure sessions, login/logout, account isolation, validation, conflict protection, one save per account, and exact Neon byte readback.");
} finally {
  // Match random fixture names too, in case a response was lost after registration.
  await pool.query("DELETE FROM accounts WHERE id=ANY($1::uuid[]) OR username=ANY($2::text[])", [ids, usernames]);
  await pool.query("DELETE FROM auth_limits WHERE key=ANY($1::text[])", [usernames.map((name) => digest(`user:${name}`))]);
  await pool.end();
}
