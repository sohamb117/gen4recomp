import { createServer } from "node:http";
import {
  randomBytes,
  randomUUID,
  createHash,
  scrypt,
  timingSafeEqual,
} from "node:crypto";
import { promisify } from "node:util";
import { readFile } from "node:fs/promises";
const derive = promisify(scrypt);
const hash = (value) => createHash("sha256").update(value).digest("hex");
const SIZE = 512 * 1024;
const DAYS = 30 * 86400;
class HttpError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}
const fail = (status, message) => {
  throw new HttpError(status, message);
};
export async function migrate(pool) {
  const db = await pool.connect();
  try {
    await db.query("BEGIN");
    await db.query("SELECT pg_advisory_xact_lock(710442819)");
    await db.query(
      await readFile(new URL("./schema.sql", import.meta.url), "utf8"),
    );
    await db.query("COMMIT");
  } catch (e) {
    await db.query("ROLLBACK");
    throw e;
  } finally {
    db.release();
  }
}
async function passwordHash(password, salt = randomBytes(16).toString("hex")) {
  const key = await derive(password, salt, 64, {
    N: 32768,
    r: 8,
    p: 1,
    maxmem: 64 * 1024 * 1024,
  });
  return `${salt}:${key.toString("hex")}`;
}
async function passwordMatches(password, stored) {
  const [salt, expected] = stored.split(":");
  const actual = (await passwordHash(password, salt)).split(":")[1];
  return timingSafeEqual(
    Buffer.from(expected, "hex"),
    Buffer.from(actual, "hex"),
  );
}
function credentials(body) {
  const username =
    typeof body.username === "string" ? body.username.trim().toLowerCase() : "";
  if (!/^[a-z0-9_]{3,24}$/.test(username))
    fail(400, "Username must be 3–24 letters, numbers or underscores.");
  if (
    typeof body.password !== "string" ||
    body.password.length < 8 ||
    body.password.length > 128
  )
    fail(400, "Password must be 8–128 characters.");
  return { username, password: body.password };
}
async function jsonBody(req, limit) {
  if (req.headers["content-type"]?.split(";")[0] !== "application/json")
    fail(415, "Expected JSON.");
  if (Number(req.headers["content-length"]) > limit)
    fail(413, "Request too large.");
  const chunks = [];
  let length = 0;
  for await (const chunk of req) {
    length += chunk.length;
    if (length > limit) fail(413, "Request too large.");
    chunks.push(chunk);
  }
  try {
    const body = JSON.parse(Buffer.concat(chunks).toString("utf8"));
    if (!body || typeof body !== "object" || Array.isArray(body)) throw Error();
    return body;
  } catch {
    fail(400, "Invalid JSON.");
  }
}
function metadata(row) {
  return row
    ? {
        game: row.game,
        name: row.name,
        revision: row.revision,
        updated: new Date(row.updated_at).getTime(),
        sha256: row.sha256,
      }
    : null;
}
export function createApp({
  pool,
  origins,
  secureCookies = true,
  trustProxy = false,
  authLimit = 30,
}) {
  const cookieName = secureCookies ? "__Host-pokeweb" : "pokeweb";
  // Fixed dummy hash ensures unknown users still pay the password verification cost.
  const dummy = passwordHash("not-a-real-password", "0".repeat(32));
  const cookie = (token, age = DAYS) =>
    `${cookieName}=${token}; Path=/; HttpOnly; SameSite=Strict; Max-Age=${age}${secureCookies ? "; Secure" : ""}`;
  const tokenFrom = (req) => {
    const value = (req.headers.cookie || "")
      .split(";")
      .map((x) => x.trim())
      .find((x) => x.startsWith(cookieName + "="))
      ?.slice(cookieName.length + 1);
    return /^[a-f0-9]{64}$/.test(value || "") ? value : null;
  };
  async function account(req) {
    const token = tokenFrom(req);
    if (!token) fail(401, "Sign in to use cloud saves.");
    const { rows } = await pool.query(
      `SELECT a.id, a.username FROM sessions s JOIN accounts a ON a.id=s.account_id
      WHERE s.token_hash=$1 AND s.expires_at > now()`,
      [hash(token)],
    );
    if (!rows[0]) fail(401, "Your session expired. Sign in again.");
    return rows[0];
  }
  async function rateLimit(req, username) {
    // Nginx supplies a trusted client address from Cloud Run's appended proxy hop.
    const ip = trustProxy
      ? req.headers["x-pokeweb-client-ip"] || req.socket.remoteAddress
      : req.socket.remoteAddress;
    for (const key of [`ip:${ip}`, `user:${username}`]) {
      const { rows } = await pool.query(
        `INSERT INTO auth_limits(key,count,expires_at) VALUES($1,1,now()+interval '15 minutes')
        ON CONFLICT(key) DO UPDATE SET count=CASE WHEN auth_limits.expires_at < now() THEN 1 ELSE auth_limits.count+1 END,
        expires_at=CASE WHEN auth_limits.expires_at < now() THEN now()+interval '15 minutes' ELSE auth_limits.expires_at END RETURNING count`,
        [hash(key)],
      );
      if (rows[0].count > authLimit)
        fail(429, "Too many sign-in attempts. Try again in 15 minutes.");
    }
  }
  async function login(req, res, body, register) {
    const { username, password } = credentials(body);
    await rateLimit(req, username);
    let user;
    if (register) {
      const encoded = await passwordHash(password);
      try {
        const result = await pool.query(
          "INSERT INTO accounts(id,username,password_hash) VALUES($1,$2,$3) RETURNING id,username",
          [randomUUID(), username, encoded],
        );
        user = result.rows[0];
      } catch (e) {
        if (e.code === "23505") fail(409, "That username is taken.");
        throw e;
      }
    } else {
      const { rows } = await pool.query(
        "SELECT id,username,password_hash FROM accounts WHERE username=$1",
        [username],
      );
      const valid = await passwordMatches(
        password,
        rows[0]?.password_hash || (await dummy),
      );
      if (!rows[0] || !valid) fail(401, "Incorrect username or password.");
      user = rows[0];
    }
    const token = randomBytes(32).toString("hex");
    const oldToken = tokenFrom(req);
    if (oldToken)
      await pool.query("DELETE FROM sessions WHERE token_hash=$1", [
        hash(oldToken),
      ]);
    await pool.query(
      `INSERT INTO sessions(token_hash,account_id,expires_at) VALUES($1,$2,now()+interval '30 days')`,
      [hash(token), user.id],
    );
    res.setHeader("Set-Cookie", cookie(token));
    return {
      user: { id: user.id, username: user.username },
      save: await getMetadata(user.id),
    };
  }
  async function getMetadata(id) {
    return metadata(
      (
        await pool.query(
          "SELECT game,name,revision,updated_at,sha256 FROM cloud_saves WHERE account_id=$1",
          [id],
        )
      ).rows[0],
    );
  }
  async function route(req, res) {
    const path = req.url?.split("?")[0];
    if (req.method === "GET" && path === "/api/health") {
      await pool.query("SELECT 1 FROM accounts LIMIT 0");
      return { ok: true };
    }
    if (
      !["GET", "HEAD"].includes(req.method) &&
      !origins.includes(req.headers.origin)
    )
      fail(403, "Request origin is not allowed.");
    if (
      req.method === "POST" &&
      ["/api/auth/register", "/api/auth/login"].includes(path)
    )
      return login(
        req,
        res,
        await jsonBody(req, 2048),
        path.endsWith("register"),
      );
    if (req.method === "POST" && path === "/api/auth/logout") {
      const token = tokenFrom(req);
      if (token)
        await pool.query("DELETE FROM sessions WHERE token_hash=$1", [
          hash(token),
        ]);
      res.setHeader("Set-Cookie", cookie("", 0));
      return { ok: true };
    }
    const user = await account(req);
    if (req.method === "GET" && path === "/api/account")
      return { user, save: await getMetadata(user.id) };
    if (req.method === "GET" && path === "/api/save") {
      const { rows } = await pool.query(
        "SELECT * FROM cloud_saves WHERE account_id=$1",
        [user.id],
      );
      const row = rows[0];
      if (!row) fail(404, "No cloud save yet.");
      if (hash(row.data) !== row.sha256)
        fail(500, "Cloud save integrity check failed.");
      return {
        accountId: user.id,
        ...metadata(row),
        data: row.data.toString("base64"),
      };
    }
    if (req.method === "PUT" && path === "/api/save") {
      const body = await jsonBody(req, 710000);
      if (body.accountId !== user.id)
        fail(403, "The signed-in account changed. Sign in again.");
      if (!["diamond", "pearl", "platinum"].includes(body.game))
        fail(400, "Unknown game.");
      if (
        typeof body.name !== "string" ||
        !body.name.trim() ||
        body.name.length > 32
      )
        fail(400, "Invalid save name.");
      if (
        !Number.isSafeInteger(body.revision) ||
        body.revision < 0 ||
        body.revision >= 2147483647
      )
        fail(400, "Invalid save revision.");
      if (
        typeof body.data !== "string" ||
        body.data.length !== 699052 ||
        !/^[A-Za-z0-9+/]+=$/.test(body.data)
      )
        fail(400, "Save must contain exactly 512 KiB.");
      const data = Buffer.from(body.data, "base64");
      if (
        data.length !== SIZE ||
        data.toString("base64") !== body.data ||
        data.every((x) => x === 0) ||
        data.every((x) => x === 255)
      )
        fail(400, "Invalid or empty save image.");
      const digest = hash(data);
      if (body.sha256 !== digest) fail(400, "Save checksum does not match.");
      const db = await pool.connect();
      try {
        await db.query("BEGIN");
        // Account lock also serializes the very first save when no save row exists.
        await db.query("SELECT id FROM accounts WHERE id=$1 FOR UPDATE", [
          user.id,
        ]);
        const current = (
          await db.query(
            "SELECT revision,sha256,game,name,updated_at FROM cloud_saves WHERE account_id=$1",
            [user.id],
          )
        ).rows[0];
        if ((current?.revision || 0) !== body.revision) {
          // A retry after a lost response is safe only if the exact content already won.
          if (
            current?.sha256 === digest &&
            current.game === body.game &&
            current.name === body.name.trim()
          ) {
            await db.query("COMMIT");
            return { save: metadata(current) };
          }
          fail(
            409,
            "Cloud save changed on another device. Load it or explicitly replace it from Save manager.",
          );
        }
        const { rows } = await db.query(
          `INSERT INTO cloud_saves(account_id,game,name,data,sha256,revision)
          VALUES($1,$2,$3,$4,$5,1) ON CONFLICT(account_id) DO UPDATE SET game=$2,name=$3,data=$4,sha256=$5,
          revision=cloud_saves.revision+1,updated_at=now() RETURNING game,name,revision,updated_at,sha256`,
          [user.id, body.game, body.name.trim(), data, digest],
        );
        await db.query("COMMIT");
        return { save: metadata(rows[0]) };
      } catch (e) {
        await db.query("ROLLBACK");
        throw e;
      } finally {
        db.release();
      }
    }
    fail(404, "Not found.");
  }
  return createServer(async (req, res) => {
    res.setHeader("Cache-Control", "no-store");
    res.setHeader("Content-Type", "application/json; charset=utf-8");
    res.setHeader("X-Content-Type-Options", "nosniff");
    try {
      const result = await route(req, res);
      res.end(JSON.stringify(result));
    } catch (e) {
      res.statusCode = e.status || 503;
      if (!e.status)
        console.error("Cloud save request failed:", e.code || e.name);
      if (res.statusCode === 429) res.setHeader("Retry-After", "900");
      res.end(
        JSON.stringify({
          error: e.status
            ? e.message
            : "Cloud saves are temporarily unavailable. Your local save is safe.",
        }),
      );
    }
  });
}
