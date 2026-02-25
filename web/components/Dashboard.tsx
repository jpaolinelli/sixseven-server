"use client";

import { useState, useEffect, useCallback, useRef } from "react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
  BarChart,
  Bar,
} from "recharts";
import type {
  DashboardData,
  HitRateDataPoint,
  SlowQuery,
} from "@/lib/dashboard-types";
import {
  fetchDashboardData,
  formatUptime,
  formatMemory,
  formatRate,
  formatMs,
} from "@/lib/dashboard-utils";
import { useConnection } from "@/lib/ConnectionContext";

const REFRESH_INTERVALS = [
  { label: "1s", value: 1000 },
  { label: "5s", value: 5000 },
  { label: "10s", value: 10000 },
  { label: "30s", value: 30000 },
  { label: "Off", value: 0 },
];

const MAX_HISTORY_POINTS = 60;

export function Dashboard() {
  const { connectionParams } = useConnection();
  const [data, setData] = useState<DashboardData | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [refreshInterval, setRefreshInterval] = useState(5000);
  const [slowQueryThreshold, setSlowQueryThreshold] = useState(1000);
  const [hitRateHistory, setHitRateHistory] = useState<HitRateDataPoint[]>([]);
  const [paused, setPaused] = useState(false);
  const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const loadData = useCallback(async () => {
    try {
      const result = await fetchDashboardData(slowQueryThreshold, connectionParams);
      setData(result);
      setError(null);
      setHitRateHistory((prev) => {
        const now = new Date().toLocaleTimeString();
        const next = [...prev, { time: now, hit_rate: result.buffer_pool.hit_rate }];
        if (next.length > MAX_HISTORY_POINTS) {
          return next.slice(next.length - MAX_HISTORY_POINTS);
        }
        return next;
      });
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to load dashboard");
    } finally {
      setLoading(false);
    }
  }, [slowQueryThreshold, connectionParams]);

  // Initial load
  useEffect(() => {
    loadData();
  }, [loadData]);

  // Auto-refresh
  useEffect(() => {
    if (intervalRef.current) {
      clearInterval(intervalRef.current);
      intervalRef.current = null;
    }
    if (refreshInterval > 0 && !paused) {
      intervalRef.current = setInterval(loadData, refreshInterval);
    }
    return () => {
      if (intervalRef.current) {
        clearInterval(intervalRef.current);
      }
    };
  }, [refreshInterval, paused, loadData]);

  if (loading && !data) {
    return (
      <div className="flex items-center justify-center h-full text-gray-500">
        Loading dashboard...
      </div>
    );
  }

  if (error && !data) {
    return (
      <div className="flex flex-col items-center justify-center h-full gap-3">
        <p className="text-red-400 text-sm">{error}</p>
        <button
          onClick={loadData}
          className="px-3 py-1.5 text-xs bg-gray-800 hover:bg-gray-700 rounded text-gray-300"
        >
          Retry
        </button>
      </div>
    );
  }

  return (
    <div className="flex flex-col h-full overflow-hidden">
      {/* Toolbar */}
      <div className="flex items-center gap-4 px-4 py-2 border-b border-gray-800 shrink-0">
        <span className="text-xs font-medium text-gray-400">Dashboard</span>

        <div className="flex items-center gap-1.5 ml-auto">
          <label className="text-xs text-gray-500">Slow query threshold:</label>
          <input
            type="number"
            value={slowQueryThreshold}
            onChange={(e) => setSlowQueryThreshold(Number(e.target.value) || 1000)}
            className="w-20 bg-gray-900 border border-gray-700 rounded px-2 py-0.5 text-xs text-gray-300"
            min={1}
          />
          <span className="text-xs text-gray-600">ms</span>
        </div>

        <div className="flex items-center gap-1.5">
          <label className="text-xs text-gray-500">Refresh:</label>
          <select
            value={refreshInterval}
            onChange={(e) => setRefreshInterval(Number(e.target.value))}
            className="bg-gray-900 border border-gray-700 rounded px-2 py-0.5 text-xs text-gray-300"
          >
            {REFRESH_INTERVALS.map((opt) => (
              <option key={opt.value} value={opt.value}>
                {opt.label}
              </option>
            ))}
          </select>
        </div>

        <button
          onClick={() => setPaused((p) => !p)}
          className={`px-2 py-0.5 text-xs rounded ${
            paused
              ? "bg-yellow-900/50 text-yellow-400 border border-yellow-800"
              : "bg-gray-800 text-gray-400 hover:bg-gray-700"
          }`}
        >
          {paused ? "Resume" : "Pause"}
        </button>

        <button
          onClick={loadData}
          className="px-2 py-0.5 text-xs bg-gray-800 hover:bg-gray-700 rounded text-gray-400"
          title="Refresh now"
        >
          Refresh
        </button>

        {error && (
          <span className="text-xs text-red-400" title={error}>
            Error
          </span>
        )}
      </div>

      {/* Dashboard content */}
      <div className="flex-1 overflow-auto p-4 space-y-4">
        {data && (
          <>
            {/* Top row: Server Status + Buffer Pool */}
            <div className="grid grid-cols-2 gap-4">
              <ServerStatusCard server={data.server} />
              <BufferPoolCard bufferPool={data.buffer_pool} />
            </div>

            {/* Middle row: Embedding Pipeline + Hit Rate Chart */}
            <div className="grid grid-cols-2 gap-4">
              <EmbeddingPipelineCard pipeline={data.embedding_pipeline} />
              <HitRateChart history={hitRateHistory} />
            </div>

            {/* Slow query distribution chart */}
            {data.slow_queries.length > 0 && (
              <SlowQueryDistribution queries={data.slow_queries} />
            )}

            {/* Bottom: Slow Queries Table */}
            <SlowQueryTable queries={data.slow_queries} />
          </>
        )}
      </div>
    </div>
  );
}

