import { NextRequest, NextResponse } from "next/server";
import { query } from "@/lib/db";
import {
  parseServerStatus,
  parseBufferPoolStats,
  parseEmbeddingPipelineStats,
  parseSlowQueries,
} from "@/lib/dashboard-utils";
import type { DashboardData } from "@/lib/dashboard-types";

export async function POST(request: NextRequest) {
  try {
    const body = await request.json();
    const thresholdMs = Number(body.slow_query_threshold_ms) || 1000;

    const [serverResult, bufferResult, embeddingResult, slowResult] =
      await Promise.all([
        query("SHOW SERVER STATUS"),
        query("SHOW BUFFER POOL"),
        query("SHOW EMBEDDING PIPELINE"),
        query(`SHOW SLOW QUERIES ${thresholdMs}`),
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
