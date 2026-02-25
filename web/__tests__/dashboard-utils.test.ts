import { describe, it, expect } from "vitest";
import {
  formatUptime,
  formatMemory,
  formatRate,
  formatMs,
  computeHitRate,
  parseServerStatus,
  parseBufferPoolStats,
  parseEmbeddingPipelineStats,
  parseSlowQueries,
} from "@/lib/dashboard-utils";

describe("formatUptime", () => {
  it("formats seconds only", () => {
    expect(formatUptime(45)).toBe("45s");
  });

  it("formats minutes and seconds", () => {
    expect(formatUptime(125)).toBe("2m 5s");
  });

  it("formats hours, minutes, seconds", () => {
    expect(formatUptime(3661)).toBe("1h 1m 1s");
  });

  it("formats days, hours, minutes", () => {
    expect(formatUptime(90061)).toBe("1d 1h 1m 1s");
  });

  it("omits zero components in the middle", () => {
    expect(formatUptime(86400)).toBe("1d");
  });

  it("formats zero seconds", () => {
    expect(formatUptime(0)).toBe("0s");
  });

  it("handles negative values", () => {
    expect(formatUptime(-5)).toBe("0s");
  });

  it("formats exact hours without trailing seconds", () => {
    expect(formatUptime(3600)).toBe("1h");
  });

  it("formats exact minutes without trailing seconds", () => {
    expect(formatUptime(120)).toBe("2m");
  });
});

describe("formatMemory", () => {
  it("formats small values in MB", () => {
    expect(formatMemory(256.5)).toBe("256.5 MB");
  });

  it("formats values >= 1024 in GB", () => {
    expect(formatMemory(2048)).toBe("2.00 GB");
  });

  it("formats exactly 1024 as 1.00 GB", () => {
    expect(formatMemory(1024)).toBe("1.00 GB");
  });

  it("formats small fractional MB", () => {
    expect(formatMemory(0.5)).toBe("0.5 MB");
  });
});

describe("formatRate", () => {
  it("formats per-second rates", () => {
    expect(formatRate(42.3)).toBe("42.3/s");
  });

  it("formats sub-1 rates as per-minute", () => {
    expect(formatRate(0.5)).toBe("30.0/min");
  });

  it("formats large rates in thousands", () => {
    expect(formatRate(1500)).toBe("1.5k/s");
  });

  it("formats exactly 1000 as k/s", () => {
    expect(formatRate(1000)).toBe("1.0k/s");
  });
});

describe("formatMs", () => {
  it("formats sub-millisecond as microseconds", () => {
    expect(formatMs(0.5)).toBe("500us");
  });

  it("formats milliseconds", () => {
    expect(formatMs(42.3)).toBe("42.3ms");
  });

  it("formats >= 1000ms as seconds", () => {
    expect(formatMs(1500)).toBe("1.50s");
  });

  it("formats exactly 1ms", () => {
    expect(formatMs(1)).toBe("1.0ms");
  });
});

describe("computeHitRate", () => {
  it("computes correct percentage", () => {
    expect(computeHitRate(90, 10)).toBe(90);
  });

  it("returns 100 for all hits", () => {
    expect(computeHitRate(100, 0)).toBe(100);
  });

  it("returns 0 for all misses", () => {
    expect(computeHitRate(0, 50)).toBe(0);
  });

  it("returns 0 for zero total", () => {
    expect(computeHitRate(0, 0)).toBe(0);
  });

  it("handles large numbers", () => {
    expect(computeHitRate(999999, 1)).toBeCloseTo(99.9999, 3);
  });
});

