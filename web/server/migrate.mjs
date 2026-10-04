import pg from "pg";
import { migrate } from "./app.mjs";

const connectionString = process.env.DATABASE_URL_UNPOOLED || process.env.DATABASE_URL;
if (!connectionString) throw new Error("A database connection is required");
const databaseUrl = new URL(connectionString);
if (databaseUrl.hostname.includes("-pooler."))
  throw new Error("Migrations require a direct database connection");
if (databaseUrl.hostname.endsWith(".neon.tech"))
  databaseUrl.searchParams.set("sslmode", "verify-full");
const pool = new pg.Pool({ connectionString: databaseUrl.href, max: 1, connectionTimeoutMillis: 15000 });
try {
  await migrate(pool);
  console.log("Save schema migration completed.");
} catch (error) {
  console.error("Save schema migration failed:", error.code || error.name);
  process.exitCode = 1;
} finally {
  await pool.end();
}
