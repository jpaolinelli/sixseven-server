import type {
  DatabaseInfo,
  DatabaseSchema,
  TableInfo,
  ColumnInfo,
  IndexInfo,
  EdgeTypeInfo,
  EmbeddingInfo,
} from "./types";

const API_BASE = "/api";

async function fetchJson<T>(url: string): Promise<T> {
  const res = await fetch(url);
  if (!res.ok) {
    const body = await res.text();
    throw new Error(`API error ${res.status}: ${body}`);
  }
  return res.json();
}

export async function fetchDatabases(): Promise<DatabaseInfo[]> {
  return fetchJson<DatabaseInfo[]>(`${API_BASE}/schema`);
}

export async function fetchDatabaseSchema(
  database: string
): Promise<DatabaseSchema> {
  return fetchJson<DatabaseSchema>(
    `${API_BASE}/schema?database=${encodeURIComponent(database)}`
  );
}

export async function fetchSampleData(
  database: string,
  table: string
): Promise<{ columns: string[]; rows: unknown[][] }> {
  const res = await fetch(`${API_BASE}/query`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      sql: `SELECT * FROM ${table} LIMIT 10`,
      database,
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
