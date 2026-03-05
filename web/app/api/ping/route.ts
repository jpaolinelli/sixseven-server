import { NextRequest, NextResponse } from "next/server";
import { ping } from "@/lib/db";
import type { ConnectionParams } from "@/lib/connection-types";

export async function POST(request: NextRequest) {
  try {
    const body = await request.json();
    const conn = parseConnectionParams(body);
    const ok = await ping(conn);
    return NextResponse.json({ ok });
  } catch {
    return NextResponse.json({ ok: false });
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