describe("parseServerStatus", () => {
  it("parses key-value rows into ServerStatus", () => {
    const rows = [
      ["version", "0.1.0"],
      ["uptime_seconds", "3600"],
      ["active_connections", "5"],
      ["memory_usage_mb", "256"],
    ];
    const status = parseServerStatus(rows);
    expect(status.version).toBe("0.1.0");
    expect(status.uptime_seconds).toBe(3600);
    expect(status.active_connections).toBe(5);
    expect(status.memory_usage_mb).toBe(256);
  });

  it("defaults missing fields", () => {
    const status = parseServerStatus([]);
    expect(status.version).toBe("unknown");
    expect(status.uptime_seconds).toBe(0);
    expect(status.active_connections).toBe(0);
    expect(status.memory_usage_mb).toBe(0);
  });

  it("handles partial data", () => {
    const rows = [["version", "1.0"]];
    const status = parseServerStatus(rows);
    expect(status.version).toBe("1.0");
    expect(status.uptime_seconds).toBe(0);
  });

  it("ignores rows with fewer than 2 columns", () => {
    const rows = [["version"], ["uptime_seconds", "100"]];
    const status = parseServerStatus(rows);
    expect(status.version).toBe("unknown");
    expect(status.uptime_seconds).toBe(100);
  });
});

describe("parseBufferPoolStats", () => {
  it("parses stats and computes hit rate", () => {
    const rows = [
      ["total_pages", "1000"],
      ["dirty_pages", "50"],
      ["hit_count", "900"],
      ["miss_count", "100"],
      ["eviction_count", "10"],
    ];
    const stats = parseBufferPoolStats(rows);
    expect(stats.total_pages).toBe(1000);
    expect(stats.dirty_pages).toBe(50);
    expect(stats.hit_count).toBe(900);
    expect(stats.miss_count).toBe(100);
    expect(stats.eviction_count).toBe(10);
    expect(stats.hit_rate).toBe(90);
  });

  it("handles zero hits and misses", () => {
    const rows = [
      ["hit_count", "0"],
      ["miss_count", "0"],
    ];
    const stats = parseBufferPoolStats(rows);
    expect(stats.hit_rate).toBe(0);
  });
});

describe("parseEmbeddingPipelineStats", () => {
  it("parses pipeline stats", () => {
    const rows = [
      ["queue_depth", "25"],
      ["processing_rate", "10.5"],
      ["total_processed", "5000"],
      ["total_failures", "3"],
      ["avg_latency_ms", "42.1"],
    ];
    const stats = parseEmbeddingPipelineStats(rows);
    expect(stats.queue_depth).toBe(25);
    expect(stats.processing_rate).toBe(10.5);
    expect(stats.total_processed).toBe(5000);
    expect(stats.total_failures).toBe(3);
    expect(stats.avg_latency_ms).toBe(42.1);
  });

  it("defaults missing fields to zero", () => {
    const stats = parseEmbeddingPipelineStats([]);
    expect(stats.queue_depth).toBe(0);
    expect(stats.processing_rate).toBe(0);
  });
});

describe("parseSlowQueries", () => {
  it("parses query rows", () => {
    const rows = [
      ["SELECT * FROM users", 1500, "2026-01-15 10:30:00", "mydb"],
      ["UPDATE orders SET status = 'shipped'", 3000, "2026-01-15 10:31:00", "mydb"],
    ];
    const queries = parseSlowQueries(rows);
    expect(queries).toHaveLength(2);
    expect(queries[0].query).toBe("SELECT * FROM users");
    expect(queries[0].execution_time_ms).toBe(1500);
    expect(queries[0].timestamp).toBe("2026-01-15 10:30:00");
    expect(queries[0].database).toBe("mydb");
  });

  it("handles empty rows", () => {
    const queries = parseSlowQueries([]);
    expect(queries).toHaveLength(0);
  });

  it("handles rows with missing columns", () => {
    const rows = [["SELECT 1"]];
    const queries = parseSlowQueries(rows);
    expect(queries).toHaveLength(1);
    expect(queries[0].query).toBe("SELECT 1");
    expect(queries[0].execution_time_ms).toBe(0);
    expect(queries[0].timestamp).toBe("");
    expect(queries[0].database).toBe("");
  });
});
