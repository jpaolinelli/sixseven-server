import { NextRequest, NextResponse } from "next/server";
import { query } from "@/lib/db";

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

    const result = await query(sql, database);
    return NextResponse.json(result);
  } catch (error) {
    const message =
      error instanceof Error ? error.message : "Unknown error";
    return NextResponse.json({ error: message }, { status: 500 });
  }
}
