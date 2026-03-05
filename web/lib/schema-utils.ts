import type {
  DatabaseInfo,
  DatabaseSchema,
  TableInfo,
  ColumnInfo,
  IndexInfo,
  EmbeddingInfo,
} from "./types";
import type { ConnectionParams } from "./connection-types";

const API_BASE = "/api";

/** Sanitize a SQL identifier to prevent injection.
 *  SixSevenDB does not support double-quoted identifiers, so we validate
 *  that the name contains only safe characters instead of quoting. */
export function quoteIdent(name: string): string {
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
    throw new Error(`Invalid identifier: ${name}`);
  }
  return name;
}

/** Build query string with connection params for GET requests. */
function connQueryString(conn?: ConnectionParams): string {
  if (!conn) return "";
  return `&connHost=${encodeURIComponent(conn.host)}&connPort=${conn.port}&connUser=${encodeURIComponent(conn.user)}`;
}

async function fetchJson<T>(url: string): Promise<T> {
  const res = await fetch(url);
  if (!res.ok) {
    const body = await res.text();
    throw new Error(`API error ${res.status}: ${body}`);
  }
  return res.json();
}

export async function fetchDatabases(
  conn?: ConnectionParams
): Promise<DatabaseInfo[]> {
  const cq = conn
    ? `?connHost=${encodeURIComponent(conn.host)}&connPort=${conn.port}&connUser=${encodeURIComponent(conn.user)}`
    : "";
  return fetchJson<DatabaseInfo[]>(`${API_BASE}/schema${cq}`);
}

export async function fetchDatabaseSchema(
  database: string,
  conn?: ConnectionParams
): Promise<DatabaseSchema> {
  return fetchJson<DatabaseSchema>(
    `${API_BASE}/schema?database=${encodeURIComponent(database)}${connQueryString(conn)}`
  );
}

export async function fetchSampleData(
  database: string,
  table: string,
  conn?: ConnectionParams
): Promise<{ columns: string[]; rows: unknown[][] }> {
  const res = await fetch(`${API_BASE}/query`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      sql: `SELECT * FROM ${quoteIdent(table)} LIMIT 10`,
      database,
      connection: conn,
    }),
  });
  if (!res.ok) {
    const body = await res.text();
    throw new Error(`Query error ${res.status}: ${body}`);
  }
  return res.json();
}

export function buildTableInfo(
  tableName: string,
  columns: ColumnInfo[],
  allIndexes: IndexInfo[],
  allEmbeddings: EmbeddingInfo[]
): TableInfo {
  return {
    name: tableName,
    columns,
    indexes: allIndexes.filter((idx) => idx.tableName === tableName),
    embeddings: allEmbeddings.filter((emb) => emb.tableName === tableName),
  };
}