/* ── Stat Card Wrapper ──────────────────────────────────────────── */

function StatCard({
  title,
  children,
}: {
  title: string;
  children: React.ReactNode;
}) {
  return (
    <div className="bg-gray-900 border border-gray-800 rounded-lg p-4">
      <h3 className="text-xs font-medium text-gray-400 mb-3 uppercase tracking-wide">
        {title}
      </h3>
      {children}
    </div>
  );
}

function StatValue({
  label,
  value,
  accent,
}: {
  label: string;
  value: string;
  accent?: "green" | "yellow" | "red" | "blue";
}) {
  const colorMap = {
    green: "text-green-400",
    yellow: "text-yellow-400",
    red: "text-red-400",
    blue: "text-blue-400",
  };
  const color = accent ? colorMap[accent] : "text-gray-200";
  return (
    <div>
      <div className="text-xs text-gray-500">{label}</div>
      <div className={`text-sm font-mono ${color}`}>{value}</div>
    </div>
  );
}

/* ── Server Status ──────────────────────────────────────────────── */

function ServerStatusCard({
  server,
}: {
  server: DashboardData["server"];
}) {
  return (
    <StatCard title="Server Status">
      <div className="grid grid-cols-2 gap-3">
        <StatValue label="Version" value={server.version} accent="blue" />
        <StatValue
          label="Uptime"
          value={formatUptime(server.uptime_seconds)}
          accent="green"
        />
        <StatValue
          label="Connections"
          value={String(server.active_connections)}
        />
        <StatValue
          label="Memory"
          value={formatMemory(server.memory_usage_mb)}
        />
      </div>
    </StatCard>
  );
}

/* ── Buffer Pool ────────────────────────────────────────────────── */

