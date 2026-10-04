import pg from "pg";
import { spawn } from "node:child_process";
import { createApp, migrate } from "./app.mjs";
const pool = new pg.Pool({
  max: 5,
  connectionTimeoutMillis: 10000,
  idleTimeoutMillis: 30000,
  ...(process.env.DATABASE_URL
    ? { connectionString: process.env.DATABASE_URL }
    : {}),
});
pool.on("error", (e) =>
  console.error("Database connection failed:", e.code || e.name),
);
// Static play stays available even when the database is temporarily offline.
let migrationTimer;
async function ensureSchema() {
  try {
    await migrate(pool);
  } catch (e) {
    console.error("Database initialization pending:", e.code || e.name);
    migrationTimer = setTimeout(() => void ensureSchema(), 15000).unref();
  }
}
void ensureSchema();
const server = createApp({
  pool,
  origins: (process.env.APP_ORIGINS || "http://127.0.0.1:5175").split(","),
  secureCookies: process.env.COOKIE_SECURE !== "false",
  trustProxy: process.env.SERVE_STATIC === "true",
});
server.requestTimeout = 20000;
server.listen(
  Number(process.env.API_PORT || 8081),
  process.env.API_HOST || "127.0.0.1",
);
let nginx;
if (process.env.SERVE_STATIC === "true") {
  nginx = spawn("nginx", ["-g", "daemon off;"], { stdio: "inherit" });
  nginx.once("exit", (code) => {
    if (!stopping) process.exit(code || 1);
  });
}
let stopping = false;
const cleanup = setInterval(() => {
  void pool
    .query("DELETE FROM sessions WHERE expires_at < now()")
    .then(() => pool.query("DELETE FROM auth_limits WHERE expires_at < now()"))
    .catch(() => {});
}, 3600000).unref();
for (const signal of ["SIGINT", "SIGTERM"])
  process.on(signal, () => {
    if (stopping) return;
    stopping = true;
    clearInterval(cleanup);
    clearTimeout(migrationTimer);
    nginx?.kill("SIGTERM");
    server.close(() => void pool.end().then(() => process.exit(0)));
    setTimeout(() => process.exit(0), 8000).unref();
  });
