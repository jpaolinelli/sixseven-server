import { NextRequest, NextResponse } from "next/server";
import { query } from "@/lib/db";
import type {
  DatabaseInfo,
  DatabaseSchema,
  ColumnInfo,
  IndexInfo,
  EdgeTypeInfo,
  EmbeddingInfo,
} from "@/lib/types";
import type { ConnectionParams } from "@/lib/connection-types";
import { buildTableInfo, quoteIdent } from "@/lib/schema-utils";

const SYSTEM_DATABASES = new Set(["sixseven_system"]);

export async function GET(request: NextRequest) {
  const database = request.nextUrl.searchParams.get("database");
  const connHost = request.nextUrl.searchParams.get("connHost");
  const connPort = request.nextUrl.searchParams.get("connPort");
  const connUser = request.nextUrl.searchParams.get("connUser");

  const conn: ConnectionParams | undefined = connHost
    ? {
        host: connHost,
        port: Number(connPort) || 6767,
        user: connUser || "sixseven",
      }
    : undefined;

  try {
    if (!database) {
      // Return list of all databases.
      const result = await query("SHOW DATABASES", undefined, conn);
      const databases: DatabaseInfo[] = result.rows.map((row) => ({
        name: String(row[0]),
        isSystem: SYSTEM_DATABASES.has(String(row[0])),
      }));
      return NextResponse.json(databases);
    }

    // Return full schema for a specific database.
    const [tablesResult, indexesResult, edgeTypesResult, embeddingsResult] =
      await Promise.all([
        query("SHOW TABLES", database, conn),
        query("SHOW INDEXES", database, conn),
        query("SHOW EDGE TYPES", database, conn),
        query("SHOW EMBEDDINGS", database, conn),
      ]);

    const indexes: IndexInfo[] = indexesResult.rows.map((row) => ({
      name: String(row[0]),
      tableName: String(row[1]),
      columns: String(row[2]),
      type: String(row[3]),
      unique: Boolean(row[4]),
    }));

    const edgeTypes: EdgeTypeInfo[] = edgeTypesResult.rows.map((row) => ({
      name: String(row[0]),
      sourceTable: String(row[1]),
      targetTable: String(row[2]),
    }));

    const embeddings: EmbeddingInfo[] = embeddingsResult.rows.map((row) => ({
      tableName: String(row[0]),
      columnName: String(row[1]),
      dimension: Number(row[2]),
      sourceExpr: String(row[3]),
      provider: String(row[4]),
    }));

    // Fetch columns for each table.
    const tableNames = tablesResult.rows.map((row) => String(row[0]));
    const tables = await Promise.all(
      tableNames.map(async (name) => {
        const colResult = await query(
          `SHOW COLUMNS FROM ${quoteIdent(name)}`,
          database,
          conn
        );
        const columns: ColumnInfo[] = colResult.rows.map((row) => ({
          name: String(row[0]),
          type: String(row[1]),
          nullable: Boolean(row[2]),
        }));
        return buildTableInfo(name, columns, indexes, embeddings);
      })
    );

    const schema: DatabaseSchema = {
      database: {
        name: database,
        isSystem: SYSTEM_DATABASES.has(database),
      },
      tables,
      edgeTypes,
    };

    return NextResponse.json(schema);
  } catch (error) {
    const message =
      error instanceof Error ? error.message : "Unknown error";
    return NextResponse.json({ error: message }, { status: 500 });
  }
}