function BufferPoolCard({
  bufferPool,
}: {
  bufferPool: DashboardData["buffer_pool"];
}) {
  const hitRateColor =
    bufferPool.hit_rate >= 95
      ? "green"
      : bufferPool.hit_rate >= 80
        ? "yellow"
        : "red";
  return (
    <StatCard title="Buffer Pool">
      <div className="grid grid-cols-2 gap-3">
        <StatValue
          label="Hit Rate"
          value={`${bufferPool.hit_rate.toFixed(1)}%`}
          accent={hitRateColor as "green" | "yellow" | "red"}
        />
        <StatValue
          label="Total Pages"
          value={bufferPool.total_pages.toLocaleString()}
        />
        <StatValue
          label="Dirty Pages"
          value={bufferPool.dirty_pages.toLocaleString()}
          accent={bufferPool.dirty_pages > 0 ? "yellow" : undefined}
        />
        <StatValue
          label="Evictions"
          value={bufferPool.eviction_count.toLocaleString()}
        />
      </div>
    </StatCard>
  );
}

/* ── Embedding Pipeline ─────────────────────────────────────────── */

function EmbeddingPipelineCard({
  pipeline,
}: {
  pipeline: DashboardData["embedding_pipeline"];
}) {
  return (
    <StatCard title="Embedding Pipeline">
      <div className="grid grid-cols-2 gap-3">
        <StatValue
          label="Queue Depth"
          value={String(pipeline.queue_depth)}
          accent={pipeline.queue_depth > 100 ? "red" : pipeline.queue_depth > 10 ? "yellow" : "green"}
        />
        <StatValue
          label="Processing Rate"
          value={formatRate(pipeline.processing_rate)}
        />
        <StatValue
          label="Avg Latency"
          value={formatMs(pipeline.avg_latency_ms)}
        />
        <StatValue
          label="Failures"
          value={pipeline.total_failures.toLocaleString()}
          accent={pipeline.total_failures > 0 ? "red" : undefined}
        />
      </div>
    </StatCard>
  );
}

/* ── Hit Rate Line Chart ────────────────────────────────────────── */

function HitRateChart({ history }: { history: HitRateDataPoint[] }) {
  return (
    <StatCard title="Buffer Pool Hit Rate Over Time">
      {history.length < 2 ? (
        <div className="flex items-center justify-center h-32 text-xs text-gray-600">
          Collecting data...
        </div>
      ) : (
        <ResponsiveContainer width="100%" height={150}>
          <LineChart data={history}>
            <CartesianGrid strokeDasharray="3 3" stroke="#374151" />
            <XAxis
              dataKey="time"
              tick={{ fontSize: 10, fill: "#6b7280" }}
              interval="preserveStartEnd"
            />
            <YAxis
              domain={[0, 100]}
              tick={{ fontSize: 10, fill: "#6b7280" }}
              tickFormatter={(v: number) => `${v}%`}
            />
            <Tooltip
              contentStyle={{
                backgroundColor: "#1f2937",
                border: "1px solid #374151",
                borderRadius: "6px",
                fontSize: "12px",
              }}
              labelStyle={{ color: "#9ca3af" }}
              formatter={(value?: number) => [`${(value ?? 0).toFixed(1)}%`, "Hit Rate"]}
            />
            <Line
              type="monotone"
              dataKey="hit_rate"
              stroke="#60a5fa"
              strokeWidth={2}
              dot={false}
              activeDot={{ r: 3 }}
            />
          </LineChart>
        </ResponsiveContainer>
      )}
    </StatCard>
  );
}

/* ── Slow Query Distribution Bar Chart ──────────────────────────── */

