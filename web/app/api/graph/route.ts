import { NextRequest, NextResponse } from "next/server";
import { query } from "@/lib/db";
import { quoteIdent } from "@/lib/schema-utils";
import type { ConnectionParams } from "@/lib/connection-types";

/**
 * POST /api/graph — Execute graph traversal or shortest-path queries.
 *
 * Body variants:
 *  { action: "traverse", database, table, id, direction?, edgeType?, connection? }
 *  { action: "shortest_path", database, sourceTable, sourceId, targetTable, targetId, connection? }
 *  { action: "node_details", database, table, id, connection? }
 */
export async function POST(request: NextRequest) {
  try {
    const body = await request.json();
    const { action, database } = body as { action: string; database: string };

    if (!action || !database) {
      return NextResponse.json(
        { error: "Missing 'action' or 'database'" },
        { status: 400 }
      );
    }

    const conn = parseConnectionParams(body);

    switch (action) {
      case "traverse":
        return await handleTraverse(body, database, conn);
      case "shortest_path":
        return await handleShortestPath(body, database, conn);
      case "node_details":
        return await handleNodeDetails(body, database, conn);
      default:
        return NextResponse.json(
          { error: `Unknown action: ${action}` },
          { status: 400 }
        );
    }
  } catch (error) {
    const message =
      error instanceof Error ? error.message : "Unknown error";
    return NextResponse.json({ error: message }, { status: 500 });
  }
}

async function handleTraverse(
  body: {
    table: string;
    id: string;
    direction?: "out" | "in" | "both";
    edgeType?: string;
  },
  database: string,
  conn?: ConnectionParams
) {
  const { table, id, direction = "both", edgeType } = body;

  if (!table || id === undefined) {
    return NextResponse.json(
      { error: "Missing 'table' or 'id'" },
      { status: 400 }
    );
  }

  const edgeClause = edgeType ? ` EDGE ${quoteIdent(edgeType)}` : "";
  const dirMap = { out: "OUT", in: "IN", both: "BOTH" } as const;
  const dir = dirMap[direction] || "BOTH";

  const sql = `TRAVERSE ${dir}${edgeClause} FROM ${quoteIdent(table)} WHERE id = ${quoteLiteral(id)} MAX_DEPTH 1`;

  const result = await query(sql, database, conn);
  return NextResponse.json(result);
}

async function handleShortestPath(
  body: {
    sourceTable: string;
    sourceId: string;
    targetTable: string;
    targetId: string;
  },
  database: string,
  conn?: ConnectionParams
) {
  const { sourceTable, sourceId, targetTable, targetId } = body;

  if (!sourceTable || !sourceId || !targetTable || !targetId) {
    return NextResponse.json(
      { error: "Missing source/target table/id" },
      { status: 400 }
    );
  }

  const sql = `SHORTEST PATH FROM ${quoteIdent(sourceTable)} WHERE id = ${quoteLiteral(sourceId)} TO ${quoteIdent(targetTable)} WHERE id = ${quoteLiteral(targetId)}`;

  const result = await query(sql, database, conn);
  return NextResponse.json(result);
}

async function handleNodeDetails(
  body: { table: string; id: string },
  database: string,
  conn?: ConnectionParams
) {
  const { table, id } = body;

  if (!table || id === undefined) {
    return NextResponse.json(
      { error: "Missing 'table' or 'id'" },
      { status: 400 }
    );
  }

  const sql = `SELECT * FROM ${quoteIdent(table)} WHERE id = ${quoteLiteral(id)}`;

  const result = await query(sql, database, conn);
  return NextResponse.json(result);
}

/** Quote a string literal for safe SQL embedding. */
function quoteLiteral(value: string): string {
  if (/^\d+$/.test(value)) return value;
  return `'${value.replace(/'/g, "''")}'`;
}

function parseConnectionParams(
  body: Record<string, unknown>
): ConnectionParams | undefined {
  const c = body.connection as Record<string, unknown> | undefined;
  if (!c || !c.host) return undefined;
  return {
    host: String(c.host),
    port: Number(c.port) || 6767,
    user: String(c.user || "sixseven"),
  };
}
