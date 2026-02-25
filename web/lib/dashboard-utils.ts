import type {
  DashboardData,
  ServerStatus,
  BufferPoolStats,
  EmbeddingPipelineStats,
  SlowQuery,
} from "./dashboard-types";
import type { ConnectionParams } from "./connection-types";

export async function fetchDashboardData(
  slowQueryThresholdMs: number,
  conn?: ConnectionParams
): Promise<DashboardData> {
  const res = await fetch("/api/dashboard", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      slow_query_threshold_ms: slowQueryThresholdMs,
      connection: conn,
    }),
  });
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(body.error || `Dashboard fetch failed: ${res.status}`);
  }
  return res.json();
}

export function formatUptime(seconds: number): string {
  if (seconds < 0) return "0s";
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const secs = Math.floor(seconds % 60);

  const parts: string[] = [];
  if (days > 0) parts.push(`${days}d`);
  if (hours > 0) parts.push(`${hours}h`);
  if (minutes > 0) parts.push(`${minutes}m`);
  if (parts.length === 0 || secs > 0) parts.push(`${secs}s`);
  return parts.join(" ");
}

export function formatMemory(mb: number): string {
  if (mb < 1024) return `${mb.toFixed(1)} MB`;
  return `${(mb / 1024).toFixed(2)} GB`;
}

export function formatRate(rate: number): string {
  if (rate < 1) return `${(rate * 60).toFixed(1)}/min`;
  if (rate >= 1000) return `${(rate / 1000).toFixed(1)}k/s`;
  return `${rate.toFixed(1)}/s`;
}

export function formatMs(ms: number): string {
  if (ms < 1) return `${(ms * 1000).toFixed(0)}us`;
  if (ms < 1000) return `${ms.toFixed(1)}ms`;
  return `${(ms / 1000).toFixed(2)}s`;
}

export function computeHitRate(hits: number, misses: number): number {
  const total = hits + misses;
  if (total === 0) return 0;
  return (hits / total) * 100;
}

export function parseServerStatus(
  rows: unknown[][]
): ServerStatus {
  const map = new Map<string, string>();
  for (const row of rows) {
    if (row.length >= 2) {
      map.set(String(row[0]), String(row[1]));
    }
  }
  return {
    version: map.get("version") || "unknown",
    uptime_seconds: Number(map.get("uptime_seconds") || 0),
    active_connections: Number(map.get("active_connections") || 0),
    memory_usage_mb: Number(map.get("memory_usage_mb") || 0),
  };
}

export function parseBufferPoolStats(
  rows: unknown[][]
): BufferPoolStats {
  const map = new Map<string, string>();
  for (const row of rows) {
    if (row.length >= 2) {
      map.set(String(row[0]), String(row[1]));
    }
  }
  const hitCount = Number(map.get("hit_count") || 0);
  const missCount = Number(map.get("miss_count") || 0);
  return {
    total_pages: Number(map.get("total_pages") || 0),
    dirty_pages: Number(map.get("dirty_pages") || 0),
    hit_count: hitCount,
    miss_count: missCount,
    eviction_count: Number(map.get("eviction_count") || 0),
    hit_rate: computeHitRate(hitCount, missCount),
  };
}

export function parseEmbeddingPipelineStats(
  rows: unknown[][]
): EmbeddingPipelineStats {
  const map = new Map<string, string>();
  for (const row of rows) {
    if (row.length >= 2) {
      map.set(String(row[0]), String(row[1]));
    }
  }
  return {
    queue_depth: Number(map.get("queue_depth") || 0),
    processing_rate: Number(map.get("processing_rate") || 0),
    total_processed: Number(map.get("total_processed") || 0),
    total_failures: Number(map.get("total_failures") || 0),
    avg_latency_ms: Number(map.get("avg_latency_ms") || 0),
  };
}

export function parseSlowQueries(
  rows: unknown[][]
): SlowQuery[] {
  return rows.map((row) => ({
    query: String(row[0] || ""),
    execution_time_ms: Number(row[1] || 0),
    timestamp: String(row[2] || ""),
    database: String(row[3] || ""),
  }));
}
