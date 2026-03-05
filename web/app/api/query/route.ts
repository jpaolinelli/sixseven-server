import { NextRequest, NextResponse } from "next/server";
import { query } from "@/lib/db";
import type { ConnectionParams } from "@/lib/connection-types";

export async function POST(request: NextRequest) {
  try {
    const body = await request.json();
    const { sql, database } = body as { sql: string; database?: string };

    if (!sql || typeof sql !== "string") {
      return NextResponse.json(
        { error: "Missing or invalid 'sql' field" },
        { status: 400 }
      );
    }

    const conn = parseConnectionParams(body);
    const result = await query(sql, database, conn);
    return NextResponse.json(result);
  } catch (error) {
    const message =
      error instanceof Error ? error.message : "Unknown error";
    return NextResponse.json({ error: message }, { status: 500 });
  }
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
