import pg from "pg";
import type { ConnectionParams } from "./connection-types";

const pools = new Map<string, pg.Pool>();

/** Build a pool key that includes connection params + database. */
function poolKey(conn: ConnectionParams | undefined, database: string): string {
  if (!conn) {
    const host = process.env.SIXSEVEN_HOST || "localhost";
    const port = process.env.SIXSEVEN_PORT || "6767";
    const user = process.env.SIXSEVEN_USER || "sixseven";
    return `${host}:${port}:${user}:${database}`;
  }
  return `${conn.host}:${conn.port}:${conn.user}:${database}`;
}

export function getPool(
  database: string,
  conn?: ConnectionParams
): pg.Pool {
  const key = poolKey(conn, database);
  let pool = pools.get(key);
  if (!pool) {
    pool = new pg.Pool({
      host: conn?.host || process.env.SIXSEVEN_HOST || "localhost",
      port: conn?.port || parseInt(process.env.SIXSEVEN_PORT || "6767", 10),
      user: conn?.user || process.env.SIXSEVEN_USER || "sixseven",
      database,
      max: 5,
      connectionTimeoutMillis: 5000,
      idleTimeoutMillis: 30000,
    });
    pools.set(key, pool);
  }
  return pool;
}

export async function query(
  sql: string,
  database?: string,
  conn?: ConnectionParams
): Promise<{ columns: string[]; rows: unknown[][] }> {
  const db = database || process.env.SIXSEVEN_DEFAULT_DATABASE || "sixseven";
  const pool = getPool(db, conn);
  const result = await pool.query(sql);
  const columns = result.fields.map((f) => f.name);
  const rows = result.rows.map((row) =>
    columns.map((col) => row[col] ?? null)
  );
  return { columns, rows };
}

/** Test connectivity to a server. Returns true if reachable. */
export async function ping(conn?: ConnectionParams): Promise<boolean> {
  const db = process.env.SIXSEVEN_DEFAULT_DATABASE || "sixseven";
  const pool = getPool(db, conn);
  try {
    await pool.query("SHOW DATABASES");
    return true;
  } catch {
    return false;
  }
}
