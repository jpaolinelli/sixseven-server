import pg from "pg";

const pools = new Map<string, pg.Pool>();

export function getPool(database: string): pg.Pool {
  let pool = pools.get(database);
  if (!pool) {
    pool = new pg.Pool({
      host: process.env.GIODB_HOST || "localhost",
      port: parseInt(process.env.GIODB_PORT || "6767", 10),
      user: process.env.GIODB_USER || "giodb",
      database,
      max: 5,
      connectionTimeoutMillis: 5000,
      idleTimeoutMillis: 30000,
    });
    pools.set(database, pool);
  }
  return pool;
}

export async function query(
  sql: string,
  database?: string
): Promise<{ columns: string[]; rows: unknown[][] }> {
  const db = database || process.env.GIODB_DEFAULT_DATABASE || "giodb";
  const pool = getPool(db);
  const result = await pool.query(sql);
  const columns = result.fields.map((f) => f.name);
  const rows = result.rows.map((row) =>
    columns.map((col) => row[col] ?? null)
  );
  return { columns, rows };
}