function SlowQueryDistribution({ queries }: { queries: SlowQuery[] }) {
  // Bucket queries by execution time ranges
  const buckets = [
    { range: "<100ms", min: 0, max: 100, count: 0 },
    { range: "100-500ms", min: 100, max: 500, count: 0 },
    { range: "500ms-1s", min: 500, max: 1000, count: 0 },
    { range: "1-5s", min: 1000, max: 5000, count: 0 },
    { range: ">5s", min: 5000, max: Infinity, count: 0 },
  ];

  for (const q of queries) {
    for (const b of buckets) {
      if (q.execution_time_ms >= b.min && q.execution_time_ms < b.max) {
        b.count++;
        break;
      }
    }
  }

  const chartData = buckets
    .filter((b) => b.count > 0)
    .map((b) => ({ range: b.range, count: b.count }));

  if (chartData.length === 0) return null;

  return (
    <StatCard title="Slow Query Distribution">
      <ResponsiveContainer width="100%" height={150}>
        <BarChart data={chartData}>
          <CartesianGrid strokeDasharray="3 3" stroke="#374151" />
          <XAxis dataKey="range" tick={{ fontSize: 10, fill: "#6b7280" }} />
          <YAxis tick={{ fontSize: 10, fill: "#6b7280" }} allowDecimals={false} />
          <Tooltip
            contentStyle={{
              backgroundColor: "#1f2937",
              border: "1px solid #374151",
              borderRadius: "6px",
              fontSize: "12px",
            }}
            labelStyle={{ color: "#9ca3af" }}
          />
          <Bar dataKey="count" fill="#f59e0b" radius={[4, 4, 0, 0]} />
        </BarChart>
      </ResponsiveContainer>
    </StatCard>
  );
}

/* ── Slow Queries Table ─────────────────────────────────────────── */

function SlowQueryTable({ queries }: { queries: SlowQuery[] }) {
  const [sortBy, setSortBy] = useState<"time" | "duration">("duration");
  const [expanded, setExpanded] = useState<Set<number>>(new Set());

  const sorted = [...queries].sort((a, b) =>
    sortBy === "duration"
      ? b.execution_time_ms - a.execution_time_ms
      : b.timestamp.localeCompare(a.timestamp)
  );

  const toggleExpand = (idx: number) => {
    setExpanded((prev) => {
      const next = new Set(prev);
      if (next.has(idx)) next.delete(idx);
      else next.add(idx);
      return next;
    });
  };

  return (
    <StatCard title="Slow Queries">
      {queries.length === 0 ? (
        <div className="text-xs text-gray-600 text-center py-4">
          No slow queries recorded
        </div>
      ) : (
        <>
          <div className="flex gap-2 mb-2">
            <button
              onClick={() => setSortBy("duration")}
              className={`px-2 py-0.5 text-xs rounded ${
                sortBy === "duration"
                  ? "bg-blue-900/50 text-blue-400"
                  : "bg-gray-800 text-gray-500 hover:text-gray-300"
              }`}
            >
              By Duration
            </button>
            <button
              onClick={() => setSortBy("time")}
              className={`px-2 py-0.5 text-xs rounded ${
                sortBy === "time"
                  ? "bg-blue-900/50 text-blue-400"
                  : "bg-gray-800 text-gray-500 hover:text-gray-300"
              }`}
            >
              By Time
            </button>
            <span className="text-xs text-gray-600 ml-auto">
              {queries.length} {queries.length === 1 ? "query" : "queries"}
            </span>
          </div>
          <div className="space-y-1 max-h-64 overflow-auto">
            {sorted.map((q, i) => (
              <div
                key={i}
                className="bg-gray-950 border border-gray-800 rounded px-3 py-2 cursor-pointer hover:border-gray-700"
                onClick={() => toggleExpand(i)}
              >
                <div className="flex items-center gap-2">
                  <span
                    className={`text-xs font-mono shrink-0 ${
                      q.execution_time_ms > 5000
                        ? "text-red-400"
                        : q.execution_time_ms > 1000
                          ? "text-yellow-400"
                          : "text-gray-400"
                    }`}
                  >
                    {formatMs(q.execution_time_ms)}
                  </span>
                  <span className="text-xs text-gray-300 truncate font-mono flex-1">
                    {q.query}
                  </span>
                  <span className="text-xs text-gray-600 shrink-0">{q.database}</span>
                  <span className="text-xs text-gray-600 shrink-0">{q.timestamp}</span>
                </div>
                {expanded.has(i) && (
                  <pre className="mt-2 text-xs text-gray-400 font-mono whitespace-pre-wrap bg-gray-900 rounded p-2">
                    {q.query}
                  </pre>
                )}
              </div>
            ))}
          </div>
        </>
      )}
    </StatCard>
  );
}
