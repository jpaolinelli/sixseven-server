import { describe, it, expect } from "vitest";
import type {
  ServerStatus,
  BufferPoolStats,
  EmbeddingPipelineStats,
  SlowQuery,
  DashboardData,
  HitRateDataPoint,
} from "@/lib/dashboard-types";

describe("DashboardData types", () => {
  it("ServerStatus has required fields", () => {
    const status: ServerStatus = {
      version: "0.1.0",
      uptime_seconds: 3600,
      active_connections: 5,
      memory_usage_mb: 256,
    };
    expect(status.version).toBe("0.1.0");
    expect(status.uptime_seconds).toBe(3600);
    expect(status.active_connections).toBe(5);
    expect(status.memory_usage_mb).toBe(256);
  });

  it("BufferPoolStats has required fields including computed hit_rate", () => {
    const stats: BufferPoolStats = {
      total_pages: 1000,
      dirty_pages: 50,
      hit_count: 900,
      miss_count: 100,
      eviction_count: 10,
      hit_rate: 90,
    };
    expect(stats.hit_rate).toBe(90);
    expect(stats.total_pages).toBe(1000);
  });

  it("EmbeddingPipelineStats has required fields", () => {
    const stats: EmbeddingPipelineStats = {
      queue_depth: 25,
      processing_rate: 10.5,
      total_processed: 5000,
      total_failures: 3,
      avg_latency_ms: 42.1,
    };
    expect(stats.queue_depth).toBe(25);
    expect(stats.avg_latency_ms).toBe(42.1);
  });

  it("SlowQuery has required fields", () => {
    const query: SlowQuery = {
      query: "SELECT * FROM users",
      execution_time_ms: 1500,
      timestamp: "2026-01-15 10:30:00",
      database: "mydb",
    };
    expect(query.execution_time_ms).toBe(1500);
  });

  it("DashboardData combines all sections", () => {
    const data: DashboardData = {
      server: {
        version: "0.1.0",
        uptime_seconds: 3600,
        active_connections: 5,
        memory_usage_mb: 256,
      },
      buffer_pool: {
        total_pages: 1000,
        dirty_pages: 50,
        hit_count: 900,
        miss_count: 100,
        eviction_count: 10,
        hit_rate: 90,
      },
      embedding_pipeline: {
        queue_depth: 0,
        processing_rate: 0,
        total_processed: 0,
        total_failures: 0,
        avg_latency_ms: 0,
      },
      slow_queries: [],
    };
    expect(data.server.version).toBe("0.1.0");
    expect(data.buffer_pool.hit_rate).toBe(90);
    expect(data.slow_queries).toHaveLength(0);
  });

  it("HitRateDataPoint has time and hit_rate", () => {
    const point: HitRateDataPoint = {
      time: "10:30:00",
      hit_rate: 95.5,
    };
    expect(point.time).toBe("10:30:00");
    expect(point.hit_rate).toBe(95.5);
  });
});
