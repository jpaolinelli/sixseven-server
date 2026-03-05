import { NextRequest, NextResponse } from "next/server";
import { query } from "@/lib/db";
import {
  parseServerStatus,
  parseBufferPoolStats,
  parseEmbeddingPipelineStats,
  parseSlowQueries,
} from "@/lib/dashboard-utils";
import type { DashboardData } from "@/lib/dashboard-types";
import type { ConnectionParams } from "@/lib/connection-types";

export async function POST(request: NextRequest) {
  try {
    const body = await request.json();
    const thresholdMs = Number(body.slow_query_threshold_ms) || 1000;
    const conn = parseConnectionParams(body);

    const [serverResult, bufferResult, embeddingResult, slowResult] =
      await Promise.all([
        query("SHOW SERVER STATUS", undefined, conn),
        query("SHOW BUFFER POOL", undefined, conn),
        query("SHOW EMBEDDING PIPELINE", undefined, conn),
        query(`SHOW SLOW QUERIES ${thresholdMs}`, undefined, conn),
      ]);

    const data: DashboardData = {
      server: parseServerStatus(serverResult.rows),
      buffer_pool: parseBufferPoolStats(bufferResult.rows),
      embedding_pipeline: parseEmbeddingPipelineStats(embeddingResult.rows),
      slow_queries: parseSlowQueries(slowResult.rows),
    };

    return NextResponse.json(data);
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
